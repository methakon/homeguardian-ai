// hg_mcp_server — a Streamable HTTP MCP server over the EXISTING read-only HomeGuardian
// contract.
//
// It does not reimplement application state, credentials or persistence: it builds the same
// Application, PersistenceAudit and AlexaCredentialStore the NDJSON bridge builds, and
// exposes them through two read-only MCP tools. Authorization is the contract's, not a
// second opinion.
//
// Security posture, stated so it can be checked rather than assumed:
//   * authentication is REQUIRED and the credential store must load securely (owner-only
//     0600 file, owned by the effective uid). A load failure exits non-zero: there is no
//     permissive fallback and no unauthenticated serving.
//   * the listener binds loopback unless HG_MCP_ALLOW_REMOTE is set explicitly.
//   * the Origin allowlist defaults to the loopback origins; an unlisted Origin is refused.
//   * logs go to stderr, so no protocol stream is polluted.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>  // declares stderr_color_sink_mt too
#include <spdlog/spdlog.h>

#include "core/AlexaContract.h"
#include "core/AlexaCredentialStore.h"
#include "core/Alert.h"
#include "core/Application.h"
#include "core/Config.h"
#include "core/PersistenceAudit.h"
#include "mcp/McpHttpServer.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpTools.h"

namespace {

std::atomic<bool> g_stop{false};

// Async-signal-safe: set a flag and nothing else. serve() polls it every 200 ms.
extern "C" void on_signal(int) { g_stop.store(true); }

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

long env_long(const char* name, long fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    try {
        return std::stol(v);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Mirrors the bridge's AppStatusProvider exactly in semantics: a read-only view over the
// real Application + PersistenceAudit, unavailable when persistence is unhealthy. It is
// duplicated rather than shared because the bridge's copy is file-local and extracting it
// would mean editing code that is already verified; the copy is pinned by the MCP tests
// against the same contract.
class McpStatusProvider : public homeguardian::IAlexaStatusProvider {
public:
    McpStatusProvider(bool capture_enabled, homeguardian::PersistenceAudit* audit)
        : capture_enabled_(capture_enabled), audit_(audit) {}

    homeguardian::AlexaStatusSnapshot snapshot() const override {
        homeguardian::AlexaStatusSnapshot s;
        s.state = "Running";
        s.capture_enabled = capture_enabled_;
        s.available = (audit_ != nullptr) && audit_->healthy();
        if (audit_ != nullptr) {
            const auto recent =
                audit_->alerts().find_recent(homeguardian::ALEXA_MAX_ALERTS_RETURNED);
            s.alert_count = audit_->total_alert_count();
            for (const auto& a : recent) s.alert_ids.push_back(a.alert_id);
            if (!recent.empty()) {
                homeguardian::Severity top = homeguardian::Severity::info;
                for (const auto& a : recent) {
                    if (a.severity == homeguardian::Severity::critical) {
                        top = homeguardian::Severity::critical;
                        break;
                    }
                    if (a.severity == homeguardian::Severity::warning) {
                        top = homeguardian::Severity::warning;
                    }
                }
                s.highest_severity = top == homeguardian::Severity::critical  ? "critical"
                                     : top == homeguardian::Severity::warning ? "warning"
                                                                              : "info";
            }
        }
        return s;
    }

private:
    bool capture_enabled_ = false;
    homeguardian::PersistenceAudit* audit_ = nullptr;
};

}  // namespace

int main(int argc, char* argv[]) {
    // stderr sink: stdout stays clean, because a machine-readable stream must never be
    // interleaved with logs.
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("hg_mcp", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);

    const std::string config_path = argc > 1 ? argv[1] : "config/homeguardian.demo.json";
    const std::string creds_path = argc > 2 ? argv[2] : "config/mcp_credentials.json";

    homeguardian::mcp::McpHttpConfig cfg;
    const long port = env_long("HG_MCP_PORT", 8790);
    cfg.port = static_cast<uint16_t>(port < 1 || port > 65535 ? 8790 : port);
    cfg.bind_address = env_or("HG_MCP_BIND", "127.0.0.1");
    cfg.allow_remote = std::getenv("HG_MCP_ALLOW_REMOTE") != nullptr;
    const std::vector<std::string> extra_origins = split_csv(env_or("HG_MCP_ORIGINS", ""));
    cfg.allowed_origins = extra_origins;
    cfg.require_auth = true;

    // ---- real application state (read-only view) ------------------------------------
    bool capture_enabled = false;  // fail-safe default
    try {
        homeguardian::Config conf;
        conf.load(config_path);
        capture_enabled = conf.media_capture_enabled;
    } catch (const std::exception&) {
        capture_enabled = false;  // never imply capture is on
    }

    homeguardian::Application app;
    app.get_status().capture_enabled.store(capture_enabled);

    homeguardian::PersistenceAudit* audit = nullptr;
    std::unique_ptr<homeguardian::PersistenceAudit> audit_owner;
    try {
        audit_owner = std::make_unique<homeguardian::PersistenceAudit>(
            env_or("HG_MCP_AUDIT_DB", "homeguardian_mcp_audit.db"));
        audit = audit_owner.get();
    } catch (const std::exception&) {
        audit = nullptr;  // degraded => the status tool reports the app unavailable
    }

    // ---- credentials: secure store only, fail closed -------------------------------
    std::unique_ptr<homeguardian::AlexaCredentialStore> store;
    try {
        store = std::make_unique<homeguardian::AlexaCredentialStore>();
        const auto load_res = store->load(creds_path);
        if (!load_res.ok) {
            std::cerr << "mcp: credential store failed to load securely: " << load_res.reason
                      << "\n";
            return 3;
        }
    } catch (const std::exception&) {
        // Generic: never echo the path or file content. No fallback, no unauthenticated serve.
        std::cerr << "mcp: credential load failed (fail-closed, no fallback)\n";
        return 3;
    }

    McpStatusProvider provider(capture_enabled, audit);
    const homeguardian::mcp::ToolRegistry tools =
        homeguardian::mcp::make_homeguardian_tools(*store, provider);

    homeguardian::mcp::McpHttpServer server(cfg, tools, *store);
    std::string error;
    if (!server.start(error)) {
        std::cerr << "mcp: startup refused: " << error << "\n";
        return 4;
    }

    // The Origin allowlist is derived from the bind address and port actually in use.
    // It is applied by the transport on every request that carries an Origin header.
    if (cfg.allowed_origins.empty()) {
        const std::string base = "http://" + cfg.bind_address + ":" + std::to_string(server.port());
        cfg.allowed_origins = {base, "http://localhost:" + std::to_string(server.port()),
                               "http://127.0.0.1:" + std::to_string(server.port())};
        server.set_allowed_origins(cfg.allowed_origins);
    }

    spdlog::info("hg_mcp_server listening on {}:{} (loopback={}, auth=required, tools={})",
                 cfg.bind_address, server.port(), !cfg.allow_remote, tools.size());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    server.serve(g_stop);

    const auto s = server.stats();
    spdlog::info("mcp: requests={} rejected_origin={} rejected_auth={} oversize={} shed={} errors={}",
                 s.requests, s.rejected_origin, s.rejected_auth, s.rejected_oversize,
                 s.rejected_too_many, s.errors);
    return 0;
}
