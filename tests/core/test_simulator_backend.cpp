#include "catch2/catch.hpp"
#include "backend/simulator/SimulatedSensorBackend.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/IConsentRepository.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <vector>

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

// ---------------------------------------------------------------------------
// Realistic Linux sensor simulation (config, pacing, signals, speaker)
// ---------------------------------------------------------------------------

TEST_CASE("Simulator: configurable camera dimensions produce matching frame size",
          "[sim][camera][config]") {
    Logger::initialize("error");
    SimulatedSensorBackend sim(DeviceKind::camera, "cam",
                               SimulatedSensorBackend::Config{/*width*/128,
                                                              /*height*/96});
    auto f = sim.generate_frame();
    REQUIRE(f.size() == static_cast<size_t>(128 * 96));
}

TEST_CASE("Simulator: frame-rate pacing produces ~requested rate (loose bound)",
          "[sim][camera][framerate]") {
    Logger::initialize("error");
    // 50 fps for 10 frames should take roughly 9 intervals (~0.18s), far more
    // than an unpaced loop. Assert a generous lower bound to avoid flakiness.
    SimulatedSensorBackend sim(DeviceKind::camera, "cam",
                               SimulatedSensorBackend::Config{64, 48, /*frame_rate*/50.0});
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) (void)sim.generate_frame();
    auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(dt >= 0.10);  // ~9 intervals at 50fps ≈ 0.18s; bound is loose
}

TEST_CASE("Simulator: configurable ring capacity bounds the buffer",
          "[sim][bounded][config]") {
    Logger::initialize("error");
    SimulatedSensorBackend sim(DeviceKind::camera, "cam",
                               SimulatedSensorBackend::Config{8, 8, 0.0, 0, 0, 1,
                                                              SimulatedSensorBackend::TestSignal::noise,
                                                              440.0, /*ring_capacity*/16});
    REQUIRE(sim.buffer_capacity() == 16);
    for (int i = 0; i < 100; ++i) {
        auto p = sim.generate_frame();
        sim.record_delivered(std::move(p));
    }
    REQUIRE(sim.buffer_size() == 16);
    REQUIRE(sim.buffer_dropped() == 100 - 16);
}

TEST_CASE("Simulator: stereo audio doubles the payload size",
          "[sim][audio][channels]") {
    Logger::initialize("error");
    SimulatedSensorBackend mono(DeviceKind::audio, "mic",
                                SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, /*channels*/1});
    SimulatedSensorBackend stereo(DeviceKind::audio, "mic",
                                  SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, /*channels*/2});
    REQUIRE(mono.audio_frame_bytes() == 320 * 1 * 2);
    REQUIRE(stereo.audio_frame_bytes() == 320 * 2 * 2);
    auto pm = mono.generate_frame();
    auto ps = stereo.generate_frame();
    REQUIRE(pm.size() == 320 * 1 * 2);
    REQUIRE(ps.size() == 320 * 2 * 2);
}

TEST_CASE("Simulator: silence signal yields all-zero samples",
          "[sim][audio][signal]") {
    Logger::initialize("error");
    SimulatedSensorBackend sim(DeviceKind::audio, "mic",
                               SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                              SimulatedSensorBackend::TestSignal::silence});
    auto p = sim.generate_frame();
    bool all_zero = true;
    for (uint8_t b : p) {
        if (b != 0) { all_zero = false; break; }
    }
    REQUIRE(all_zero);
}

TEST_CASE("Simulator: sine signal is deterministic and bounded",
          "[sim][audio][signal]") {
    Logger::initialize("error");
    SimulatedSensorBackend::Config cfg{0, 0, 0.0, 16000, 320, 1,
                                       SimulatedSensorBackend::TestSignal::sine, 440.0};
    SimulatedSensorBackend a(DeviceKind::audio, "mic", cfg);
    SimulatedSensorBackend b(DeviceKind::audio, "mic", cfg);
    auto pa = a.generate_frame();
    auto pb = b.generate_frame();
    REQUIRE(pa == pb);  // deterministic
    // Every 16-bit sample must be within the amplitude bound.
    for (size_t i = 0; i + 1 < pa.size(); i += 2) {
        int16_t v;
        std::memcpy(&v, &pa[i], 2);
        REQUIRE(v <= 12000);
        REQUIRE(v >= -12000);
    }
}

TEST_CASE("Simulator: square signal alternates polarity at the tone rate",
          "[sim][audio][signal]") {
    Logger::initialize("error");
    // 50 Hz at 16 kHz -> 320 samples/period -> 160 samples per half cycle.
    // Square wave should be constant within a half cycle and flip between
    // half cycles.
    SimulatedSensorBackend sim(DeviceKind::audio, "mic",
                               SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                              SimulatedSensorBackend::TestSignal::square,
                                                              /*tone_hz*/50.0});
    auto p = sim.generate_frame();
    auto sample = [&](size_t idx) {
        int16_t v;
        std::memcpy(&v, &p[idx * 2], 2);
        return v;
    };
    // Within the first half cycle (samples 0..159) sign is constant.
    int first_sign = (sample(0) >= 0) ? 1 : -1;
    for (size_t i = 1; i < 160; ++i) {
        REQUIRE(((sample(i) >= 0) ? 1 : -1) == first_sign);
    }
    // In the next half cycle (samples 160..319) the sign has flipped.
    REQUIRE(((sample(200) >= 0) ? 1 : -1) == -first_sign);
}

TEST_CASE("Simulator: speaker playback succeeds when initialized",
          "[sim][speaker]") {
    Logger::initialize("error");
    SimulatedSensorBackend spk(DeviceKind::audio, "spk",
                               SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                              SimulatedSensorBackend::TestSignal::sine});
    spk.initialize();
    auto p = spk.generate_frame();
    REQUIRE(spk.play(p));
    REQUIRE(spk.playing());
    REQUIRE(spk.played_count() == 1);
    // Playback is output-only: it must NOT touch the capture buffer.
    REQUIRE(spk.buffer_size() == 0);
}

TEST_CASE("Simulator: speaker playback fails when not initialized",
          "[sim][speaker]") {
    Logger::initialize("error");
    SimulatedSensorBackend spk(DeviceKind::audio, "spk");
    std::vector<uint8_t> p(8, 0);
    REQUIRE_FALSE(spk.play(p));  // Idle -> not initialized
    REQUIRE_FALSE(spk.playing());
    REQUIRE(spk.played_count() == 0);
}

TEST_CASE("Simulator: injected playback error makes play fail (recoverable)",
          "[sim][speaker][error]") {
    Logger::initialize("error");
    SimulatedSensorBackend spk(DeviceKind::audio, "spk");
    spk.initialize();
    spk.inject_playback_error();
    std::vector<uint8_t> p(8, 0);
    REQUIRE_FALSE(spk.play(p));
    // Recover via close + re-init.
    spk.close();
    spk.initialize();
    REQUIRE(spk.play(p));
    REQUIRE(spk.played_count() == 1);
}

TEST_CASE("Simulator: speaker play is independent of capture consent gate",
          "[sim][speaker][consent]") {
    Logger::initialize("error");
    MemRepo repo;  // no consent granted at all
    ConsentGate gate(repo, true);
    SimulatedSensorBackend spk(DeviceKind::audio, "spk");
    ConsentGuardedDevice guard(spk, gate, {"harness", "sim", "audio"});

    // Capture start is denied without consent...
    REQUIRE_FALSE(guard.start());
    // ...but playback (output) still works once the device is initialized
    // directly, because play() is not a capture path and has no delivery gate.
    spk.initialize();
    std::vector<uint8_t> p(8, 0);
    REQUIRE(spk.play(p));
    REQUIRE(spk.played_count() == 1);
}
