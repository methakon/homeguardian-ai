#include "catch2/catch.hpp"
#include "core/IMediaDevice.h"
#include "core/FakeMediaDevice.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/SQLiteDatabase.h"
#include "persistence/SchemaManager.h"
#include "persistence/ConsentRepository.h"
#include <filesystem>
#include <chrono>
#include <atomic>

using namespace homeguardian;

// Host-side tests for the Camera1-JNI adapter contract.
//
// The Camera1 backend (apk/.../Camera1Bridge.java + nativeOnPreviewFrame in
// hg_harness_jni.cpp) delegates its per-frame consent decision to
// ConsentGuardedDevice::authorize_delivery(), exactly as these tests exercise.
// We verify the contract the Camera1 path depends on:
//   1. authorize_delivery() returns true while consent is granted, letting the
//      frame be processed and counted.
//   2. authorize_delivery() returns false the instant consent is withdrawn,
//      forcing capture to stop — the frame is dropped and delivery count stops.
//   3. Permission revocation (via the permission callback) denies delivery.
//   4. Device error denies delivery and forces release.
//   5. Repeated start/stop/close cycles are idempotent and clean.
// These are host tests of the shared consent/lifecycle core; they do NOT run
// real Camera1 capture (that requires the device and separate approval).

static const std::string C1_DB = "test_camera1_adapter.db";

struct Camera1AdapterFixture {
    std::shared_ptr<SQLiteDatabase> db;
    std::unique_ptr<ConsentRepository> repo;
    Camera1AdapterFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(C1_DB);
        std::filesystem::remove(C1_DB + "-wal");
        std::filesystem::remove(C1_DB + "-shm");
        db = std::make_shared<SQLiteDatabase>();
        db->open(C1_DB);
        SchemaManager schema(db);
        schema.initialize();
        repo = std::make_unique<ConsentRepository>(db);
    }
    ~Camera1AdapterFixture() {
        repo.reset();
        if (db && db->is_open()) db->close();
        db.reset();
        std::filesystem::remove(C1_DB);
        std::filesystem::remove(C1_DB + "-wal");
        std::filesystem::remove(C1_DB + "-shm");
    }
};

// Models the Camera1 onPreviewFrame -> nativeOnPreviewFrame -> delivery gate
// loop. Returns frames delivered to "processing" and whether capture was told
// to stop.
static int run_camera1_callback_loop(ConsentGuardedDevice& guard, FakeMediaDevice& dev,
                                     int frames, bool& stopped) {
    int delivered = 0;
    stopped = false;
    for (int i = 0; i < frames; ++i) {
        // This mirrors nativeOnPreviewFrame: the gate decides per frame.
        if (!guard.authorize_delivery()) {
            stopped = true;  // native returns JNI_FALSE; bridge stops preview
            break;
        }
        dev.deliver_payload();
        delivered++;
    }
    return delivered;
}

TEST_CASE("Camera1 adapter: delivery gate authorizes frames while consent granted",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "harness", "camera_test", {"camera"}, "1.0.0", "operator"));

    ConsentGate gate(*f.repo, /*media_capture_enabled=*/true);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    REQUIRE(dev.state() == DeviceState::Capturing);

    bool stopped = false;
    int delivered = run_camera1_callback_loop(guard, dev, 10, stopped);
    REQUIRE(delivered == 10);
    REQUIRE_FALSE(stopped);
    REQUIRE(dev.delivered().size() == 10);
}

TEST_CASE("Camera1 adapter: consent withdrawal drops subsequent frames and stops capture",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    auto now = std::chrono::system_clock::now();
    f.repo->save(ConsentRecord("g1", "harness", "camera_test", {"camera"}, ConsentDecision::granted,
                               "1.0.0", now - std::chrono::hours(1), std::nullopt, "operator", 1));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);

    bool stopped = false;
    REQUIRE(run_camera1_callback_loop(guard, dev, 5, stopped) == 5);

    // Operator withdraws consent while capture is active.
    f.repo->save(ConsentRecord::create_withdrawn("w1", "harness", "camera_test", "1.0.0", "operator"));

    // The very next preview frame must be denied and signal stop.
    stopped = false;
    int after = run_camera1_callback_loop(guard, dev, 10, stopped);
    REQUIRE(after == 0);
    REQUIRE(stopped);
    REQUIRE(dev.state() != DeviceState::Capturing);
    REQUIRE(dev.delivered().size() == 5);  // no extra frames delivered
}

TEST_CASE("Camera1 adapter: permission revocation denies delivery and stops capture",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "harness", "camera_test", {"camera"}, "1.0.0", "operator"));

    std::atomic<bool> permission{true};
    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"},
                               [&permission] { return permission.load(); });

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    bool stopped = false;
    REQUIRE(run_camera1_callback_loop(guard, dev, 4, stopped) == 4);

    // OS revokes CAMERA.
    permission.store(false);
    stopped = false;
    int after = run_camera1_callback_loop(guard, dev, 10, stopped);
    REQUIRE(after == 0);
    REQUIRE(stopped);
    REQUIRE(dev.state() != DeviceState::Capturing);
}

TEST_CASE("Camera1 adapter: device error denies delivery and forces release",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "harness", "camera_test", {"camera"}, "1.0.0", "operator"));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    bool stopped = false;
    REQUIRE(run_camera1_callback_loop(guard, dev, 2, stopped) == 2);

    dev.fail();
    stopped = false;
    int after = run_camera1_callback_loop(guard, dev, 10, stopped);
    REQUIRE(after == 0);
    REQUIRE(stopped);
    // Device stays in Error until explicitly re-initialized (no resurrection).
    REQUIRE(dev.state() == DeviceState::Error);
}

TEST_CASE("Camera1 adapter: repeated start/stop/close is idempotent and clean",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "harness", "camera_test", {"camera"}, "1.0.0", "operator"));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"});

    for (int cycle = 0; cycle < 3; ++cycle) {
        REQUIRE(guard.initialize().allowed);
        REQUIRE(guard.start().allowed);
        bool stopped = false;
        REQUIRE(run_camera1_callback_loop(guard, dev, 3, stopped) == 3);
        guard.stop();
        guard.close();
        REQUIRE(dev.state() == DeviceState::Idle);
    }
    REQUIRE(dev.init_count() == 3);
    REQUIRE(dev.start_count() == 3);
    REQUIRE(dev.stop_count() == 3);
    REQUIRE(dev.close_count() == 3);
    REQUIRE(dev.released());
}

TEST_CASE("Camera1 adapter: capture disabled by default (config kill switch) blocks start",
          "[f22][camera1][adapter]") {
    Camera1AdapterFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "harness", "camera_test", {"camera"}, "1.0.0", "operator"));

    // media_capture_enabled = false (the shipped default) denies everything.
    ConsentGate gate(*f.repo, /*media_capture_enabled=*/false);
    FakeMediaDevice dev(DeviceKind::camera, "camera1-model");
    ConsentGuardedDevice guard(dev, gate, {"harness", "camera_test", "camera"});

    REQUIRE_FALSE(guard.initialize().allowed);
    REQUIRE_FALSE(guard.start().allowed);
    REQUIRE(dev.state() == DeviceState::Idle);
    REQUIRE(dev.start_count() == 0);
}
