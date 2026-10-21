// Camera1 backend lifecycle tests (H-03 blocked-acceptance fix).
//
// These exist because the physical device hit
//     camera: initialization failed (NdkCameraDevice: no camera available)
// on a phone whose legacy Camera1 path works. The native lifecycle now has a representation for the
// Java Camera1 bridge, and these tests pin the rules that make that representation safe:
//   * no reported camera / zero cameras  -> initialize() REFUSES (it does not pretend a camera exists);
//   * a failed Camera.open()/startPreview() can NEVER leave the device Capturing (no `video=on` lie);
//   * the consent guard works on this backend exactly as on the NDK one, including withdrawal and OS
//     permission revocation forcing the device out of capture.

#include "catch2/catch.hpp"

#include "backend/android/Camera1Device.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "persistence/SingleActiveGrantConsentRepo.h"

using namespace homeguardian;

namespace {
AcquisitionRequest cam_request() { return {"node", "camera_acquire", "camera"}; }
} // namespace

TEST_CASE("camera1: initialize refuses before the platform count is reported", "[camera1]") {
    Camera1Device dev;
    REQUIRE(dev.state() == DeviceState::Idle);
    REQUIRE_THROWS(dev.initialize());
    REQUIRE(dev.state() == DeviceState::Error);
    dev.close();
    REQUIRE(dev.state() == DeviceState::Idle);
}

TEST_CASE("camera1: initialize refuses when the platform reports zero cameras", "[camera1]") {
    Camera1Device dev;
    dev.set_platform_camera_count(0);          // the NDK-free device still has no camera: refuse
    REQUIRE_THROWS(dev.initialize());
    REQUIRE(dev.state() == DeviceState::Error);
    REQUIRE(dev.state() != DeviceState::Capturing);
}

TEST_CASE("camera1: with a camera present the lifecycle is Initialized -> Capturing -> Stopped",
          "[camera1]") {
    Camera1Device dev;
    dev.set_platform_camera_count(1);
    dev.initialize();
    REQUIRE(dev.state() == DeviceState::Initialized);
    REQUIRE_FALSE(dev.state() == DeviceState::Capturing);   // initialize is NOT capture
    dev.start();
    REQUIRE(dev.state() == DeviceState::Capturing);
    dev.stop();
    REQUIRE(dev.state() == DeviceState::Stopped);
    dev.close();
    REQUIRE(dev.state() == DeviceState::Idle);
    REQUIRE(dev.kind() == DeviceKind::camera);
    REQUIRE(dev.name().find("camera1") != std::string::npos);
}

TEST_CASE("camera1: repeated start/stop cycles are supported", "[camera1]") {
    Camera1Device dev;
    dev.set_platform_camera_count(2);
    dev.initialize();
    for (int i = 0; i < 3; ++i) {
        dev.start();
        REQUIRE(dev.state() == DeviceState::Capturing);
        dev.stop();
        REQUIRE(dev.state() == DeviceState::Stopped);
    }
    dev.close();
    REQUIRE(dev.state() == DeviceState::Idle);
}

TEST_CASE("camera1: a failed java start can never leave the device Capturing", "[camera1]") {
    Camera1Device dev;
    dev.set_platform_camera_count(1);
    dev.initialize();
    dev.start();
    REQUIRE(dev.state() == DeviceState::Capturing);

    dev.set_java_start_failed("Camera.open failed (device busy)");
    REQUIRE(dev.state() == DeviceState::Stopped);            // NOT Capturing
    REQUIRE(dev.last_java_failure().find("busy") != std::string::npos);

    // A later successful start clears the recorded failure.
    dev.start();
    REQUIRE(dev.state() == DeviceState::Capturing);
    REQUIRE(dev.last_java_failure().empty());
}

TEST_CASE("camera1: stop/close are idempotent and never throw", "[camera1]") {
    Camera1Device dev;
    dev.stop();
    dev.close();
    dev.close();
    REQUIRE(dev.state() == DeviceState::Idle);
    dev.set_platform_camera_count(1);
    dev.initialize();
    dev.start();
    dev.stop();
    dev.stop();
    REQUIRE(dev.state() == DeviceState::Stopped);
}

TEST_CASE("camera1: start from Idle or Error is refused", "[camera1]") {
    Camera1Device dev;
    dev.set_platform_camera_count(1);
    REQUIRE_THROWS(dev.start());          // Idle: not initialized
    dev.fail();
    REQUIRE(dev.state() == DeviceState::Error);
    REQUIRE_THROWS(dev.start());
}

TEST_CASE("camera1: the consent guard gates this backend exactly like the NDK one", "[camera1]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, /*media_capture_enabled=*/true);
    bool permission = true;
    Camera1Device dev;
    dev.set_platform_camera_count(1);
    ConsentGuardedDevice guarded(dev, gate, cam_request(), [&permission] { return permission; });

    // No consent at all: nothing may be initialized or started.
    const auto denied = guarded.initialize();
    REQUIRE_FALSE(denied.allowed);
    REQUIRE(guarded.state() != DeviceState::Capturing);

    repo.grant("node", "camera_acquire", {"camera"});
    REQUIRE(guarded.initialize().allowed);
    REQUIRE(guarded.start().allowed);
    REQUIRE(guarded.state() == DeviceState::Capturing);
    REQUIRE(guarded.authorize_delivery());          // frames may be delivered

    // Withdrawal forces the device out of capture at the next delivery check.
    repo.withdraw("node", "camera_acquire");
    REQUIRE_FALSE(guarded.authorize_delivery());
    REQUIRE(guarded.state() != DeviceState::Capturing);

    // Regrant, restart, then revoke the OS permission instead: same outcome.
    repo.grant("node", "camera_acquire", {"camera"});
    REQUIRE(guarded.start().allowed);
    REQUIRE(guarded.state() == DeviceState::Capturing);
    permission = false;
    REQUIRE_FALSE(guarded.authorize_delivery());
    REQUIRE(guarded.state() != DeviceState::Capturing);
}

TEST_CASE("camera1: consecutive grants still authorize (the H-03 consent regression holds here too)",
          "[camera1]") {
    SingleActiveGrantConsentRepo repo;
    ConsentGate gate(repo, true);
    for (int i = 0; i < 3; ++i) repo.grant("node", "camera_acquire", {"camera"});
    Camera1Device dev;
    dev.set_platform_camera_count(1);
    ConsentGuardedDevice guarded(dev, gate, cam_request());
    const auto r = guarded.initialize();
    REQUIRE(r.allowed);
    REQUIRE(r.deny_reason == DenyReason::none);
    REQUIRE(guarded.start().allowed);
}
