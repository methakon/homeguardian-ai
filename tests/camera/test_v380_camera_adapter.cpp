#include <catch2/catch.hpp>

#include "camera/CameraInventory.h"
#include "camera/CameraTypes.h"
#include "camera/RegistryAuthorizer.h"
#include "camera/SiteConnector.h"
#include "camera/V380CameraAdapter.h"
#include "core/LocalMediaObservation.h"
#include "net/DeviceRegistry.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;
using namespace homeguardian::camera;

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

V380CameraAdapter::Options default_options(bool synthetic = true) {
    V380CameraAdapter::Options opts;
    opts.id = LogicalDeviceId{"site.home.prod", "cam.v380.ec39.01"};
    opts.attachment_summary = "V380 EC39 on 192.168.1.19:8800/9800";
    opts.observed_ip = "192.168.1.19";
    opts.source_config.enrolled_identity = "cam.v380.ec39.01";
    opts.source_config.endpoint_host = "192.168.1.19";
    opts.source_config.consent_granted = true;
    opts.source_config.synthetic_fixture = synthetic;
    opts.source_config.fixture_frames = 6;
    opts.frame_width = 32;
    opts.frame_height = 32;
    return opts;
}

}  // namespace

TEST_CASE("v380 camera adapter: identity and capabilities", "[v380camera]") {
    auto opts = default_options();
    V380CameraAdapter adapter(opts);

    REQUIRE(adapter.id().site_id == "site.home.prod");
    REQUIRE(adapter.id().device_id == "cam.v380.ec39.01");
    REQUIRE(adapter.observed_ip() == "192.168.1.19");
    REQUIRE(adapter.attachment_summary() == "V380 EC39 on 192.168.1.19:8800/9800");

    const auto caps = adapter.declared_capabilities();
    REQUIRE(caps.has(Capability::video));
    REQUIRE(caps.has(Capability::motion_events));
    REQUIRE_FALSE(caps.has(Capability::ptz));
    REQUIRE(adapter.probe() == ConnectOutcome::ok);
}

TEST_CASE("v380 camera adapter: inventory onboarding and device registry integration", "[v380camera][registry]") {
    CameraInventory inventory;
    V380CameraAdapter::RegistrationOptions reg_opts;
    reg_opts.id = LogicalDeviceId{"site.home.prod", "cam.v380.ec39.01"};
    reg_opts.hardware_mac = "44:12:65:aa:bb:cc";  // non-randomized unicast MAC
    reg_opts.endpoint_host = "192.168.1.19";
    reg_opts.owner_identity_label = "ec39.01";
    reg_opts.owner_authorized = true;
    reg_opts.now_ms = now_ms();

    std::string reason;
    REQUIRE(V380CameraAdapter::register_into_inventory(inventory, reg_opts, &reason));
    REQUIRE(inventory.size() == 1);

    const auto* rec = inventory.find(reg_opts.id);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->owner_identity_label == "ec39.01");
    REQUIRE(rec->firmware_model == "V380_EC39");
    REQUIRE(rec->media == MediaStatus::vendor_protocol_only);

    // Reject randomized MAC
    reg_opts.hardware_mac = "02:11:22:33:44:55";  // locally administered / randomized bit set
    reg_opts.id.device_id = "cam.v380.ec39.02";
    REQUIRE_FALSE(V380CameraAdapter::register_into_inventory(inventory, reg_opts, &reason));
    REQUIRE(reason.find("randomized") != std::string::npos);

    // DeviceRegistry pairing and consent
    net::DeviceRegistry dev_registry{net::DeviceRegistry::defaults()};
    REQUIRE(V380CameraAdapter::register_into_device_registry(
        dev_registry, LogicalDeviceId{"site.home.prod", "cam.v380.ec39.01"}, "v380-cred-01", true, true, now_ms()));

    RegistryAuthorizer authorizer(dev_registry, "presence");
    REQUIRE(authorizer.authorize(LogicalDeviceId{"site.home.prod", "cam.v380.ec39.01"},
                                 "v380-cred-01", Capability::video) == AuthorizationOutcome::allowed);
}

TEST_CASE("v380 camera adapter: honest refusal when unverified local interface without fixture", "[v380camera][refusal]") {
    auto opts = default_options(/*synthetic=*/false);
    V380CameraAdapter adapter(opts);

    StreamRequest req;
    REQUIRE(adapter.start_stream(req) == ConnectOutcome::refused_unsafe_interface);
    REQUIRE_FALSE(adapter.streaming());
    REQUIRE(adapter.last_outcome() == ConnectOutcome::refused_unsafe_interface);

    FrameMeta metas[2];
    REQUIRE(adapter.read_frames(metas, 2) == 0);
}

TEST_CASE("v380 camera adapter: streams frames and drives motion analysis pipeline", "[v380camera][pipeline]") {
    auto opts = default_options(/*synthetic=*/true);
    opts.frame_width = 32;
    opts.frame_height = 32;
    V380CameraAdapter adapter(opts);

    std::vector<LocalMediaObservation> motion_events;
    adapter.set_motion_sink([&](const LocalMediaObservation& obs) {
        motion_events.push_back(obs);
    });

    StreamRequest req;
    req.max_frames = 4;
    REQUIRE(adapter.start_stream(req) == ConnectOutcome::ok);
    REQUIRE(adapter.streaming());

    FrameMeta metas[4];
    size_t read = adapter.read_frames(metas, 4);
    REQUIRE(read == 4);
    REQUIRE(adapter.frames_accounted() == 4);

    for (size_t i = 0; i < read; ++i) {
        REQUIRE(metas[i].bytes > 0);
        REQUIRE(metas[i].codec == "raw_luma");
    }

    // Since 4 consecutive frames were read, derive_motion_observation was invoked for pairs:
    // pair (1,2), (2,3), (3,4) -> 3 motion events
    REQUIRE(motion_events.size() == 3);
    for (const auto& obs : motion_events) {
        REQUIRE(obs.valid);
        REQUIRE(obs.frame_id > 0);
    }
    REQUIRE(adapter.recent_motion_observations().size() == 3);

    adapter.stop_stream();
    REQUIRE_FALSE(adapter.streaming());
}

TEST_CASE("v380 camera adapter: site connector driving v380 adapter", "[v380camera][site_connector]") {
    auto opts = default_options(/*synthetic=*/true);
    opts.frame_width = 32;
    opts.frame_height = 32;
    V380CameraAdapter adapter(opts);

    net::DeviceRegistry dev_registry{net::DeviceRegistry::defaults()};
    net::DeviceRegistry::Device d;
    d.device_id = opts.id.device_id;
    d.display_name = "V380 camera";
    d.device_type = "camera";
    d.credential_id = "cred-v380";
    d.paired = true;
    d.has_camera = true;
    d.capabilities = {"camera"};
    std::string why;
    REQUIRE(dev_registry.pair(d, &why));
    REQUIRE(dev_registry.set_consent(d.device_id, true, "household", "presence", now_ms()));
    REQUIRE(dev_registry.set_capture_enabled(d.device_id, true));

    RegistryAuthorizer authorizer(dev_registry, "presence");
    std::vector<Event> emitted_events;
    auto sink = [&](const Event& e) {
        emitted_events.push_back(e);
    };

    ConnectorConfig cfg;
    cfg.max_frames_per_pull = 2;
    SiteConnector connector(adapter, authorizer, "cred-v380", sink, cfg);

    REQUIRE(connector.register_device() == AuthorizationOutcome::allowed);
    REQUIRE(connector.start() == ConnectOutcome::ok);

    size_t pulled = connector.pull_once(now_ms());
    REQUIRE(pulled == 2);
    REQUIRE(adapter.frames_accounted() == 2);
    REQUIRE_FALSE(emitted_events.empty());
}
