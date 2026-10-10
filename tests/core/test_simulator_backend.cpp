#include "catch2/catch.hpp"
#include "backend/simulator/SimulatedSensorBackend.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/IConsentRepository.h"
#include <atomic>
#include <chrono>

using namespace homeguardian;

// Simulator-first tests. These run entirely in-process against the deterministic
// SimulatedSensorBackend behind the SAME ConsentGuardedDevice gate used by the
// Android/Ubuntu backends. No real sensor is opened; no device file, no
// hardware syscall, no media file.
//
// Validated here (host): lifecycle, consent denial/withdrawal/expiry,
// delivery-boundary authorization, permission revocation, bounded buffers,
// error recovery, and shutdown/cleanup.

// Minimal in-memory consent repository.
struct MemRepo : IConsentRepository {
    std::vector<ConsentRecord> recs;
    void save(const ConsentRecord& r) override { recs.push_back(r); }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
        std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s) o.push_back(r); return o; }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        std::vector<ConsentRecord> o; for (auto& r : recs) if (r.get_subject_scope()==s && r.get_purpose()==p) o.push_back(r); return o; }
    size_t count() override { return recs.size(); }
    size_t delete_all() override { size_t n=recs.size(); recs.clear(); return n; }
};

// Helper: grant an active consent record (no expiry).
static void grant(MemRepo& repo, const std::string& cat) {
    repo.save(ConsentRecord::create_granted("g-"+cat, "harness", "sim", {cat}, "1.0.0", "operator"));
}

// Deliver N payloads through the guard; return how many were delivered.
static int deliver_through_gate(ConsentGuardedDevice& guard, SimulatedSensorBackend& sim, int n) {
    int delivered = 0;
    for (int i = 0; i < n; ++i) {
        if (!guard.authorize_delivery()) break;
        auto payload = sim.generate_frame();
        sim.record_delivered(std::move(payload));
        delivered++;
    }
    return delivered;
}

TEST_CASE("Simulator: deterministic frame generation is reproducible",
          "[sim][determinism]") {
    Logger::initialize("error");
    SimulatedSensorBackend a(DeviceKind::camera, "cam", 16, 16, 0, 0, /*seed=*/1234);
    SimulatedSensorBackend b(DeviceKind::camera, "cam", 16, 16, 0, 0, /*seed=*/1234);
    auto fa = a.generate_frame();
    auto fb = b.generate_frame();
    REQUIRE(fa.size() == 16 * 16);
    REQUIRE(fa == fb);  // same seed -> identical bytes
    REQUIRE(a.sequence() == 1);
}

TEST_CASE("Simulator: device lifecycle transitions and idempotency",
          "[sim][lifecycle]") {
    Logger::initialize("error");
    SimulatedSensorBackend sim(DeviceKind::audio, "mic", 0, 0, 16000, 320);
    REQUIRE(sim.state() == DeviceState::Idle);

    sim.initialize();
    REQUIRE(sim.state() == DeviceState::Initialized);

    sim.start();
    REQUIRE(sim.state() == DeviceState::Capturing);

    sim.stop();
    REQUIRE(sim.state() == DeviceState::Stopped);

    // Repeated stop is a safe no-op.
    sim.stop();
    REQUIRE(sim.stop_count() == 1);

    sim.close();
    REQUIRE(sim.state() == DeviceState::Idle);
    REQUIRE(sim.released());

    // Repeated close from Idle is safe.
    sim.close();
    REQUIRE(sim.close_count() == 2);
}

TEST_CASE("Simulator: consent denial (capture disabled) blocks start; nothing delivered",
          "[sim][consent][denial]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    // media_capture_enabled=false = shipped default -> deny even with a grant.
    ConsentGate gate(repo, /*media_capture_enabled=*/false);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE_FALSE(guard.initialize());
    REQUIRE_FALSE(guard.start());
    REQUIRE(sim.state() == DeviceState::Idle);
    REQUIRE(sim.init_count() == 0);
    REQUIRE(sim.start_count() == 0);
    REQUIRE(deliver_through_gate(guard, sim, 5) == 0);
    REQUIRE(sim.buffer_size() == 0);
}

TEST_CASE("Simulator: authorized delivery increments buffer while consent active",
          "[sim][consent][deliver]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 10) == 10);
    REQUIRE(sim.buffer_size() == 10);
}

TEST_CASE("Simulator: consent withdrawal stops delivery at the boundary",
          "[sim][consent][withdraw]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 6) == 6);

    // Withdraw while capture is active.
    repo.save(ConsentRecord::create_withdrawn("w-camera","harness","sim","1.0.0","operator"));

    int after = deliver_through_gate(guard, sim, 10);
    REQUIRE(after == 0);
    REQUIRE(sim.state() != DeviceState::Capturing);
    REQUIRE(sim.buffer_size() == 6);  // no extra payloads delivered
}

TEST_CASE("Simulator: consent expiry stops delivery at the boundary",
          "[sim][consent][expiry]") {
    Logger::initialize("error");
    MemRepo repo;
    auto now = std::chrono::system_clock::now();
    // Grant that expired one hour ago.
    repo.save(ConsentRecord("g-exp","harness","sim",{"camera"},ConsentDecision::granted,
                            "1.0.0", now - std::chrono::hours(2),
                            now - std::chrono::hours(1), "operator", 1));
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE_FALSE(guard.initialize());
    REQUIRE_FALSE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 5) == 0);
    REQUIRE(sim.buffer_size() == 0);
}

TEST_CASE("Simulator: permission revocation stops delivery at the boundary",
          "[sim][consent][permission]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "audio");
    std::atomic<bool> permission{true};
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::audio, "mic", 0, 0, 16000, 320);
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","audio"},
                               [&permission]{ return permission.load(); });

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 4) == 4);

    // OS/sound-server revokes access mid-capture.
    permission.store(false);
    int after = deliver_through_gate(guard, sim, 10);
    REQUIRE(after == 0);
    REQUIRE(sim.state() != DeviceState::Capturing);
    REQUIRE(sim.buffer_size() == 4);
}

TEST_CASE("Simulator: bounded buffer never grows without bound",
          "[sim][bounded]") {
    Logger::initialize("error");
    // capacity defaults to 256.
    SimulatedSensorBackend sim(DeviceKind::camera, "cam", 8, 8);
    REQUIRE(sim.buffer_capacity() == 256);

    // Push far more than capacity; the ring must stay bounded and drop oldest.
    for (int i = 0; i < 1000; ++i) {
        auto p = sim.generate_frame();
        sim.record_delivered(std::move(p));
    }
    REQUIRE(sim.buffer_size() == 256);
    REQUIRE(sim.buffer_dropped() == 1000 - 256);
}

TEST_CASE("Simulator: device error denies delivery and forces release",
          "[sim][error]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 3) == 3);

    sim.fail();
    int after = deliver_through_gate(guard, sim, 10);
    REQUIRE(after == 0);
    REQUIRE(sim.state() == DeviceState::Error);
}

TEST_CASE("Simulator: error recovery via close + re-init allows restart",
          "[sim][error][recovery]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 2) == 2);

    sim.fail();
    // Denied at the boundary; guard closes the errored device (releases the
    // acquisition buffer) and keeps it in Error (no resurrection).
    REQUIRE(deliver_through_gate(guard, sim, 5) == 0);
    REQUIRE(sim.state() == DeviceState::Error);
    REQUIRE(sim.buffer_size() == 0);

    sim.recover();  // deterministic cleanup + clear error -> Idle
    REQUIRE(sim.state() == DeviceState::Idle);

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 2) == 2);
    REQUIRE(sim.buffer_size() == 2);  // only the post-recovery payloads
}

TEST_CASE("Simulator: shutdown releases resources and clears buffer",
          "[sim][shutdown]") {
    Logger::initialize("error");
    MemRepo repo;
    grant(repo, "camera");
    ConsentGate gate(repo, true);
    SimulatedSensorBackend sim(DeviceKind::camera, "cam");
    ConsentGuardedDevice guard(sim, gate, {"harness","sim","camera"});

    REQUIRE(guard.initialize());
    REQUIRE(guard.start());
    REQUIRE(deliver_through_gate(guard, sim, 20) == 20);
    REQUIRE(sim.buffer_size() == 20);

    guard.stop();
    guard.close();
    REQUIRE(sim.state() == DeviceState::Idle);
    REQUIRE(sim.released());
    REQUIRE(sim.buffer_size() == 0);  // buffer released on close
    REQUIRE(sim.stop_count() == 1);
    REQUIRE(sim.close_count() == 1);
}

TEST_CASE("Simulator: audio payloads are PCM-sized and deterministic",
          "[sim][audio]") {
    Logger::initialize("error");
    SimulatedSensorBackend sim(DeviceKind::audio, "mic", 0, 0, 16000, 320, /*seed=*/99);
    auto p1 = sim.generate_frame();
    SimulatedSensorBackend sim2(DeviceKind::audio, "mic", 0, 0, 16000, 320, /*seed=*/99);
    auto p2 = sim2.generate_frame();
    REQUIRE(p1.size() == 320 * 2);  // 16-bit PCM mono
    REQUIRE(p1 == p2);
}
