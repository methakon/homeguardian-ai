// hg_device_ingress: the network-facing device ingestion boundary (milestone D-03).
//
// This binary is the ONLY thing that listens on the network for devices. It:
//   * requires mutual TLS from a private CA (no plaintext path exists),
//   * binds a device certificate to a registry record (identity, not permission),
//   * enforces consent, capture state, allowlists, sequence, timestamps and bounds,
//   * converts accepted observations into the EXISTING Observation -> Event ->
//     Pipeline -> AlertPolicy path and persists alerts to SQLite (WAL +
//     synchronous=FULL, unchanged),
//   * feeds the D-02x detectors (activity change, statistical anomaly, liveness,
//     de-duplication) from the accepted stream,
//   * serves NO dashboard, NO commands and NO SQLite over this port.
//
// Configuration is a JSON file; every field is optional except the three
// certificate paths, and a missing/invalid required value fails closed at startup.
// Nothing in this program logs a key, a certificate body or a token.

#include "core/Application.h"
#include "core/Logger.h"
#include "core/Observation.h"
#include "core/PersistenceAudit.h"
#include "core/Pipeline.h"
#include "core/DetectionFusionRule.h"
#include "core/AlertPolicy.h"
#include "net/DeviceIngressServer.h"
#include "net/DeviceRegistry.h"
#include "net/IngestDetectors.h"
#include "net/ObservationIngest.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace homeguardian;
using namespace homeguardian::net;

namespace {

DeviceIngressServer* g_server = nullptr;

void handle_signal(int) {
    if (g_server != nullptr) g_server->request_stop();  // async-signal-safe flag set
}

struct IngressConfig {
    TlsServerConfig tls;
    std::string registry_path = "devices.json";
    std::string revocation_path = "revocations.json";
    std::string audit_db_path = "homeguardian_ingest_audit.db";
    int retention_days = 30;
    size_t max_pending = 64;
    size_t max_queued_connections = 16;
    size_t max_workers = 2;
    bool verbose = false;
    std::vector<std::string> extra_source_types;
    // The same extension point for the OBSERVATION allowlist. A producer's vocabulary is its own: the Android
    // harness speaks observation_type "motion" over source_type "camera", and an operator must be able to admit
    // those names BY CONFIGURATION rather than by editing the built-in default. Additive only - the defaults
    // stay exactly as they are, so this cannot loosen what no operator asked to accept.
    std::vector<std::string> extra_observation_types;
    // OPT-IN fusion evidence allowlist for observations that arrive over the device
    // boundary. Empty means "keep the built-in conservative default"
    // ({"activity-energy"}), under which ingested sensor observations are recorded
    // and audited but cast NO vote - so they can never raise an alert on their own.
    // Listing the ingested ids (e.g. "sensor.motion") is an explicit operator
    // decision to let that evidence participate in fusion.
    std::vector<std::string> evidence_detector_ids;
    // Durable replay protection: the per-device sequence watermark lives here so a hub
    // restart cannot make an already-accepted observation acceptable again. Defaults
    // to a file beside the audit database; empty disables it (reported loudly).
    std::string replay_state_path;
    // How quickly an authorization change (revocation, consent, capture) takes effect
    // on an established connection. Bounded by the per-observation re-check.
    int authorization_refresh_ms = 250;
    // Isolated-test escape hatch: run with NO durable replay state. Refused for a
    // non-loopback bind, so a deployment cannot end up here.
    bool allow_ephemeral_replay_protection = false;
};

bool load_config(const std::string& path, IngressConfig* config, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("config file not readable: " + path);
    const nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return fail("config is not a JSON object");

    config->tls.certificate_path = parsed.value("certificate_path", std::string());
    config->tls.private_key_path = parsed.value("private_key_path", std::string());
    config->tls.ca_path = parsed.value("ca_path", std::string());
    if (config->tls.certificate_path.empty() || config->tls.private_key_path.empty() ||
        config->tls.ca_path.empty()) {
        return fail("certificate_path, private_key_path and ca_path are all required");
    }
    config->tls.bind_address = parsed.value("bind_address", std::string("0.0.0.0"));
    const int port = parsed.value("port", 8443);
    if (port <= 0 || port > 65535) return fail("port out of range");
    config->tls.port = static_cast<uint16_t>(port);
    config->tls.idle_timeout_ms = parsed.value("idle_timeout_ms", 30000);
    config->tls.handshake_timeout_ms = parsed.value("handshake_timeout_ms", 5000);
    config->tls.max_connections = static_cast<size_t>(parsed.value("max_connections", 8));
    config->tls.max_workers = static_cast<size_t>(parsed.value("max_workers", 2));
    config->registry_path = parsed.value("registry_path", config->registry_path);
    config->revocation_path = parsed.value("revocation_path", config->revocation_path);
    config->authorization_refresh_ms =
        parsed.value("authorization_refresh_ms", config->authorization_refresh_ms);
    config->allow_ephemeral_replay_protection =
        parsed.value("allow_ephemeral_replay_protection", config->allow_ephemeral_replay_protection);
    if (config->authorization_refresh_ms < 0 || config->authorization_refresh_ms > 60000) {
        return fail("authorization_refresh_ms must be between 0 and 60000");
    }
    config->audit_db_path = parsed.value("audit_db_path", config->audit_db_path);
    // Default the durable replay state to a file beside the audit database (parsed
    // ABOVE, so an operator-chosen database path is what decides this), so a deployment
    // gets restart-safe replay protection unless it opts out.
    if (parsed.contains("replay_state_path")) {
        config->replay_state_path = parsed.value("replay_state_path", std::string());
    } else {
        const size_t slash = config->audit_db_path.find_last_of('/');
        const std::string directory =
            slash == std::string::npos ? std::string(".") : config->audit_db_path.substr(0, slash);
        config->replay_state_path = directory + "/device_replay_state.json";
    }
    config->retention_days = parsed.value("alert_retention_days", config->retention_days);
    config->max_pending = static_cast<size_t>(parsed.value("max_pending_observations", 64));
    config->max_queued_connections =
        static_cast<size_t>(parsed.value("max_queued_connections", 16));
    config->max_workers = static_cast<size_t>(parsed.value("max_workers", 2));
    config->verbose = parsed.value("verbose", false);
    if (parsed.contains("evidence_detector_ids")) {
        if (!parsed["evidence_detector_ids"].is_array()) {
            return fail("evidence_detector_ids must be an array");
        }
        for (const auto& entry : parsed["evidence_detector_ids"]) {
            if (!entry.is_string()) return fail("evidence_detector_ids entries must be strings");
            const std::string id = entry.get<std::string>();
            if (id.empty() || id.size() > 64) {
                return fail("evidence_detector_ids entries must be 1..64 characters");
            }
            config->evidence_detector_ids.push_back(id);
        }
    }
    if (parsed.contains("extra_observation_types")) {
        if (!parsed["extra_observation_types"].is_array()) {
            return fail("extra_observation_types must be an array");
        }
        for (const auto& entry : parsed["extra_observation_types"]) {
            if (!entry.is_string()) return fail("extra_observation_types entries must be strings");
            const std::string name = entry.get<std::string>();
            if (name.empty() || name.size() > 64) {
                return fail("extra_observation_types entries must be 1..64 characters");
            }
            config->extra_observation_types.push_back(name);
        }
    }
    if (parsed.contains("extra_source_types")) {
        if (!parsed["extra_source_types"].is_array()) return fail("extra_source_types must be an array");
        for (const auto& entry : parsed["extra_source_types"]) {
            if (!entry.is_string()) return fail("extra_source_types entries must be strings");
            config->extra_source_types.push_back(entry.get<std::string>());
        }
    }
    // Bounds on the bounds: a config must not be able to disable the limits.
    if (config->tls.max_connections == 0 || config->tls.max_connections > 64) {
        return fail("max_connections must be between 1 and 64");
    }
    if (config->max_workers == 0 || config->max_workers > 8) return fail("max_workers must be 1..8");
    if (config->max_pending == 0 || config->max_pending > 1024) {
        return fail("max_pending_observations must be 1..1024");
    }
    if (error != nullptr) error->clear();
    return true;
}

void usage() {
    std::cerr << "usage: hg_device_ingress --config <ingress.json> [--verbose]\n";
    std::cerr << "  required in the config: certificate_path, private_key_path, ca_path\n";
    std::cerr << "  optional: bind_address (default 0.0.0.0), port (8443), registry_path,\n";
    std::cerr << "            audit_db_path, alert_retention_days, max_connections,\n";
    std::cerr << "            evidence_detector_ids (opt-in fusion allowlist),\n";
    std::cerr << "            max_queued_connections, max_workers, max_pending_observations,\n";
    std::cerr << "            idle_timeout_ms, handshake_timeout_ms, verbose\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage();
            return 2;
        }
    }
    if (config_path.empty()) {
        usage();
        return 2;
    }

    IngressConfig config;
    std::string error;
    if (!load_config(config_path, &config, &error)) {
        std::cerr << "configuration rejected (fail-closed): " << error << "\n";
        return 3;
    }
    if (verbose) config.verbose = true;

    // Logger::initialize returns void: a logger that cannot reach its file must not
    // take the security boundary down with it, so there is nothing to branch on
    // here (this is how the bridge does it too).
    Logger::initialize("warn");

    // Everything below touches the filesystem (registry, SQLite audit store) and
    // the network. A failure there must be a REFUSAL, not a crash: an uncaught
    // DatabaseException escaped to std::terminate and killed the boundary with
    // SIGABRT when its audit path was not writable (e.g. a read-only working
    // directory), which is the opposite of failing closed. Any exception here is
    // converted into a clean refusal with a reason.
    try {

    // ---- registry ----
    DeviceRegistry registry;
    {
        std::ifstream probe(config.registry_path);
        if (probe.good()) {
            std::string why;
            if (!registry.load(config.registry_path, &why)) {
                std::cerr << "device registry rejected (fail-closed): " << why << "\n";
                return 3;
            }
        } else {
            std::cerr << "device registry not found at " << config.registry_path
                      << "; starting EMPTY and refusing every device until paired\n";
        }
    }

    // ---- pipeline + policy + persistence (the existing machinery) ----
    auto detection_rule = std::make_shared<DetectionFusionRule>("detection-fusion");
    if (!config.evidence_detector_ids.empty()) {
        // NOTE: an EMPTY allowlist means "accept every detector" in the rule, so a
        // blank config must not be passed through - absence of a decision must not
        // become the widest possible permission.
        detection_rule->set_evidence_detector_ids(config.evidence_detector_ids);
        std::cout << "hg_device_ingress: fusion evidence allowlist = ";
        for (size_t i = 0; i < config.evidence_detector_ids.size(); ++i) {
            std::cout << (i ? "," : "") << config.evidence_detector_ids[i];
        }
        std::cout << " (operator opt-in; default is conservative)" << std::endl;
    }
    std::vector<std::shared_ptr<ICorrelationRule>> rules{detection_rule};
    auto pipeline = std::make_shared<Pipeline>(rules);
    AlertPolicy policy(std::chrono::milliseconds(60000));
    auto audit = std::make_unique<PersistenceAudit>(config.audit_db_path);
    if (config.retention_days > 0) {
        (void)audit->prune_older_than_age(std::chrono::hours(24) * config.retention_days);
    }

    // Ingestion + per-device detectors.
    ObservationIngest::Config ingest_config;
    ingest_config.max_pending = config.max_pending;
    for (const std::string& extra : config.extra_source_types) {
        ingest_config.allowed_source_types.push_back(extra);
    }
    for (const std::string& extra : config.extra_observation_types) {
        ingest_config.allowed_observation_types.push_back(extra);
    }
    ObservationIngest ingest(registry, ingest_config);
    IngestDetectors detectors(IngestDetectors::defaults());

    // The ONE place an accepted observation becomes an event.
    std::atomic<uint64_t> alerts_persisted{0};
    const auto deliver = [&](const SecurityObservation& observation) {
        const Event event = observation.to_observation().to_event();
        const auto alert = pipeline->process(event);
        if (!alert.has_value()) return;
        bool suppressed = false;
        if (!policy.admit(alert.value(), suppressed)) return;
        if (audit->record_alert(alert.value())) alerts_persisted.fetch_add(1);
    };
    const auto deliver_derived = [&](const Observation& observation) {
        const Event event = observation.to_event();
        const auto alert = pipeline->process(event);
        if (!alert.has_value()) return;
        bool suppressed = false;
        if (!policy.admit(alert.value(), suppressed)) return;
        if (audit->record_alert(alert.value())) alerts_persisted.fetch_add(1);
    };

    DeviceIngressServer::Config server_config;
    server_config.tls = config.tls;
    server_config.max_queued_connections = config.max_queued_connections;
    server_config.max_workers = config.max_workers;
    server_config.verbose = config.verbose;
    server_config.replay_state_path = config.replay_state_path;
    server_config.allow_ephemeral_replay_protection = config.allow_ephemeral_replay_protection;
    server_config.authorization_refresh_ms = config.authorization_refresh_ms;

    DeviceIngressServer server(server_config, registry, ingest, detectors,
                               DeviceIngressServer::Sinks{deliver, deliver_derived},
                               config.audit_db_path, config.revocation_path, config.registry_path);
    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (!server.start(&error)) {
        std::cerr << "ingress failed to start (fail-closed): " << error << "\n";
        return 4;
    }

    std::cout << "hg_device_ingress ready: bind=" << config.tls.bind_address
              << " port=" << server.port() << " registry_devices=" << registry.device_count()
              << " paired=" << registry.paired_count() << " tls=mutual(min 1.2)" << std::endl;

    server.run();

    std::cout << "hg_device_ingress stopping: accepted=" << server.stats().messages_accepted
              << " rejected=" << server.stats().messages_rejected
              << " alerts_persisted=" << alerts_persisted.load() << std::endl;
    audit->close();
    return 0;

    } catch (const std::exception& e) {
        std::cerr << "ingress failed to start (fail-closed): " << e.what() << "\n";
        return 5;
    } catch (...) {
        std::cerr << "ingress failed to start (fail-closed): unknown error\n";
        return 5;
    }
}
