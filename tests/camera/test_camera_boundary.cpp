#include "catch2/catch.hpp"

#include "camera/CameraTypes.h"
#include "camera/MockCameraAdapter.h"
#include "camera/RegistryAuthorizer.h"
#include "camera/SiteConnector.h"

#include "core/Logger.h"
#include "core/Observation.h"
#include "core/Pipeline.h"
#include "net/DeviceRegistry.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Camera adapter boundary (HG-V380-09, task 3).
//
// WHAT THESE TESTS PROVE
//   That the boundary works: authorization is consulted before every frame, refusals
//   are classified rather than collapsed, a revocation/withdrawal/capture-off taking
//   effect mid-stream stops the stream, reconnection re-authorizes, and an
//   unauthorized device produces NO observation at all.
//
// WHAT THEY DO NOT PROVE — stated here so a green run cannot be misread
//   Nothing about V380 media. Every adapter used here is a MOCK: it opens no socket,
//   speaks no protocol, accounts for frames as METADATA ONLY (no payload is created),
//   and the connector derives Provenance::synthetic from AdapterClass::mock. The two
//   real cameras remain unintegrated: camera 1's standard interface is
//   unauthenticated and disabled, and camera 2 has no documented media interface.
// ---------------------------------------------------------------------------

using namespace homeguardian;
using namespace homeguardian::camera;

namespace {

constexpr const char* kSite = "site.home.dev";
constexpr const char* kDev = "cam.v380.ec39.01";
constexpr const char* kCred = "cred-fingerprint-01";

LogicalDeviceId dev_id() { return LogicalDeviceId{kSite, kDev}; }

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

net::DeviceRegistry::Device make_device(const std::string& id,
                                        const std::string& credential_id,
                                        bool has_camera = true) {
    net::DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "dev camera";
    d.device_type = "camera";
    d.credential_id = credential_id;
    d.paired = true;
    d.has_camera = has_camera;
    if (has_camera) d.capabilities = {"camera"};
    return d;
}

// Pair + consent + capture, i.e. the fully authorized state.
void authorize_fully(net::DeviceRegistry& reg, const std::string& id) {
    REQUIRE(reg.set_consent(id, true, "household", "presence", 1000));
    REQUIRE(reg.set_capture_enabled(id, true));
}

// A mock that can actually stream video, so the tests below exercise the streaming
// and mid-stream revocation paths rather than only the refusal paths.
MockScript streaming_script(uint32_t frames_per_read = 2) {
    MockScript s;
    s.capabilities.add(Capability::video);
    s.capabilities.add(Capability::ptz);
    s.capabilities.add(Capability::illumination);
    s.frames_per_read = frames_per_read;
    s.frame_bytes = 512;
    return s;
}

// An explicit pull bound, so each test states the number it is asserting rather than
// depending on a default.
ConnectorConfig pull_of(uint32_t max_frames) {
    ConnectorConfig c;
    c.max_frames_per_pull = max_frames;
    return c;
}

// An authorizer that can change its mind MID-PULL. This is what makes the
// "authorize before EVERY frame" rule testable rather than merely asserted: the
// decision can flip between two frames of the same call.
class ScriptedAuthorizer final : public IDeviceAuthorizer {
public:
    ScriptedAuthorizer() = default;

    AuthorizationOutcome authorize(const LogicalDeviceId&, const std::string&,
                                   Capability) const override {
        ++calls_;
        if (allow_calls_remaining_ < 0) return AuthorizationOutcome::allowed;
        if (allow_calls_remaining_ == 0) return deny_reason_;
        --allow_calls_remaining_;
        return AuthorizationOutcome::allowed;
    }

    void allow_then_deny(int allow_calls, AuthorizationOutcome reason) const {
        allow_calls_remaining_ = allow_calls;
        deny_reason_ = reason;
    }
    int calls() const { return calls_; }

private:
    mutable int calls_ = 0;
    mutable int allow_calls_remaining_ = -1;
    mutable AuthorizationOutcome deny_reason_ = AuthorizationOutcome::revoked;
};

struct Harness {
    net::DeviceRegistry registry{net::DeviceRegistry::defaults()};
    std::vector<Event> events{};
    RegistryAuthorizer authorizer;

    explicit Harness(std::string purpose = "presence")
        : authorizer(registry, std::move(purpose)) {
        Logger::initialize("error");
    }

    SiteConnector::EventSink sink() {
        return [this](const Event& e) { events.push_back(e); };
    }
};

}  // namespace

TEST_CASE("Camera boundary: logical identity is validated and an address can never be one",
          "[v380][identity]") {
    Logger::initialize("error");

    const LogicalDeviceId good{kSite, kDev};
    REQUIRE(good.valid());
    REQUIRE(good.canonical() == "site.home.dev/cam.v380.ec39.01");

    // An address in an identity field is refused: both real cameras have already
    // changed address once, so this is an identity bug waiting to happen.
    REQUIRE_FALSE(LogicalDeviceId{kSite, "10.20.1.19"}.valid());
    REQUIRE_FALSE(LogicalDeviceId{"10.20.1.20", kDev}.valid());
    REQUIRE(LogicalDeviceId::is_ipv4_literal("10.20.0.164"));
    REQUIRE(LogicalDeviceId::is_ipv4_literal("10.0.0.1"));

    // Not every dotted string is an address, and a non-address must still be usable.
    REQUIRE_FALSE(LogicalDeviceId::is_ipv4_literal("cam.v380.ec39.01"));
    REQUIRE_FALSE(LogicalDeviceId::is_ipv4_literal("999.1.1.1"));   // not a valid octet
    REQUIRE_FALSE(LogicalDeviceId::is_ipv4_literal("1.2.3"));       // not four groups

    REQUIRE_FALSE(LogicalDeviceId{"", kDev}.valid());
    REQUIRE_FALSE(LogicalDeviceId{kSite, "Has Space"}.valid());
    REQUIRE_FALSE(LogicalDeviceId{kSite, "UPPER"}.valid());
    REQUIRE_FALSE(LogicalDeviceId{kSite, std::string(LogicalDeviceId::kMaxSegmentChars + 1, 'a')}.valid());
}

TEST_CASE("Camera boundary: an unknown device is refused with its own reason",
          "[v380][authorization]") {
    Harness h;
    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());

    REQUIRE(connector.authorize(Capability::video) == AuthorizationOutcome::unknown_device);
    REQUIRE(connector.register_device() == AuthorizationOutcome::unknown_device);
    REQUIRE_FALSE(connector.status().registered);
    REQUIRE(connector.start() == ConnectOutcome::refused_unsafe_interface);
    REQUIRE(h.events.empty());   // nothing is emitted for a device we may not use
}

TEST_CASE("Camera boundary: pairing, consent and the capture switch are three separate gates",
          "[v380][authorization][consent]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));

    // 1. Paired but no consent.
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::consent_missing);

    // 2. Consent granted, capture switch still off -> a DIFFERENT reason, not the same one.
    authorize_fully(h.registry, kDev);
    REQUIRE(h.registry.set_capture_enabled(kDev, false));
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::capture_disabled);

    // 3. Both on -> allowed. Neither is inferred from the other.
    REQUIRE(h.registry.set_capture_enabled(kDev, true));
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::allowed);

    // 4. Consent withdrawn, capture left on -> consent is the reason again, and the
    //    capture switch cannot keep a device alive on its own.
    REQUIRE(h.registry.set_consent(kDev, false, "", "", 2000));
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::consent_missing);
}

TEST_CASE("Camera boundary: an unpaired device and a wrong credential are refused",
          "[v380][authorization][identity]") {
    Harness h;
    auto device = make_device(kDev, kCred);
    device.paired = false;
    REQUIRE(h.registry.pair(device, nullptr));
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::not_paired);

    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);
    REQUIRE(h.registry.set_capture_enabled(kDev, true));
    REQUIRE(h.authorizer.authorize(dev_id(), kCred, Capability::video) ==
            AuthorizationOutcome::allowed);

    // A credential that does not match the record's binding is refused...
    REQUIRE(h.authorizer.authorize(dev_id(), "someone-elses-cred", Capability::video) ==
            AuthorizationOutcome::credential_mismatch);
    // ...and so is presenting none at all. An empty credential is NOT "no check needed".
    REQUIRE(h.authorizer.authorize(dev_id(), "", Capability::video) ==
            AuthorizationOutcome::credential_mismatch);
}

TEST_CASE("Camera boundary: an authorized mock streams and its observation is SYNTHETIC",
          "[v380][mock][provenance]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock attachment", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink(), pull_of(2));

    REQUIRE(connector.register_device() == AuthorizationOutcome::allowed);
    REQUIRE(connector.start() == ConnectOutcome::ok);
    const size_t frames = connector.pull_once(now_ms());
    REQUIRE(frames == 2);
    REQUIRE(adapter.frames_accounted() == 2);

    REQUIRE(h.events.size() == 1);
    auto obs = Observation::from_event(h.events.front());
    REQUIRE(obs.has_value());
    // THE RULE: a mock can never be reported as a real camera.
    REQUIRE(obs->provenance == Provenance::synthetic);
    REQUIRE(obs->status == DetectorStatus::detected);
    REQUIRE(obs->detector_id == "camera-stream-activity");
    // Identity in the observation is the stable logical id, never an address.
    REQUIRE(obs->source == "camera:site.home.dev/cam.v380.ec39.01");
    REQUIRE(obs->source.find("192.168") == std::string::npos);
    REQUIRE(obs->features["adapter_class"] == "mock");
    REQUIRE(connector.status().adapter_class == AdapterClass::mock);
}

TEST_CASE("Camera boundary: a device with no video capability fails capability_absent",
          "[v380][mock][capability]") {
    // Camera 2's shape: the device is reachable but offers no documented media
    // interface, so no video capability is declared at all.
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "vendor ports only", no_video_device_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());

    REQUIRE(connector.register_device() == AuthorizationOutcome::allowed);
    REQUIRE(connector.start() == ConnectOutcome::capability_absent);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(adapter.frames_accounted() == 0);
    // A missing capability is announced as "unavailable", never as "no activity":
    // "we could not run it" must not look like "it ran and found nothing".
    REQUIRE(h.events.size() == 1);
    auto obs = Observation::from_event(h.events.front());
    REQUIRE(obs.has_value());
    REQUIRE(obs->status == DetectorStatus::unavailable);
}

TEST_CASE("Camera boundary: an interface that is not safe to use is refused and left stopped",
          "[v380][mock][safety]") {
    // Camera 1's shape: a standard interface that served anonymous live video. An
    // adapter for it must be able to refuse, and the stream must stay stopped.
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "unsafe onvif", unsafe_interface_device_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());

    REQUIRE(connector.start() == ConnectOutcome::refused_unsafe_interface);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(adapter.frames_accounted() == 0);
    REQUIRE(connector.status().last_connect == ConnectOutcome::refused_unsafe_interface);
}

TEST_CASE("Camera boundary: revoking mid-stream stops the stream and emits nothing",
          "[v380][revocation]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink(), pull_of(2));
    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(connector.pull_once(now_ms()) == 2);
    REQUIRE(h.events.size() == 1);

    // Operator revokes while the stream is running.
    REQUIRE(h.registry.revoke(kDev, "owner withdrew access"));

    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(connector.status().stopped_by_authorization);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::revoked);
    REQUIRE(connector.status().pulls_denied == 1);
    // Still exactly one observation: a revoked device must never produce another, and
    // in particular must not produce a "quiet camera" observation.
    REQUIRE(h.events.size() == 1);
}

TEST_CASE("Camera boundary: withdrawing consent mid-stream stops the stream",
          "[v380][consent][revocation]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());
    REQUIRE(connector.start() == ConnectOutcome::ok);

    REQUIRE(h.registry.set_consent(kDev, false, "", "", 3000));
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE(connector.status().stopped_by_authorization);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::consent_missing);
    REQUIRE(h.events.empty());
}

TEST_CASE("Camera boundary: switching capture off mid-stream stops the stream",
          "[v380][consent][revocation]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());
    REQUIRE(connector.start() == ConnectOutcome::ok);

    REQUIRE(h.registry.set_capture_enabled(kDev, false));
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE(connector.status().stopped_by_authorization);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::capture_disabled);
    REQUIRE(h.events.empty());
}

TEST_CASE("Camera boundary: a transport close ends the stream and is announced once",
          "[v380][mock][transport]") {
    // This family closes a client with no payload and no diagnosis (measured: ~5.0 s
    // idle, ~0.21 s on an unrecognized payload, zero bytes returned). The connector
    // must treat that as a normal, classified event — and must not flood the pipeline
    // while the device stays down.
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    auto script = streaming_script(1);
    script.close_after_reads = 1;
    MockCameraAdapter adapter(dev_id(), "mock", script);
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink(), pull_of(1));

    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(connector.pull_once(now_ms()) == 1);
    REQUIRE(connector.status().streaming);

    // Next pull hits the scripted close.
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(connector.status().last_connect == ConnectOutcome::transport_closed);

    const size_t after_close = h.events.size();
    // Repeated pulls while down do not keep announcing.
    connector.pull_once(now_ms());
    connector.pull_once(now_ms());
    REQUIRE(h.events.size() == after_close);
    REQUIRE(connector.status().observations_suppressed > 0);
}

TEST_CASE("Camera boundary: reconnect re-authorizes, so a device revoked while offline stays down",
          "[v380][reconnect][revocation]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());
    REQUIRE(connector.start() == ConnectOutcome::ok);

    // The link drops (transport closes), then the operator revokes before it comes back.
    adapter.set_forced_outcome(ConnectOutcome::transport_closed);
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE(h.registry.revoke(kDev, "revoked while offline"));

    adapter.clear_forced_outcome();
    REQUIRE(connector.reconnect(now_ms()) == ConnectOutcome::refused_unsafe_interface);
    REQUIRE_FALSE(connector.status().registered);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::revoked);
}

TEST_CASE("Camera boundary: reconnection is bounded and does not loop forever",
          "[v380][reconnect]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());

    // The device is gone. Every reconnect attempt probes and fails.
    adapter.set_forced_outcome(ConnectOutcome::unreachable);
    ConnectorConfig cfg;   // defaults: 3 attempts
    SiteConnector bounded(adapter, h.authorizer, kCred, h.sink(), cfg);

    for (uint32_t i = 0; i < 5; ++i) (void)bounded.reconnect(now_ms());

    REQUIRE(bounded.status().reconnects == cfg.max_reconnect_attempts);
    REQUIRE(bounded.status().reconnect_failures >= cfg.max_reconnect_attempts);
    REQUIRE_FALSE(bounded.status().streaming);
}

TEST_CASE("Camera boundary: observations flow through the EXISTING pipeline",
          "[v380][mock][integration]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    // No second event engine: the connector's sink is the existing Pipeline.
    Pipeline pipeline({}, std::chrono::milliseconds(5000));
    MockCameraAdapter adapter(dev_id(), "mock", streaming_script());
    SiteConnector connector(adapter, h.authorizer, kCred,
                            [&pipeline](const Event& e) { pipeline.process(e); }, pull_of(2));

    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(connector.pull_once(now_ms()) == 2);

    const auto recent = pipeline.get_recent_events(std::chrono::milliseconds(60000));
    REQUIRE(recent.size() == 1);
    auto obs = Observation::from_event(recent.front());
    REQUIRE(obs.has_value());
    REQUIRE(obs->provenance == Provenance::synthetic);
    REQUIRE(connector.status().observations_emitted == 1);
    REQUIRE(connector.status().observations_rejected == 0);
}

TEST_CASE("Camera boundary: authorization is re-checked BEFORE EACH FRAME, mid-pull",
          "[v380][authorization][toctou]") {
    // The strongest form of the rule: a decision that changes between two frames of the
    // SAME pull must stop the pull immediately. A session-level check would not notice.
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    MockCameraAdapter adapter(dev_id(), "mock", streaming_script(1));
    ScriptedAuthorizer scripted;
    // start() consumes one authorization; allow two more so the SECOND frame of the
    // pull is the one that gets refused.
    scripted.allow_then_deny(3, AuthorizationOutcome::revoked);
    SiteConnector connector(adapter, scripted, kCred, h.sink(), pull_of(4));

    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(scripted.calls() == 1);

    const size_t accounted = connector.pull_once(now_ms());
    REQUIRE(accounted == 2);                       // stopped on the third check, not after 4
    REQUIRE(accounted < 4);                        // and strictly fewer than the bound
    REQUIRE(connector.status().stopped_by_authorization);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::revoked);
    REQUIRE(connector.status().pulls_denied == 1);
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(h.events.empty());                     // nothing is emitted for a refused device
}

TEST_CASE("Camera boundary: control commands require the matching declared capability",
          "[v380][mock][capability]") {
    Harness h;
    REQUIRE(h.registry.pair(make_device(kDev, kCred), nullptr));
    authorize_fully(h.registry, kDev);

    // A device that streams but declares no PTZ and no light.
    MockScript script;
    script.capabilities.add(Capability::video);
    MockCameraAdapter adapter(dev_id(), "video only", script);
    SiteConnector connector(adapter, h.authorizer, kCred, h.sink());

    REQUIRE(adapter.control(ControlCommand::ptz_left) == ConnectOutcome::capability_absent);
    REQUIRE(adapter.control(ControlCommand::light_on) == ConnectOutcome::capability_absent);
    REQUIRE(adapter.control_calls() == 2);

    // With the capability declared, the command is attempted (and still never
    // authorized by declaration alone — the connector would gate it in production).
    MockScript with_ptz = streaming_script();
    MockCameraAdapter ptz_adapter(dev_id(), "ptz", with_ptz);
    REQUIRE(ptz_adapter.control(ControlCommand::ptz_left) == ConnectOutcome::ok);
    REQUIRE(ptz_adapter.last_control() == ControlCommand::ptz_left);
}
