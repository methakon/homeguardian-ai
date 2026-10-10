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

static const std::string FAKE_DB = "test_fake_delivery.db";

struct FakeDeliveryFixture {
    std::shared_ptr<SQLiteDatabase> db;
    std::unique_ptr<ConsentRepository> repo;
    FakeDeliveryFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(FAKE_DB);
        std::filesystem::remove(FAKE_DB + "-wal");
        std::filesystem::remove(FAKE_DB + "-shm");
        db = std::make_shared<SQLiteDatabase>();
        db->open(FAKE_DB);
        SchemaManager schema(db);
        schema.initialize();
        repo = std::make_unique<ConsentRepository>(db);
    }
    ~FakeDeliveryFixture() {
        repo.reset();
        if (db && db->is_open()) db->close();
        db.reset();
        std::filesystem::remove(FAKE_DB);
        std::filesystem::remove(FAKE_DB + "-wal");
        std::filesystem::remove(FAKE_DB + "-shm");
    }
};

// Helper: simulate a native capture callback loop that MUST gate every delivery.
// Returns the number of payloads actually delivered to "processing".
static int run_capture_loop(ConsentGuardedDevice& guard, FakeMediaDevice& dev, int frames) {
    int delivered = 0;
    for (int i = 0; i < frames; ++i) {
        // The delivery gate is the TOCTOU-closing check, immediately before the
        // payload would be handed to processing.
        if (!guard.authorize_delivery()) {
            break;  // deny: drop payload, stop the loop
        }
        dev.deliver_payload();
        delivered++;
    }
    return delivered;
}

TEST_CASE("Delivery gate: authorized frames are delivered", "[f22][delivery]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    REQUIRE(dev.state() == DeviceState::Capturing);

    int delivered = run_capture_loop(guard, dev, 5);
    REQUIRE(delivered == 5);
    REQUIRE(dev.delivered().size() == 5);
}

TEST_CASE("Delivery gate: consent withdrawal mid-capture stops delivery", "[f22][delivery]") {
    FakeDeliveryFixture f;
    auto now = std::chrono::system_clock::now();
    f.repo->save(ConsentRecord("g1", "p1", "presence", {"camera"}, ConsentDecision::granted, "1.0.0",
                               now - std::chrono::hours(1), std::nullopt, "user", 1));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);

    // Deliver 2 frames while authorized.
    REQUIRE(run_capture_loop(guard, dev, 2) == 2);
    REQUIRE(dev.delivered().size() == 2);

    // Withdraw consent.
    f.repo->save(ConsentRecord::create_withdrawn("w1", "p1", "presence", "1.0.0", "user"));

    // The very next delivery attempt must be denied and force stop.
    REQUIRE_FALSE(guard.authorize_delivery(now));
    REQUIRE(dev.state() != DeviceState::Capturing);
    REQUIRE(dev.stop_count() == 1);

    // No further frames can be delivered.
    REQUIRE(run_capture_loop(guard, dev, 5) == 0);
    REQUIRE(dev.delivered().size() == 2);  // unchanged
}

TEST_CASE("Delivery gate: permission revocation mid-capture stops delivery", "[f22][delivery]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    std::atomic<bool> permission{true};
    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"},
                               [&permission] { return permission.load(); });

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    REQUIRE(run_capture_loop(guard, dev, 3) == 3);

    permission.store(false);  // OS revokes camera permission
    REQUIRE_FALSE(guard.authorize_delivery());
    REQUIRE(dev.state() != DeviceState::Capturing);
    REQUIRE(run_capture_loop(guard, dev, 5) == 0);
    REQUIRE(dev.delivered().size() == 3);
}

TEST_CASE("Delivery gate: expired consent stops delivery", "[f22][delivery]") {
    FakeDeliveryFixture f;
    auto now = std::chrono::system_clock::now();
    // Grant valid now, but we check delivery after its expiry instant.
    f.repo->save(ConsentRecord("g1", "p1", "presence", {"camera"}, ConsentDecision::granted, "1.0.0",
                               now - std::chrono::hours(1), now + std::chrono::hours(1), "user", 1));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    REQUIRE(run_capture_loop(guard, dev, 2) == 2);

    // Delivery check at a time after expiry must deny and force stop.
    auto after_expiry = now + std::chrono::hours(2);
    REQUIRE_FALSE(guard.authorize_delivery(after_expiry));
    REQUIRE(dev.state() != DeviceState::Capturing);
    REQUIRE(dev.delivered().size() == 2);
}

TEST_CASE("Delivery gate: device failure stops delivery", "[f22][delivery]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

    REQUIRE(guard.initialize().allowed);
    REQUIRE(guard.start().allowed);
    REQUIRE(run_capture_loop(guard, dev, 1) == 1);

    dev.fail();
    REQUIRE(dev.state() == DeviceState::Error);
    REQUIRE_FALSE(guard.authorize_delivery());
    REQUIRE(run_capture_loop(guard, dev, 5) == 0);
    REQUIRE(dev.delivered().size() == 1);
}

TEST_CASE("Lifecycle: repeated start/stop/close releases resources each cycle", "[f22][lifecycle]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));

    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});

    for (int cycle = 0; cycle < 3; ++cycle) {
        REQUIRE(guard.initialize().allowed);
        REQUIRE(guard.start().allowed);
        REQUIRE(dev.state() == DeviceState::Capturing);
        REQUIRE(run_capture_loop(guard, dev, 2) == 2);
        guard.stop();
        REQUIRE(dev.state() == DeviceState::Stopped);
        guard.close();
        REQUIRE(dev.state() == DeviceState::Idle);
        REQUIRE(dev.released());  // close() released resources
    }
    REQUIRE(dev.init_count() == 3);
    REQUIRE(dev.start_count() == 3);
    REQUIRE(dev.stop_count() == 3);
    REQUIRE(dev.close_count() == 3);
}

TEST_CASE("Lifecycle: destructor releases a still-capturing device", "[f22][lifecycle]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));
    ConsentGate gate(*f.repo, true);

    {
        FakeMediaDevice dev(DeviceKind::camera, "fake-cam");
        ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});
        REQUIRE(guard.initialize().allowed);
        REQUIRE(guard.start().allowed);
        REQUIRE(dev.state() == DeviceState::Capturing);
        // Leave capturing; destructor must stop+close.
    }
    SUCCEED("capturing device released on scope exit without error");
}

TEST_CASE("Initialization failure: device in error state cannot initialize", "[f22][lifecycle]") {
    FakeDeliveryFixture f;
    f.repo->save(ConsentRecord::create_granted("g1", "p1", "presence", {"camera"}, "1.0.0", "user"));
    ConsentGate gate(*f.repo, true);
    FakeMediaDevice dev(DeviceKind::camera, "fake-cam");

    dev.fail();
    REQUIRE(dev.state() == DeviceState::Error);
    REQUIRE_THROWS_AS(dev.initialize(), std::runtime_error);

    ConsentGuardedDevice guard(dev, gate, {"p1", "presence", "camera"});
    // Even with consent, an errored device cannot be initialized/started.
    REQUIRE_FALSE(guard.initialize().allowed);
    REQUIRE_FALSE(guard.start().allowed);
}
