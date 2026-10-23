// main_site_connector_rehearsal — a MOCK-ONLY, CONTROL-ONLY rehearsal of the site-connector
// host that does not exist yet (HG-NET-02C follow-up, assignment A).
//
// WHAT THIS IS
//   The smallest executable that runs the real connector chain end to end, so the deployment
//   shape can be measured instead of guessed: site profile + reach allowlist, camera inventory
//   identity, registry authorization, per-frame consent enforcement, session termination, the
//   control-channel wire, and the media-path decision.
//
// WHAT THIS IS NOT
//   - It opens NO socket and speaks NO camera protocol. Every adapter is `MockCameraAdapter`,
//     so every observation carries SYNTHETIC provenance and no payload exists anywhere.
//   - It is NOT evidence of camera media. There is no media in this program.
//   - It is NOT a 24/7 suitability claim. The resource numbers it prints were taken on a
//     development workstation, which this project has measured as not-always-on.
//
// Run it, read the transcript, check the accounting at the end. Exit code is non-zero if any
// expectation fails, so it is usable as a smoke test rather than a demo that always "passes".

#include "camera/CameraInventory.h"
#include "camera/CameraTypes.h"
#include "camera/ControlChannel.h"
#include "camera/DirectMediaPlanner.h"
#include "camera/MockCameraAdapter.h"
#include "camera/RegistryAuthorizer.h"
#include "camera/SiteConnector.h"
#include "camera/SiteProfile.h"

#include "core/Observation.h"
#include "net/DeviceRegistry.h"

#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::camera;

namespace {

int failures = 0;
int checks = 0;

void expect(bool ok, const std::string& label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

long peak_rss_kb() {
    struct rusage ru {};
    if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
    return ru.ru_maxrss;   // KiB on Linux; a HIGH-WATER mark, so it never decreases
}

// Current resident size. Reading /proc rather than getrusage because ru_maxrss cannot show
// whether the footprint is BOUNDED over many cycles - it only ever grows.
long current_rss_kb() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return -1;
    long total = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &total, &resident);
    std::fclose(f);
    if (n != 2) return -1;
    const long page_kb = static_cast<long>(sysconf(_SC_PAGESIZE)) / 1024;
    return resident * (page_kb > 0 ? page_kb : 4);
}

// Synthetic identities and addresses. Deliberately not any real device: this file is committed.
constexpr const char* kSite = "site.rehearsal";
constexpr const char* kConnector = "connector.rehearsal.01";
constexpr const char* kConnCred = "connector-credential-rehearsal";
constexpr const char* kReach = "10.20.30.0/24";

struct RehearsalCamera {
    const char* mac;
    const char* owner_identity;
    const char* credential;
    LogicalDeviceId id;
    std::string address;
};

HardwareFingerprint fingerprint_of(const char* mac) {
    HardwareFingerprint f{};
    std::string why;
    if (!HardwareFingerprint::normalize(mac, &f, &why)) {
        std::printf("FATAL: bad fingerprint literal %s (%s)\n", mac, why.c_str());
        std::exit(2);
    }
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    long cycles = 1;
    if (argc > 1) {
        cycles = std::strtol(argv[1], nullptr, 10);
        if (cycles < 1) cycles = 1;
    }
    const int64_t t0 = now_ms();
    std::printf("=====================================================================\n");
    std::printf(" Site-connector host rehearsal (MOCK-ONLY, CONTROL-ONLY)\n");
    std::printf(" No socket. No camera. No media. Synthetic provenance throughout.\n");
    std::printf("=====================================================================\n");

    // ---------------------------------------------------------------- site profile
    std::printf("\n== 1. SITE PROFILE (who this connector is, and what it may reach) ==\n");
    SiteProfile profile;
    profile.site_id = kSite;
    profile.connector_device_id = kConnector;
    profile.connector_display_name = "rehearsal connector";
    profile.connector_credential_id = kConnCred;
    Ipv4Cidr reach{};
    expect(Ipv4Cidr::parse(kReach, &reach), "reach allowlist parses");
    profile.allowed_camera_subnets.push_back(reach);
    expect(profile.valid(), "site profile is valid");
    std::printf("    connector identity : %s\n", profile.connector_device_id.c_str());
    std::printf("    reach allowlist    : %s (nothing outside it)\n", kReach);

    // ---------------------------------------------------------------- inventory
    std::printf("\n== 2. CAMERA IDENTITY (inventory, keyed on fingerprint + owner identity) ==\n");
    CameraInventory inventory;
    std::vector<RehearsalCamera> cams = {
        {"00:11:22:33:44:01", "REHEARSALA01", "cred-a", {kSite, "cam.v380.rehearsala01"}, "10.20.30.19"},
        {"00:11:22:33:44:02", "REHEARSALB02", "cred-b", {kSite, "cam.v380.rehearsalb02"}, "10.20.30.20"},
        {"00:11:22:33:44:03", "REHEARSALC03", "cred-c", {kSite, "cam.v380.rehearsalc03"}, "10.20.30.10"},
    };
    for (auto& c : cams) {
        CameraObservation o;
        o.fingerprint = fingerprint_of(c.mac);
        o.address = c.address;
        o.interface_class = InterfaceClass::vendor_protocol_only;
        o.vendor_port_answering = true;
        o.observed_at_ms = t0;
        std::string why;
        expect(inventory.onboard(o, kSite, c.owner_identity, /*authorized=*/true, t0, &why),
               std::string("onboard ") + c.id.device_id);
    }
    expect(inventory.size() == 3, "three distinct logical cameras");

    // ---------------------------------------------------------------- registry
    std::printf("\n== 3. AUTHORIZATION (the existing registry; consent recorded per camera) ==\n");
    net::DeviceRegistry registry{net::DeviceRegistry::defaults()};
    for (const auto& c : cams) {
        net::DeviceRegistry::Device d;
        d.device_id = c.id.device_id;
        d.display_name = "rehearsal camera";
        d.device_type = "camera";
        d.credential_id = c.credential;
        d.paired = true;
        d.has_camera = true;
        d.capabilities = {"camera"};
        std::string why;
        expect(registry.pair(d, &why), std::string("pair ") + c.id.device_id);
    }
    for (const auto& c : cams) {
        registry.set_consent(c.id.device_id, true, "household", "presence", t0);
        registry.set_capture_enabled(c.id.device_id, true);
    }
    RegistryAuthorizer authorizer(registry, "presence");
    for (const auto& c : cams) {
        expect(authorizer.authorize(c.id, c.credential, Capability::video) ==
                   AuthorizationOutcome::allowed,
               std::string("authorized: ") + c.id.device_id);
    }
    expect(authorizer.authorize(cams[1].id, cams[0].credential, Capability::video) ==
               AuthorizationOutcome::credential_mismatch,
           "a credential belonging to another camera is refused");

    // ---------------------------------------------------------------- control channel
    std::printf("\n== 4. CONTROL CHANNEL (the only thing a coordination hub may carry) ==\n");
    std::vector<std::string> wire;
    ControlChannel control;
    control.set_capture(&wire);

    // ---------------------------------------------------------------- adapters + connectors
    std::printf("\n== 5. CONNECTORS (mock adapters; synthetic provenance) ==\n");
    MockScript script;
    script.capabilities.add(Capability::video);
    script.frames_per_read = 1;
    script.frame_bytes = 512;

    // A COUNTING sink with a tiny bounded tail, not an accumulator. An Event carries a JSON
    // payload of roughly 2 KiB, so a consumer that retains every observation grows without
    // bound - measured at ~2.2 KiB per retained event in this rehearsal's soak. The connector
    // itself is bounded; a host must forward to the pipeline/persistence rather than hoard.
    uint64_t events_seen = 0;
    std::vector<Event> last_events;      // bounded tail, for inspection only
    auto sink = [&events_seen, &last_events](const Event& e) {
        ++events_seen;
        if (last_events.size() >= 16) last_events.erase(last_events.begin());
        last_events.push_back(e);
    };

    ConnectorConfig cfg;
    cfg.max_frames_per_pull = 1;
    cfg.telemetry_every_n_pulls = 5;

    std::vector<MockCameraAdapter> adapters;
    std::vector<SiteConnector> connectors;
    adapters.reserve(3);
    connectors.reserve(3);
    for (auto& c : cams) {
        adapters.emplace_back(c.id, std::string("mock ") + c.id.device_id, script);
        adapters.back().set_observed_ip(c.address);
    }
    for (size_t i = 0; i < cams.size(); ++i) {
        connectors.emplace_back(adapters[i], authorizer, cams[i].credential, sink, cfg, &profile,
                                &control);
        expect(connectors.back().register_device() == AuthorizationOutcome::allowed,
               std::string("register ") + cams[i].id.device_id);
        expect(connectors.back().start() == ConnectOutcome::ok, std::string("start ") + cams[i].id.device_id);
        expect(connectors.back().pull_once(t0) == 1, std::string("one frame accounted (mock) ") + cams[i].id.device_id);
    }
    std::printf("    observations emitted so far: %llu (synthetic provenance)\n",
                static_cast<unsigned long long>(events_seen));
    expect(events_seen == 3, "one observation per camera");
    for (const auto& e : last_events) {
        const auto& p = e.get_payload();
        expect(p.contains("provenance") && p["provenance"] == "synthetic",
               "every observation is labelled synthetic");
    }
    // The provenance comes from the adapter CLASS and has no override, so a mock cannot
    // produce a real observation - asserted structurally as well as on the wire.
    for (const auto& a : adapters) {
        expect(a.adapter_class() == AdapterClass::mock, "adapter is a mock (synthetic provenance)");
    }

    // ---------------------------------------------------------------- scope enforcement
    std::printf("\n== 6. REACH ISOLATION (a camera outside the site's allowlist) ==\n");
    MockCameraAdapter outsider(cams[2].id, "mock outsider", script);
    outsider.set_observed_ip("10.99.99.10");
    SiteConnector out_of_scope(outsider, authorizer, cams[2].credential, sink, cfg, &profile, &control);
    expect(out_of_scope.scope_check() == ConnectOutcome::out_of_site_scope,
           "outside the allowlist => out_of_site_scope");
    expect(out_of_scope.start() == ConnectOutcome::out_of_site_scope, "start refused");
    expect(outsider.frames_accounted() == 0, "zero frames pulled from an out-of-scope camera");

    // ---------------------------------------------------------------- DHCP move
    std::printf("\n== 7. ADDRESS CHANGE (identity must not move with the address) ==\n");
    const std::string id_c = inventory.find_by_fingerprint(fingerprint_of(cams[2].mac))->id.canonical();
    CameraObservation moved;
    moved.fingerprint = fingerprint_of(cams[2].mac);
    moved.address = "10.20.30.77";
    moved.interface_class = InterfaceClass::vendor_protocol_only;
    moved.observed_at_ms = t0 + 1000;
    expect(inventory.observe(moved, t0 + 1000, nullptr) == ObservationOutcome::known_moved,
           "same fingerprint at a new address => known_moved");
    expect(inventory.find_by_fingerprint(fingerprint_of(cams[2].mac))->id.canonical() == id_c,
           "logical identity unchanged");
    expect(inventory.size() == 3, "no duplicate record, no phantom camera");
    expect(authorizer.authorize(cams[2].id, cams[2].credential, Capability::video) ==
               AuthorizationOutcome::allowed,
           "authorization unaffected by the address change");

    // ---------------------------------------------------------------- consent + revocation
    std::printf("\n== 8. CONSENT WITHDRAWAL AND REVOCATION (mid-session) ==\n");
    const uint64_t before = events_seen;
    wire.clear();
    registry.set_consent(cams[1].id.device_id, false, "", "", t0 + 2000);
    expect(connectors[1].pull_once(t0 + 2000) == 0, "collection stops on consent withdrawal");
    expect(!connectors[1].session_active(), "session terminated");
    expect(connectors[1].status().stopped_by_authorization, "recorded as an authorization stop");
    expect(events_seen == before, "no observation emitted for a refused device");
    bool saw_consent = false;
    for (const auto& w : wire) {
        ControlMessage m;
        std::string why;
        if (control.decode(w, &m, &why) && m.kind == ControlKind::consent_change) saw_consent = true;
    }
    expect(saw_consent, "control plane is told it was a CONSENT change");

    const bool regranted =
        registry.set_consent(cams[1].id.device_id, true, "household", "presence", t0 + 3000);
    // Deliberate behaviour in the existing registry, demonstrated rather than worked around:
    // a consent WITHDRAWAL also latches the operator's capture switch OFF. Recovering takes
    // TWO explicit acts - re-consent AND re-asserting capture - so a device cannot silently
    // return to collecting merely because consent was restored.
    const bool recaptured = registry.set_capture_enabled(cams[1].id.device_id, true);
    expect(recaptured, "capture re-asserted by the operator (withdrawal had latched it off)");
    const AuthorizationOutcome after_regrant =
        authorizer.authorize(cams[1].id, cams[1].credential, Capability::video);
    expect(regranted, "consent re-recorded");
    expect(after_regrant == AuthorizationOutcome::allowed,
           std::string("authorization restored: ") + authorization_outcome_to_string(after_regrant));
    const ConnectOutcome resume = connectors[1].start();
    expect(resume == ConnectOutcome::ok,
           std::string("collection resumes after re-consent (got ") +
               connect_outcome_to_string(resume) + ", last denial " +
               authorization_outcome_to_string(connectors[1].status().last_denial) + ")");
    wire.clear();
    registry.revoke(cams[1].id.device_id, "rehearsal revocation");
    const size_t revoked_frames = connectors[1].pull_once(t0 + 4000);
    expect(revoked_frames == 0,
           std::string("collection stops on revocation (frames=") + std::to_string(revoked_frames) +
               ", streaming=" + (connectors[1].status().streaming ? "yes" : "no") + ")");
    bool saw_revocation = false;
    for (const auto& w : wire) {
        ControlMessage m;
        std::string why;
        if (control.decode(w, &m, &why) && m.kind == ControlKind::revocation) saw_revocation = true;
    }
    expect(saw_revocation, "control plane is told it was a REVOCATION (not a consent change)");
    expect(connectors[0].session_active(), "an unrelated camera keeps its session");

    std::printf("\n== 9. THE WIRE (every message that travelled; control only) ==\n");
    std::printf("    messages captured: %zu\n", wire.size());
    bool all_bounded = true;
    for (const auto& w : wire) {
        if (w.size() > ControlChannel::kMaxEncodedBytes) all_bounded = false;
        if (w.rfind("HC1|", 0) != 0) all_bounded = false;
    }
    expect(all_bounded, "every captured message is a well-formed control message within the ceiling");
    for (size_t i = 0; i < wire.size() && i < 6; ++i) {
        std::printf("    [%zu] %s\n", i, wire[i].c_str());
    }
    if (wire.size() > 6) std::printf("    ... %zu more\n", wire.size() - 6);

    std::printf("\n== 10. MEDIA PATH DECISION (design only; no media exists in this binary) ==\n");
    DirectMediaPlanner planner;
    MediaPathRequest req;
    req.site_nat = NatClass::symmetric_nat;
    req.viewer_nat = NatClass::unknown;
    const MediaPathDecision decided = planner.plan(req);
    std::printf("    path          : %s\n", media_path_to_string(decided.path));
    std::printf("    reason        : %s\n", decided.reason.c_str());
    std::printf("    site bandwidth: %s\n", decided.uses_site_bandwidth ? "yes" : "no");
    expect(!decided.uses_control_hub, "the coordination hub is NOT a media path");
    const uint64_t ratio =
        DirectMediaPlanner::media_to_control_ratio(ControlChannel::kReferenceFrameBytes);
    std::printf("    one 1080p H.265 keyframe is %llu control messages\n",
                static_cast<unsigned long long>(ratio));
    expect(ratio >= 32, "a frame cannot be expressed on the control channel");

    // ---------------------------------------------------------------- soak
    std::printf("\n== 12. BOUNDED FOOTPRINT UNDER REPETITION (%ld cycle(s)) ==\n", cycles);
    const long rss_first = current_rss_kb();
    long rss_mid = rss_first;
    long rss_max = rss_first;
    for (long i = 0; i < cycles; ++i) {
        // The same two operations a running host performs continuously: re-observe the
        // camera (endpoint + health) and account for a bounded batch of frames.
        CameraObservation o;
        o.fingerprint = fingerprint_of(cams[0].mac);
        o.address = (i % 2 == 0) ? cams[0].address : "10.20.30.19";
        o.interface_class = InterfaceClass::vendor_protocol_only;
        o.observed_at_ms = t0 + 100000 + i;
        (void)inventory.observe(o, o.observed_at_ms, nullptr);
        (void)connectors[0].pull_once(o.observed_at_ms);
        // The sink above counts and keeps a bounded tail, so this loop measures the CONNECTOR's
        // footprint rather than a consumer's retention. Retaining every event instead was
        // measured separately at ~2.2 KiB per event (~44 MiB over these 20000 cycles).
        if (i == cycles / 2) rss_mid = current_rss_kb();
        const long now = current_rss_kb();
        if (now > rss_max) rss_max = now;
    }
    const long rss_last = current_rss_kb();
    const long rss = peak_rss_kb();
    std::printf("    resident after first cycle : %ld KiB\n", rss_first);
    std::printf("    resident at midpoint        : %ld KiB\n", rss_mid);
    std::printf("    resident at end             : %ld KiB\n", rss_last);
    std::printf("    resident high-water         : %ld KiB\n", rss_max);
    std::printf("    growth over %ld cycle(s)   : %ld KiB\n", cycles, rss_last - rss_first);
    expect(rss_last - rss_first < 2048,
           "footprint does not grow with cycles (bounded histories, not an unbounded log)");
    expect(inventory.size() == 3, "still exactly three cameras after the soak");
    expect(inventory.candidates().empty(), "no candidate records accumulated during the soak");

    // ---------------------------------------------------------------- accounting
    std::printf("\n== 11. ACCOUNTING ==\n");
    std::printf("    checks            : %d (%d failed)\n", checks, failures);
    std::printf("    events processed  : %llu\n", static_cast<unsigned long long>(events_seen));
    std::printf("    control messages  : %zu\n", wire.size());
    std::printf("    peak RSS          : %ld KiB (%.2f MiB) on this workstation\n", rss,
                rss > 0 ? static_cast<double>(rss) / 1024.0 : 0.0);
    if (rss > 0) {
        expect(rss < 16384, "peak footprint is under 16 MiB (a small host, not a media server)");
    }
    std::printf("    wall time         : %lld ms\n",
                static_cast<long long>(now_ms() - t0));
    std::printf("\n    NOT MEDIA: no frame was decoded, no audio was sampled, no socket was\n");
    std::printf("    opened, and every adapter was a mock. This is a rehearsal of the\n");
    std::printf("    control and authorization shape, not a camera demonstration.\n");
    std::printf("    NOT 24/7 EVIDENCE: this is a single short run on a development host.\n");

    std::printf("\n== RESULT: %s ==\n", failures == 0 ? "REHEARSAL OK" : "REHEARSAL FAILED");
    return failures == 0 ? 0 : 1;
}
