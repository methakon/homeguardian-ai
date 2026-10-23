#include "catch2/catch.hpp"

#include "camera/CameraInventory.h"
#include "camera/DeviceFingerprint.h"
#include "camera/MockCameraAdapter.h"
#include "camera/RegistryAuthorizer.h"
#include "camera/SiteConnector.h"
#include "camera/SiteProfile.h"

#include "core/Logger.h"
#include "net/DeviceRegistry.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// HG-CAM-10 / Issue #19 - durable camera identity, DHCP churn and onboarding.
//
// Every fingerprint below is SYNTHETIC. The owner's real camera MAC addresses, LAN
// addresses and device identifiers are deliberately absent from this repository: they live
// only in the owner-restricted evidence file. A MAC identifies one specific unit, so
// committing one would publish a device identifier.
//
// THE QUESTION THESE TESTS ANSWER
//   Three observations arrive from a read-only LAN pass. Which one is a camera that moved,
//   and which one is a camera nobody had seen before? Getting that wrong either invents a
//   phantom camera or silently lends one camera's consent to a different unit.
// ---------------------------------------------------------------------------

using namespace homeguardian;
using namespace homeguardian::camera;

namespace {

constexpr const char* kSite = "site.home.dev";

// One vendor class (00:11:22), three distinct units, plus a second vendor and a phone.
constexpr const char* kCamA = "00:11:22:33:44:01";   // registered, does not move
constexpr const char* kCamB = "00:11:22:33:44:02";   // registered, DHCP lease moves
constexpr const char* kCamC = "00:11:22:33:44:03";   // genuinely new (the third camera)
constexpr const char* kPhone = "02:11:22:33:44:05";  // locally administered (randomized)
constexpr const char* kOtherVendor = "00:aa:bb:cc:dd:01";

HardwareFingerprint fp(const char* raw) {
    HardwareFingerprint f{};
    std::string why;
    REQUIRE(HardwareFingerprint::normalize(raw, &f, &why));
    return f;
}

CameraObservation make_obs(const char* fingerprint, const std::string& address, int64_t t,
                           InterfaceClass ic = InterfaceClass::vendor_protocol_only) {
    CameraObservation o;
    o.fingerprint = fp(fingerprint);
    o.address = address;
    o.interface_class = ic;
    o.vendor_port_answering = true;
    o.observed_at_ms = t;
    return o;
}

// The three-camera trust chain used by the integration test: inventory identity ->
// registry authorization -> site connector enforcement.
net::DeviceRegistry::Device registry_device(const std::string& id, const std::string& cred) {
    net::DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "v380 camera";
    d.device_type = "camera";
    d.credential_id = cred;
    d.paired = true;
    d.has_camera = true;
    d.capabilities = {"camera"};
    return d;
}

}  // namespace

// ---------------------------------------------------------------- fingerprint

TEST_CASE("HardwareFingerprint: an address is never a device identity",
          "[inventory][fingerprint]") {
    Logger::initialize("error");

    HardwareFingerprint f{};
    std::string why;

    // The three on-wire forms all normalize to one canonical value.
    REQUIRE(HardwareFingerprint::normalize("00:11:22:33:44:01", &f, &why));
    REQUIRE(f.canonical() == "00:11:22:33:44:01");
    REQUIRE(HardwareFingerprint::normalize("00-11-22-33-44-01", &f, &why));
    REQUIRE(f.canonical() == "00:11:22:33:44:01");
    REQUIRE(HardwareFingerprint::normalize("001122334401", &f, &why));
    REQUIRE(f.canonical() == "00:11:22:33:44:01");
    REQUIRE(HardwareFingerprint::normalize("00:11:22:33:44:0A", &f, &why));
    REQUIRE(f.canonical() == "00:11:22:33:44:0a");   // case-insensitive, one canonical form
    REQUIRE(f.oui() == "00:11:22");
    REQUIRE(f.durable());

    // An IPv4 literal is refused with the reason that explains WHY it cannot be identity.
    REQUIRE_FALSE(HardwareFingerprint::normalize("10.20.30.40", &f, &why));
    REQUIRE(why.find("not a device identity") != std::string::npos);
    REQUIRE_FALSE(HardwareFingerprint::normalize("10.0.0.5", &f, &why));

    // Placeholders a scan reports when it learned nothing are not devices.
    REQUIRE_FALSE(HardwareFingerprint::normalize("00:00:00:00:00:00", &f, &why));
    REQUIRE_FALSE(HardwareFingerprint::normalize("ff:ff:ff:ff:ff:ff", &f, &why));

    // Not a MAC: an SSID, a timestamp, a truncated address.
    REQUIRE_FALSE(HardwareFingerprint::normalize("HomeWiFi_5G", &f, &why));
    REQUIRE_FALSE(HardwareFingerprint::normalize("2026-10-18T02:07:00", &f, &why));
    REQUIRE_FALSE(HardwareFingerprint::normalize("00:11:22:33:44", &f, &why));
    REQUIRE_FALSE(HardwareFingerprint::normalize("", &f, &why));

    // A randomized MAC is a WEAK identity and reports itself as one.
    REQUIRE(HardwareFingerprint::normalize(kPhone, &f, &why));
    REQUIRE(f.valid());
    REQUIRE(f.locally_administered());
    REQUIRE_FALSE(f.durable());     // it may be a different value next time

    // Vendor family: shares an OUI, still a different unit.
    REQUIRE(fp(kCamA).same_vendor(fp(kCamB)));
    REQUIRE_FALSE(fp(kCamA) == fp(kCamB));
    REQUIRE_FALSE(fp(kCamA).same_vendor(fp(kOtherVendor)));
}

// ---------------------------------------------------------------- DHCP churn

TEST_CASE("HG-CAM-10: a moved camera keeps its identity; a new unit does not become one",
          "[inventory][dhcp][churn]") {
    Logger::initialize("error");
    CameraInventory inv;

    // Two cameras are already registered. Registration is keyed by the owner's identity,
    // never by the address.
    REQUIRE(inv.onboard(make_obs(kCamA, "10.0.0.19", 1000), kSite, "CAMUNIT0001", true, 1000, nullptr));
    REQUIRE(inv.onboard(make_obs(kCamB, "10.0.0.20", 1000), kSite, "CAMUNIT0002", true, 1000, nullptr));
    REQUIRE(inv.size() == 2);

    const LogicalDeviceId id_a = inv.find_by_fingerprint(fp(kCamA))->id;
    const LogicalDeviceId id_b = inv.find_by_fingerprint(fp(kCamB))->id;
    const std::string moved_from = inv.find_by_fingerprint(fp(kCamB))->last_address;
    REQUIRE(moved_from == "10.0.0.20");

    // THIS is the pass that used to be ambiguous: one camera stayed, one camera's lease
    // moved, and one device had never been seen.
    std::vector<CameraObservation> pass{
        make_obs(kCamA, "10.0.0.19", 5000),
        make_obs(kCamB, "10.0.0.10", 5000),      // same unit, new address
        make_obs(kCamC, "10.0.0.20", 5000),      // new unit, RECYCLED old address
    };
    const PassReport report = inv.reconcile_pass(pass, 5000);

    REQUIRE(report.consistent);
    REQUIRE(report.outcomes.size() == 3);
    REQUIRE(report.outcomes[0] == ObservationOutcome::known_at_same_address);
    REQUIRE(report.outcomes[1] == ObservationOutcome::known_moved);
    REQUIRE(report.outcomes[2] == ObservationOutcome::new_distinct_device);
    REQUIRE(report.known_same_address == 1);
    REQUIRE(report.known_moved == 1);
    REQUIRE(report.new_devices == 1);
    REQUIRE(report.unusable == 0);

    // The moved camera: identity and authorization unchanged, address is only an attachment.
    const CameraRecord* b = inv.find_by_fingerprint(fp(kCamB));
    REQUIRE(b != nullptr);
    REQUIRE(b->id.canonical() == id_b.canonical());
    REQUIRE(b->authorized);                       // NOT inherited by, nor lost to, the new unit
    REQUIRE(b->last_address == "10.0.0.10");
    REQUIRE(b->address_changes == 1);
    REQUIRE(b->address_history.size() == 1);
    REQUIRE(b->address_history[0] == "10.0.0.20");
    REQUIRE(b->health == HealthState::online);

    // The unmoved camera is untouched.
    const CameraRecord* a = inv.find_by_fingerprint(fp(kCamA));
    REQUIRE(a->id.canonical() == id_a.canonical());
    REQUIRE(a->address_changes == 0);
    REQUIRE(a->last_address == "10.0.0.19");

    // The third camera is a CANDIDATE, not a registered device: no consent, no id, no
    // authorization was invented for it.
    REQUIRE(inv.size() == 2);
    REQUIRE(inv.find_by_fingerprint(fp(kCamC)) == nullptr);
    REQUIRE(inv.candidates().size() == 1);
    REQUIRE(inv.candidates()[0].fingerprint == fp(kCamC));
    REQUIRE(inv.candidates()[0].last_address == "10.0.0.20");
    REQUIRE(report.summary.find("1 distinct and unregistered") != std::string::npos);
}

// ---------------------------------------------------------------- conflicts

TEST_CASE("An inconsistent pass is not evidence: conflicts are refused, nothing is applied",
          "[inventory][conflict]") {
    Logger::initialize("error");
    CameraInventory inv;
    REQUIRE(inv.onboard(make_obs(kCamA, "10.0.0.19", 1000), kSite, "CAMUNIT0001", true, 1000, nullptr));

    SECTION("one NIC reported at two addresses") {
        std::vector<CameraObservation> pass{
            make_obs(kCamA, "10.0.0.19", 5000),
            make_obs(kCamA, "10.0.0.77", 5000),
        };
        const PassReport r = inv.reconcile_pass(pass, 5000);
        REQUIRE_FALSE(r.consistent);
        REQUIRE(r.conflicts.size() == 1);
        REQUIRE(r.outcomes[0] == ObservationOutcome::conflicting_fingerprint);
        REQUIRE(r.outcomes[1] == ObservationOutcome::conflicting_fingerprint);
        // Nothing was applied: the recorded address is still the last confirmed one.
        REQUIRE(inv.find_by_fingerprint(fp(kCamA))->last_address == "10.0.0.19");
        REQUIRE(inv.find_by_fingerprint(fp(kCamA))->address_changes == 0);
    }

    SECTION("one address claimed by two NICs") {
        std::vector<CameraObservation> pass{
            make_obs(kCamB, "10.0.0.55", 5000),
            make_obs(kCamC, "10.0.0.55", 5000),
        };
        const PassReport r = inv.reconcile_pass(pass, 5000);
        REQUIRE_FALSE(r.consistent);
        REQUIRE(r.conflicts.size() == 1);
        REQUIRE(r.conflicts[0].find("claimed by 2 different NIC") != std::string::npos);
        REQUIRE(inv.size() == 1);
        REQUIRE(inv.candidates().empty());
    }

    SECTION("an empty pass is consistent, and is NOT evidence that cameras are absent") {
        const PassReport r = inv.reconcile_pass({}, 5000);
        REQUIRE(r.consistent);
        REQUIRE(r.new_devices == 0);
        REQUIRE(r.known_moved == 0);
        REQUIRE(inv.size() == 1);          // the registered camera is untouched by silence
    }
}

// ---------------------------------------------------------------- onboarding gates

TEST_CASE("Registration requires a stable owner-provided identity and explicit authorization",
          "[inventory][onboarding]") {
    Logger::initialize("error");
    CameraInventory inv;
    std::string why;

    // No owner-provided identity: an address alone may not become a device.
    REQUIRE_FALSE(inv.onboard(make_obs(kCamC, "10.0.0.10", 1000), kSite, "", true, 1000, &why));
    REQUIRE(why.find("owner-provided") != std::string::npos);

    // An IP address is not an identity, even when the owner hands one over as one.
    REQUIRE_FALSE(inv.onboard(make_obs(kCamC, "10.0.0.10", 1000), kSite, "10.0.0.10", true, 1000, &why));
    REQUIRE(why.find("IP address") != std::string::npos);

    // Seen, but not authorized: stays a candidate.
    REQUIRE_FALSE(inv.onboard(make_obs(kCamC, "10.0.0.10", 1000), kSite, "CAMUNIT0003", false, 1000, &why));
    REQUIRE(why.find("explicitly authorized") != std::string::npos);

    // A randomized MAC cannot ground a durable identity, however willing the owner is.
    REQUIRE_FALSE(inv.onboard(make_obs(kPhone, "10.0.0.31", 1000), kSite, "PHONE01", true, 1000, &why));
    REQUIRE(why.find("randomized") != std::string::npos);

    // All gates satisfied.
    REQUIRE(inv.onboard(make_obs(kCamC, "10.0.0.10", 1000), kSite, "CAMUNIT0003", true, 1000, &why));
    const CameraRecord* c = inv.find_by_fingerprint(fp(kCamC));
    REQUIRE(c != nullptr);
    REQUIRE(c->site_id == kSite);
    REQUIRE(c->authorized);
    REQUIRE(c->id.device_id == "cam.v380.camunit0003");   // derived from the OWNER's identity
    REQUIRE_FALSE(LogicalDeviceId::is_ipv4_literal(c->id.device_id));
    REQUIRE(c->last_address == "10.0.0.10");           // recorded, but not part of the id

    // Registering the same NIC twice is refused rather than duplicated.
    REQUIRE_FALSE(inv.onboard(make_obs(kCamC, "10.0.0.99", 2000), kSite, "OTHER", true, 2000, &why));
    REQUIRE(why.find("already registered") != std::string::npos);
    REQUIRE(inv.size() == 1);

    // An address already attributed to a different registered device is not ours to take.
    REQUIRE_FALSE(inv.onboard(make_obs(kCamB, "10.0.0.10", 3000), kSite, "B02", true, 3000, &why));
    REQUIRE(why.find("already attributed") != std::string::npos);
}

// ---------------------------------------------------------------- evidence rules

TEST_CASE("Media readiness is never raised by a scan, and firmware is never inferred",
          "[inventory][evidence]") {
    Logger::initialize("error");
    CameraInventory inv;
    REQUIRE(inv.onboard(make_obs(kCamC, "10.0.0.10", 1000), kSite, "CAMUNIT0003", true, 1000, nullptr));
    const LogicalDeviceId id = inv.find_by_fingerprint(fp(kCamC))->id;

    // A fresh record claims nothing.
    REQUIRE(inv.find(id)->media == MediaStatus::unknown);
    REQUIRE(inv.find(id)->firmware_model.empty());
    REQUIRE_FALSE(inv.find(id)->firmware_verified);

    // A scan that merely sees a standard port answering does NOT upgrade readiness: only an
    // authenticated session could, and this layer cannot observe one.
    std::vector<CameraObservation> pass{
        make_obs(kCamC, "10.0.0.10", 2000, InterfaceClass::standard_media_answering)};
    REQUIRE(inv.reconcile_pass(pass, 2000).consistent);
    REQUIRE(inv.find(id)->interface_class == InterfaceClass::standard_media_answering);
    REQUIRE(inv.find(id)->media == MediaStatus::unknown);

    // Only an evidence-backed call moves it, and the vocabulary cannot express "working".
    REQUIRE(inv.set_media_status(id, MediaStatus::vendor_protocol_only, nullptr));
    REQUIRE(inv.find(id)->media == MediaStatus::vendor_protocol_only);
    REQUIRE(std::string(media_status_to_string(MediaStatus::vendor_protocol_only)) != "working");

    // An unverified firmware report is NOT a firmware version, and is discarded.
    REQUIRE(inv.note_firmware(id, "AppDEMO_V1_V2.0.0.1_20240101", /*verified=*/false, nullptr));
    REQUIRE(inv.find(id)->firmware_model.empty());
    REQUIRE_FALSE(inv.find(id)->firmware_verified);

    // A verified read-out is recorded verbatim.
    REQUIRE(inv.note_firmware(id, "Hw_DEMOBOARD_XHR_E_V1.0_WF_20240101", /*verified=*/true, nullptr));
    REQUIRE(inv.find(id)->firmware_model == "Hw_DEMOBOARD_XHR_E_V1.0_WF_20240101");
    REQUIRE(inv.find(id)->firmware_verified);
}

// ---------------------------------------------------------------- health

TEST_CASE("Health: a camera goes offline after silence and recovers with the SAME identity",
          "[inventory][health][offline]") {
    Logger::initialize("error");
    CameraInventory::Config cfg;
    cfg.offline_after_ms = 1000;
    CameraInventory inv(cfg);
    REQUIRE(inv.onboard(make_obs(kCamB, "10.0.0.20", 0), kSite, "CAMUNIT0002", true, 0, nullptr));
    REQUIRE(inv.size() == 1);
    const std::string id = inv.find_by_fingerprint(fp(kCamB))->id.canonical();

    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->health == HealthState::online);

    // Silence past the timeout: offline. The record is NOT deleted - an offline camera is
    // still a consented, registered camera.
    REQUIRE(inv.sweep_offline(5000) == 1);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->health == HealthState::offline);
    REQUIRE(inv.size() == 1);

    // It comes back at the SAME address...
    REQUIRE(inv.observe(make_obs(kCamB, "10.0.0.20", 6000), 6000, nullptr) ==
            ObservationOutcome::known_at_same_address);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->health == HealthState::online);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->address_changes == 0);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->id.canonical() == id);

    // ...and then at a NEW one after a router restart: recovery plus churn, identity intact.
    REQUIRE(inv.observe(make_obs(kCamB, "10.0.0.33", 7000), 7000, nullptr) ==
            ObservationOutcome::known_moved);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->id.canonical() == id);
    REQUIRE(inv.find_by_fingerprint(fp(kCamB))->address_changes == 1);

    // A randomized MAC is refused on the single-observation path too.
    std::string why;
    REQUIRE(inv.observe(make_obs(kPhone, "10.0.0.31", 8000), 8000, &why) ==
            ObservationOutcome::unusable_fingerprint);
    REQUIRE_FALSE(why.empty());
}

// ---------------------------------------------------------------- durability

TEST_CASE("Identity bindings survive a restart: a moved camera is still known afterwards",
          "[inventory][persistence]") {
    Logger::initialize("error");
    const std::string path = "hg_inventory_test.txt";
    std::string why;

    CameraInventory inv;
    REQUIRE(inv.onboard(make_obs(kCamA, "10.0.0.19", 1000), kSite, "CAMUNIT0001", true, 1000, nullptr));
    REQUIRE(inv.onboard(make_obs(kCamB, "10.0.0.20", 1000), kSite, "CAMUNIT0002", true, 1000, nullptr));
    const LogicalDeviceId id_b = inv.find_by_fingerprint(fp(kCamB))->id;
    REQUIRE(inv.set_media_status(id_b, MediaStatus::vendor_protocol_only, nullptr));
    REQUIRE(inv.note_firmware(id_b, "AppDEMO_V1_V2.0.0.1_20240101", true, nullptr));
    REQUIRE(inv.observe(make_obs(kCamB, "10.0.0.10", 2000), 2000, nullptr) ==
            ObservationOutcome::known_moved);
    REQUIRE(inv.save(path, &why));

    // A fresh process.
    CameraInventory reloaded;
    REQUIRE(reloaded.load(path, &why));
    REQUIRE(reloaded.size() == 2);
    const CameraRecord* b = reloaded.find_by_fingerprint(fp(kCamB));
    REQUIRE(b != nullptr);
    REQUIRE(b->id.canonical() == id_b.canonical());
    REQUIRE(b->authorized);
    REQUIRE(b->last_address == "10.0.0.10");
    REQUIRE(b->address_changes == 1);
    REQUIRE(b->address_history.size() == 1);
    REQUIRE(b->address_history[0] == "10.0.0.20");
    REQUIRE(b->media == MediaStatus::vendor_protocol_only);
    REQUIRE(b->firmware_model == "AppDEMO_V1_V2.0.0.1_20240101");
    REQUIRE(b->firmware_verified);

    // The binding works: another move is still KNOWN, not a new device.
    REQUIRE(reloaded.observe(make_obs(kCamB, "10.0.0.44", 3000), 3000, nullptr) ==
            ObservationOutcome::known_moved);
    REQUIRE(reloaded.size() == 2);

    // Strict parsing: a malformed or unbounded file is refused, never half-applied.
    {
        std::ofstream bad("hg_inventory_bad.txt");
        bad << "hg-camera-inventory 1\ncamera nonsense\n";
    }
    CameraInventory broken;
    REQUIRE_FALSE(broken.load("hg_inventory_bad.txt", &why));
    REQUIRE_FALSE(why.empty());
    REQUIRE(broken.size() == 0);

    CameraInventory missing;
    REQUIRE_FALSE(missing.load("hg_inventory_does_not_exist.txt", &why));
    REQUIRE(missing.size() == 0);

    std::remove(path.c_str());
    std::remove("hg_inventory_bad.txt");
}

// ---------------------------------------------------------------- integration

TEST_CASE("Three distinct cameras are onboarded and enforced independently",
          "[inventory][integration][multi-camera]") {
    Logger::initialize("error");

    // 1. IDENTITY: the inventory decides who each device is.
    CameraInventory inv;
    const std::string addr_a = "10.0.0.19", addr_b = "10.0.0.20", addr_c = "10.0.0.10";
    REQUIRE(inv.onboard(make_obs(kCamA, addr_a, 1000), kSite, "CAMUNIT0001", true, 1000, nullptr));
    REQUIRE(inv.onboard(make_obs(kCamB, addr_b, 1000), kSite, "CAMUNIT0002", true, 1000, nullptr));
    REQUIRE(inv.onboard(make_obs(kCamC, addr_c, 1000), kSite, "CAMUNIT0003", true, 1000, nullptr));
    REQUIRE(inv.size() == 3);

    const auto id_a = inv.find_by_fingerprint(fp(kCamA))->id;
    const auto id_b = inv.find_by_fingerprint(fp(kCamB))->id;
    const auto id_c = inv.find_by_fingerprint(fp(kCamC))->id;
    // Three distinct logical identities, none of them derived from an address.
    REQUIRE(id_a.canonical() != id_b.canonical());
    REQUIRE(id_b.canonical() != id_c.canonical());
    REQUIRE_FALSE(LogicalDeviceId::is_ipv4_literal(id_a.device_id));

    // 2. AUTHORIZATION: the existing registry, one record per camera, three credentials.
    net::DeviceRegistry registry{net::DeviceRegistry::defaults()};
    REQUIRE(registry.pair(registry_device(id_a.device_id, "cred-a"), nullptr));
    REQUIRE(registry.pair(registry_device(id_b.device_id, "cred-b"), nullptr));
    REQUIRE(registry.pair(registry_device(id_c.device_id, "cred-c"), nullptr));
    for (const auto* d : {&id_a, &id_b, &id_c}) {
        REQUIRE(registry.set_consent(d->device_id, true, "household", "presence", 1000));
        REQUIRE(registry.set_capture_enabled(d->device_id, true));
    }
    RegistryAuthorizer authz(registry, "presence");

    // 3. ENFORCEMENT: the existing site connector, one per camera, on one site profile.
    SiteProfile profile;
    profile.site_id = kSite;
    profile.connector_device_id = "connector.edge.01";
    profile.connector_display_name = "edge";
    profile.connector_credential_id = "connector-cred-01";
    Ipv4Cidr cidr{};
    REQUIRE(Ipv4Cidr::parse("10.0.0.0/24", &cidr));
    profile.allowed_camera_subnets.push_back(cidr);

    MockScript script;
    script.capabilities.add(Capability::video);
    script.frames_per_read = 1;
    script.frame_bytes = 512;

    std::vector<Event> events;
    auto sink = [&events](const Event& e) { events.push_back(e); };

    MockCameraAdapter adapter_a(id_a, "cam a", script);
    MockCameraAdapter adapter_b(id_b, "cam b", script);
    MockCameraAdapter adapter_c(id_c, "cam c", script);
    adapter_a.set_observed_ip(addr_a);
    adapter_b.set_observed_ip(addr_b);
    adapter_c.set_observed_ip(addr_c);

    // One frame per pull, so "only this camera was collected from" is exact arithmetic.
    ConnectorConfig ccfg;
    ccfg.max_frames_per_pull = 1;
    SiteConnector conn_a(adapter_a, authz, "cred-a", sink, ccfg, &profile, nullptr);
    SiteConnector conn_b(adapter_b, authz, "cred-b", sink, ccfg, &profile, nullptr);
    SiteConnector conn_c(adapter_c, authz, "cred-c", sink, ccfg, &profile, nullptr);

    for (SiteConnector* c : {&conn_a, &conn_b, &conn_c}) {
        REQUIRE(c->register_device() == AuthorizationOutcome::allowed);
        REQUIRE(c->start() == ConnectOutcome::ok);
        REQUIRE(c->pull_once(5000) == 1);
    }
    REQUIRE(adapter_a.frames_accounted() == 1);
    REQUIRE(adapter_b.frames_accounted() == 1);
    REQUIRE(adapter_c.frames_accounted() == 1);

    // A credential belonging to camera A is NOT accepted for camera B: three separate
    // identities, three separate credentials.
    REQUIRE(authz.authorize(id_b, "cred-a", Capability::video) ==
            AuthorizationOutcome::credential_mismatch);

    // Revoking ONE camera stops only that camera.
    REQUIRE(registry.revoke(id_c.device_id, "owner revoked the third camera"));
    REQUIRE(conn_c.pull_once(6000) == 0);
    REQUIRE_FALSE(conn_c.session_active());
    REQUIRE(conn_c.status().stopped_by_authorization);
    REQUIRE(conn_a.pull_once(6000) == 1);
    REQUIRE(conn_b.pull_once(6000) == 1);
    REQUIRE(conn_a.session_active());
    REQUIRE(conn_b.session_active());

    // Withdrawing CONSENT on another stops only that one, and reports a different reason.
    REQUIRE(registry.set_consent(id_b.device_id, false, "", "", 7000));
    REQUIRE(conn_b.pull_once(7000) == 0);
    REQUIRE_FALSE(conn_b.session_active());
    REQUIRE(conn_b.status().last_denial == AuthorizationOutcome::consent_missing);
    REQUIRE(conn_a.pull_once(7000) == 1);

    // Camera A is still exactly where it started.
    REQUIRE(conn_a.session_active());
    REQUIRE(adapter_a.frames_accounted() == 3);
}
