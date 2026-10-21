#include "catch2/catch.hpp"

#include "camera/CameraTypes.h"
#include "camera/ControlChannel.h"
#include "camera/DirectMediaPlanner.h"
#include "camera/MockCameraAdapter.h"
#include "camera/RegistryAuthorizer.h"
#include "camera/SiteConnector.h"
#include "camera/SiteProfile.h"

#include "core/Logger.h"
#include "core/Observation.h"
#include "net/DeviceRegistry.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Site connectors, multi-site isolation and the control/media split
// (HG-NET-02C / Issue #16, Hermes 3).
//
// WHAT THESE TESTS PROVE
//   That a site connector cannot reach a camera outside its own site's configured
//   subnets even when a route exists; that an address is an attachment and not an
//   identity (the same logical device keeps its authorization when its IP moves);
//   that the coordination channel cannot carry media by construction; that media never
//   plans through the control hub; and that revocation and consent withdrawal terminate
//   a live session and say which of the two happened.
//
// WHAT THEY DO NOT PROVE
//   Nothing about V380 media. Every adapter here is a MOCK (Provenance::synthetic, no
//   socket, no payload). No site connector has been deployed at a remote site, no VPN is
//   involved in these tests, and camera 2 is not shown to produce any sensor output.
// ---------------------------------------------------------------------------

using namespace homeguardian;
using namespace homeguardian::camera;

namespace {

constexpr const char* kSiteA = "site.home.dev";
constexpr const char* kSiteB = "site.lab.remote";
constexpr const char* kConnA = "connector.edge.01";
constexpr const char* kConnB = "connector.edge.02";
// Two distinct credentials, deliberately: the connector PRESENTS the camera's bound
// credential when it asks the registry about that camera, while the site profile carries
// the CONNECTOR's own credential id. Conflating them would silently test the wrong thing.
constexpr const char* kCamCred = "cam-cred-01";
constexpr const char* kConnCred = "connector-cred-01";
constexpr const char* kCam = "cam.v380.ec39.02";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

SiteProfile profile_for(const char* site, const char* connector, const std::string& cidr) {
    SiteProfile p;
    p.site_id = site;
    p.connector_device_id = connector;
    p.connector_display_name = "edge connector";
    p.connector_credential_id = kConnCred;
    Ipv4Cidr c{};
    REQUIRE(Ipv4Cidr::parse(cidr, &c));
    p.allowed_camera_subnets.push_back(c);
    return p;
}

net::DeviceRegistry::Device make_device(const std::string& id) {
    net::DeviceRegistry::Device d;
    d.device_id = id;
    d.display_name = "dev camera";
    d.device_type = "camera";
    d.credential_id = kCamCred;
    d.paired = true;
    d.has_camera = true;
    d.capabilities = {"camera"};
    return d;
}

MockScript streaming_script() {
    MockScript s;
    s.capabilities.add(Capability::video);
    s.capabilities.add(Capability::ptz);
    s.frames_per_read = 1;
    s.frame_bytes = 512;
    return s;
}

LogicalDeviceId cam_id() { return LogicalDeviceId{kSiteA, kCam}; }

// A registry with one fully authorized camera.
struct Fixture {
    net::DeviceRegistry registry{net::DeviceRegistry::defaults()};
    RegistryAuthorizer authorizer;
    std::vector<Event> events{};
    std::vector<std::string> wire{};
    ControlChannel channel{};
    Fixture() : authorizer(registry, "presence") {
        Logger::initialize("error");
        channel.set_capture(&wire);
        REQUIRE(registry.pair(make_device(kCam), nullptr));
        REQUIRE(registry.set_consent(kCam, true, "household", "presence", 1000));
        REQUIRE(registry.set_capture_enabled(kCam, true));
    }
    SiteConnector::EventSink sink() { return [this](const Event& e) { events.push_back(e); }; }

    // Decode the kinds this connector put on the wire.
    std::vector<ControlKind> kinds() const {
        std::vector<ControlKind> out;
        for (const auto& w : wire) {
            ControlMessage m;
            std::string why;
            if (channel.decode(w, &m, &why)) out.push_back(m.kind);
        }
        return out;
    }
    bool has_kind(ControlKind k) const {
        for (auto x : kinds()) if (x == k) return true;
        return false;
    }
};

MockCameraAdapter camera_at(const std::string& ip) {
    MockCameraAdapter a(cam_id(), "mock camera", streaming_script());
    a.set_observed_ip(ip);
    return a;
}

}  // namespace

// ---------------------------------------------------------------- reach rules

TEST_CASE("Site profile: reach rules parse strictly and fail closed",
          "[sites][profile]") {
    Logger::initialize("error");

    Ipv4Cidr c{};
    REQUIRE(Ipv4Cidr::parse("10.20.30.0/24", &c));
    REQUIRE(c.contains(0x0A141E14u));       // 10.20.30.20
    REQUIRE_FALSE(c.contains(0x0A142805u)); // 10.20.0.5
    REQUIRE(Ipv4Cidr::parse("10.0.0.0/8", &c));
    REQUIRE(c.contains(0x0A142805u));

    // A bare address is NOT a reach rule, and a bad prefix is refused rather than
    // defaulting to something permissive.
    REQUIRE_FALSE(Ipv4Cidr::parse("10.20.1.0", &c));
    REQUIRE_FALSE(Ipv4Cidr::parse("10.20.1.0/33", &c));
    REQUIRE_FALSE(Ipv4Cidr::parse("10.20.1.0/x", &c));
    REQUIRE_FALSE(Ipv4Cidr::parse("999.1.1.0/24", &c));
    REQUIRE_FALSE(Ipv4Cidr::parse("10.20.1/24", &c));

    // An empty allowlist reaches NOTHING (it does not mean "reach everything").
    SiteProfile empty = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    empty.allowed_camera_subnets.clear();
    REQUIRE_FALSE(empty.valid());
    REQUIRE_FALSE(empty.permits(0x0A141E14u));

    // A profile with no connector credential is unusable, so it reaches nothing.
    SiteProfile no_cred = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    no_cred.connector_credential_id.clear();
    REQUIRE_FALSE(no_cred.valid());
    REQUIRE_FALSE(no_cred.permits(0x0A141E14u));

    // The connector's own identity is a logical id and refuses an address.
    SiteProfile bad_id = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    bad_id.connector_device_id = "10.20.30.3";
    REQUIRE_FALSE(bad_id.valid());
}

// ---------------------------------------------------------------- multi-site

TEST_CASE("Multi-site isolation: a connector cannot reach another site's camera",
          "[sites][isolation]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    const SiteProfile site_b = profile_for(kSiteB, kConnB, "10.20.0.0/16");

    // The camera is observed at 10.20.0.5 — site B's subnet.
    MockCameraAdapter adapter = camera_at("10.20.0.5");
    SiteConnector at_site_a(adapter, fx.authorizer, kCamCred, fx.sink(), {}, &site_a, &fx.channel);

    REQUIRE(at_site_a.has_site_profile());
    REQUIRE(at_site_a.scope_check() == ConnectOutcome::out_of_site_scope);
    REQUIRE(at_site_a.register_device() == AuthorizationOutcome::unknown_device);
    REQUIRE(at_site_a.status().out_of_site_scope);
    REQUIRE(at_site_a.start() == ConnectOutcome::out_of_site_scope);
    REQUIRE(adapter.frames_accounted() == 0);      // nothing was ever pulled
    REQUIRE_FALSE(at_site_a.session_active());
    // It IS reachable on the network and IS authorized - neither of which is permission to
    // collect from another site.
    REQUIRE(fx.authorizer.authorize(cam_id(), "cam-cred-01", Capability::video) ==
            AuthorizationOutcome::allowed);

    // The same camera under the site it actually belongs to is allowed.
    MockCameraAdapter adapter_b = camera_at("10.20.0.5");
    SiteConnector at_site_b(adapter_b, fx.authorizer, kCamCred, fx.sink(), {}, &site_b, &fx.channel);
    REQUIRE(at_site_b.scope_check() == ConnectOutcome::ok);
    REQUIRE(at_site_b.register_device() == AuthorizationOutcome::allowed);
    REQUIRE(at_site_b.start() == ConnectOutcome::ok);
    REQUIRE(at_site_b.session_active());
}

TEST_CASE("Site scope: an unevaluable reach rule is not permission",
          "[sites][isolation]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");

    // The adapter cannot report where it reached. We therefore cannot show the camera is in
    // this site, so we refuse rather than assume.
    MockCameraAdapter adapter(cam_id(), "no observed address", streaming_script());
    SiteConnector connector(adapter, fx.authorizer, kCamCred, fx.sink(), {}, &site_a, &fx.channel);

    REQUIRE(connector.scope_check() == ConnectOutcome::out_of_site_scope);
    REQUIRE(connector.start() == ConnectOutcome::out_of_site_scope);
    REQUIRE(connector.status().last_connect == ConnectOutcome::out_of_site_scope);

    // With no site profile bound there is nothing to evaluate, and the development path
    // still works (this is not a silent weakening: bound profile => enforced).
    MockCameraAdapter adapter2(cam_id(), "no address", streaming_script());
    SiteConnector dev(adapter2, fx.authorizer, kCamCred, fx.sink());
    REQUIRE_FALSE(dev.has_site_profile());
    REQUIRE(dev.scope_check() == ConnectOutcome::ok);
}

// ---------------------------------------------------------------- dynamic IP

TEST_CASE("An address is an attachment: the same logical device keeps its authorization when it moves",
          "[sites][identity][dynamic-ip]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");

    // Observed endpoint record: it starts at the old development address and moves.
    ObservedEndpoint endpoint;
    endpoint.device = cam_id();
    endpoint.first_seen_ms = 1000;
    endpoint.reobserve("10.20.30.20", "10.20.30.0/24", 2000);
    REQUIRE(endpoint.observed_ip == "10.20.30.20");
    REQUIRE(endpoint.observation_count == 1);

    // The camera moves to a NAT'ed segment address that is still inside the site's allowlist.
    endpoint.reobserve("10.20.40.164", "10.20.40.0/24", 3000);
    REQUIRE(endpoint.observation_count == 2);
    REQUIRE(endpoint.device.canonical() == cam_id().canonical());   // identity did NOT change

    // Authorization follows the IDENTITY, not the address: unchanged across the move.
    REQUIRE(fx.authorizer.authorize(cam_id(), "cam-cred-01", Capability::video) ==
            AuthorizationOutcome::allowed);

    // And a move to an address OUTSIDE the allowlist is refused, without changing identity,
    // consent or the registry record.
    SiteProfile narrow = profile_for(kSiteA, kConnA, "10.20.40.0/24");
    MockCameraAdapter moved = camera_at("10.20.0.5");
    SiteConnector connector(moved, fx.authorizer, kCamCred, fx.sink(), {}, &narrow, &fx.channel);
    REQUIRE(connector.scope_check() == ConnectOutcome::out_of_site_scope);
    REQUIRE(fx.authorizer.authorize(cam_id(), "cam-cred-01", Capability::video) ==
            AuthorizationOutcome::allowed);   // permission is untouched by reach
    const auto* rec = fx.registry.find(kCam);
    REQUIRE(rec != nullptr);
    // The reach failure changed NOTHING about the device record: paired, consented and
    // capture-enabled all still hold, because reach is not permission.
    REQUIRE(rec->paired);
    REQUIRE(rec->consent_granted);
    REQUIRE(rec->capture_enabled);
    REQUIRE_FALSE(rec->revoked);
}

// ---------------------------------------------------------------- control channel

TEST_CASE("Control channel: media is unrepresentable, not merely forbidden",
          "[sites][control][no-media]") {
    ControlChannel channel;
    REQUIRE(channel.max_bytes() == ControlChannel::kMaxEncodedBytes);

    // Every kind the channel can express is a control kind. Assert it structurally: no kind
    // name may even suggest media.
    for (ControlKind k : {ControlKind::connector_register, ControlKind::heartbeat,
                          ControlKind::authorization_request, ControlKind::authorization_result,
                          ControlKind::consent_change, ControlKind::revocation,
                          ControlKind::ice_candidate, ControlKind::stream_control,
                          ControlKind::session_terminated, ControlKind::telemetry}) {
        const std::string name = control_kind_to_string(k);
        REQUIRE(name.find("video") == std::string::npos);
        REQUIRE(name.find("audio") == std::string::npos);
        REQUIRE(name.find("frame") == std::string::npos);
        REQUIRE(name.find("media") == std::string::npos);
    }

    // A well-formed small message encodes and round-trips.
    ControlMessage m;
    m.kind = ControlKind::heartbeat;
    m.device = LogicalDeviceId{kSiteA, kConnA};
    m.seq = 7;
    m.at_ms = 1234;
    m.detail = "alive";
    const std::string wire = channel.encode(m);
    REQUIRE_FALSE(wire.empty());
    REQUIRE(wire.size() <= ControlChannel::kMaxEncodedBytes);

    ControlMessage back;
    std::string why;
    REQUIRE(channel.decode(wire, &back, &why));
    REQUIRE(back.kind == ControlKind::heartbeat);
    REQUIRE(back.seq == 7);
    REQUIRE(back.device.canonical() == "site.home.dev/connector.edge.01");

    // A frame-sized payload cannot travel here - refused, not truncated. The reference is a
    // single 1080p H.265 keyframe; the channel ceiling is a small control message.
    REQUIRE(ControlChannel::kReferenceFrameBytes > ControlChannel::kMaxEncodedBytes);
    std::string frame(ControlChannel::kReferenceFrameBytes, 'x');
    REQUIRE_FALSE(channel.reject_bulk(frame, &why));
    REQUIRE(why.find("media") != std::string::npos);
    REQUIRE(channel.refused_bulk() == 1);

    // Even a well-formed control message with a frame-sized detail is refused.
    ControlMessage fat = m;
    fat.detail = std::string(ControlMessage::kMaxDetailChars + 1, 'a');
    REQUIRE(channel.encode(fat).empty());
    REQUIRE(channel.refused() >= 1);

    // Unknown kinds and malformed wire forms are refused.
    REQUIRE_FALSE(channel.decode("HC1|teleport|site.home.dev/x|1|2|", &back, &why));
    REQUIRE_FALSE(channel.decode("garbage", &back, &why));
    REQUIRE_FALSE(channel.decode("HC1|heartbeat|not-an-id|1|2|", &back, &why));
    REQUIRE_FALSE(channel.decode("HC1|heartbeat|site.home.dev/x|1|2|a|b", &back, &why));
}

// ---------------------------------------------------------------- direct media

TEST_CASE("Direct media: the control hub is never a media path",
          "[sites][media][no-media]") {
    DirectMediaPlanner planner;

    // A no-media device (camera 2's shape) has nothing to transport.
    MediaPathRequest none;
    none.connector_declares_media = false;
    REQUIRE(planner.plan(none).path == MediaPath::unavailable);

    // Same subnet -> the cheapest direct path.
    MediaPathRequest same;
    same.same_subnet = true;
    auto d = planner.plan(same);
    REQUIRE(d.path == MediaPath::direct_same_site);
    REQUIRE_FALSE(d.uses_control_hub);

    // Ordinary NATs -> ICE-assisted direct.
    MediaPathRequest ice;
    ice.site_nat = NatClass::cone_nat;
    ice.viewer_nat = NatClass::cone_nat;
    d = planner.plan(ice);
    REQUIRE(d.path == MediaPath::direct_ice);
    REQUIRE_FALSE(d.uses_control_hub);
    REQUIRE_FALSE(d.uses_site_bandwidth);

    // Hard NAT -> the SITE serves it over its own uplink (still not the hub, and the site
    // pays for its own camera rather than the VPS).
    MediaPathRequest hard;
    hard.site_nat = NatClass::symmetric_nat;
    hard.viewer_nat = NatClass::cgnat;
    d = planner.plan(hard);
    REQUIRE(d.path == MediaPath::site_uplink);
    REQUIRE(d.uses_site_bandwidth);
    REQUIRE_FALSE(d.uses_control_hub);

    // Site uplink unusable -> a relay is REQUIRED and needs an explicit decision; it is
    // never taken automatically.
    MediaPathRequest relay = hard;
    relay.site_uplink_usable = false;
    d = planner.plan(relay, /*allow_relay=*/false);
    REQUIRE(d.path == MediaPath::relay_required);
    REQUIRE(d.needs_owner_approval);
    REQUIRE_FALSE(d.uses_control_hub);

    d = planner.plan(relay, /*allow_relay=*/true);
    REQUIRE(d.path == MediaPath::relay_required);
    REQUIRE_FALSE(d.needs_owner_approval);   // pre-approved separately, still not the hub
    REQUIRE_FALSE(d.uses_control_hub);

    // THE INVARIANT, across the whole input space: no combination selects the control hub.
    for (auto site : {NatClass::unknown, NatClass::same_site, NatClass::cone_nat,
                      NatClass::symmetric_nat, NatClass::cgnat}) {
        for (auto viewer : {NatClass::unknown, NatClass::cone_nat, NatClass::symmetric_nat,
                            NatClass::cgnat}) {
            for (bool same_subnet : {false, true}) {
                for (bool media : {false, true}) {
                    for (bool fail : {false, true}) {
                        for (bool uplink : {false, true}) {
                            MediaPathRequest r;
                            r.site_nat = site; r.viewer_nat = viewer;
                            r.same_subnet = same_subnet;
                            r.connector_declares_media = media;
                            r.direct_attempt_failed = fail;
                            r.site_uplink_usable = uplink;
                            REQUIRE_FALSE(planner.plan(r, true).uses_control_hub);
                            REQUIRE_FALSE(planner.plan(r, false).uses_control_hub);
                        }
                    }
                }
            }
        }
    }

    // The ratio that makes the ceiling meaningful, stated in code rather than prose.
    REQUIRE(DirectMediaPlanner::media_to_control_ratio(ControlChannel::kReferenceFrameBytes) >= 32);
}

// ---------------------------------------------------------------- sessions

TEST_CASE("Control plane: a connector registers and reports on the wire, control only",
          "[sites][control]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    MockCameraAdapter adapter = camera_at("10.20.30.20");
    SiteConnector connector(adapter, fx.authorizer, kCamCred, fx.sink(), {}, &site_a, &fx.channel);

    REQUIRE(connector.register_device() == AuthorizationOutcome::allowed);
    REQUIRE(fx.has_kind(ControlKind::connector_register));
    REQUIRE(connector.status().control_sent >= 1);

    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(fx.has_kind(ControlKind::stream_control));
    REQUIRE(connector.session_active());

    connector.terminate_session("operator closed it", now_ms());
    REQUIRE(fx.has_kind(ControlKind::session_terminated));
    REQUIRE_FALSE(connector.session_active());
    REQUIRE_FALSE(connector.status().streaming);
    REQUIRE(connector.status().session_termination_reason == "operator closed it");

    // Idempotent: a second termination adds nothing.
    const size_t before = fx.wire.size();
    connector.terminate_session("again", now_ms());
    REQUIRE(fx.wire.size() == before);

    // Every message on the wire really is control-only.
    for (const auto& w : fx.wire) {
        REQUIRE(w.rfind("HC1|", 0) == 0);
        REQUIRE(w.size() <= ControlChannel::kMaxEncodedBytes);
    }
}

TEST_CASE("Revocation and consent withdrawal terminate a live session, and say which happened",
          "[sites][control][revocation]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    MockCameraAdapter adapter = camera_at("10.20.30.20");
    // One frame per pull, so "a frame is accounted for, then a denial stops everything" is
    // an explicit step rather than a consequence of the default batch size.
    ConnectorConfig cfg;
    cfg.max_frames_per_pull = 1;
    SiteConnector connector(adapter, fx.authorizer, kCamCred, fx.sink(), cfg, &site_a, &fx.channel);

    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(connector.session_active());
    REQUIRE(connector.pull_once(now_ms()) == 1);
    const size_t events_after_pull = fx.events.size();
    REQUIRE(events_after_pull >= 1);

    // Consent withdrawal mid-stream: the session must die and the control plane must be told
    // it was a CONSENT change, not a revocation.
    REQUIRE(fx.registry.set_consent(kCam, false, "", "", 4000));
    fx.wire.clear();
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE_FALSE(connector.session_active());
    REQUIRE(connector.status().stopped_by_authorization);
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::consent_missing);
    REQUIRE(fx.has_kind(ControlKind::consent_change));
    REQUIRE_FALSE(fx.has_kind(ControlKind::revocation));
    REQUIRE(fx.events.size() == events_after_pull);   // nothing emitted for a refused device

    // Now a revocation, from a fresh authorized session, must report REVOCATION.
    REQUIRE(fx.registry.set_consent(kCam, true, "household", "presence", 5000));
    REQUIRE(fx.registry.set_capture_enabled(kCam, true));
    REQUIRE(connector.start() == ConnectOutcome::ok);
    REQUIRE(connector.session_active());
    fx.wire.clear();
    REQUIRE(fx.registry.revoke(kCam, "owner withdrew access"));
    REQUIRE(connector.pull_once(now_ms()) == 0);
    REQUIRE_FALSE(connector.session_active());
    REQUIRE(connector.status().last_denial == AuthorizationOutcome::revoked);
    REQUIRE(fx.has_kind(ControlKind::revocation));
    REQUIRE(connector.status().session_termination_reason.find("revoked") != std::string::npos);
}

TEST_CASE("Telemetry on the control channel is bounded, not per-frame",
          "[sites][control][bounds]") {
    Fixture fx;
    const SiteProfile site_a = profile_for(kSiteA, kConnA, "10.20.30.0/24");
    MockCameraAdapter adapter = camera_at("10.20.30.20");
    ConnectorConfig cfg;
    cfg.max_frames_per_pull = 1;
    cfg.telemetry_every_n_pulls = 2;
    SiteConnector connector(adapter, fx.authorizer, kCamCred, fx.sink(), cfg, &site_a, &fx.channel);

    REQUIRE(connector.start() == ConnectOutcome::ok);
    fx.wire.clear();
    for (int i = 0; i < 6; ++i) REQUIRE(connector.pull_once(now_ms()) == 1);

    size_t telemetry = 0;
    for (auto k : fx.kinds()) if (k == ControlKind::telemetry) ++telemetry;
    REQUIRE(telemetry == 3);              // 6 pulls / every 2nd = 3
    REQUIRE(telemetry < 6);              // and never one per frame

    // Whatever the volume, nothing on the wire is frame-sized.
    for (const auto& w : fx.wire) REQUIRE(w.size() < ControlChannel::kMaxEncodedBytes);
}
