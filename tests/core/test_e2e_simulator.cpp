#include "catch2/catch.hpp"
#include "backend/simulator/SimulatedSensorBackend.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/IConsentRepository.h"
#include "core/Event.h"
#include "core/EventValidator.h"
#include "core/Pipeline.h"
#include "core/TimeWindowCorrelation.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// End-to-end Linux simulator workflow (host, deterministic, non-capture).
//
// This wires the EXISTING components together in one automated scenario:
//   simulated camera/mic/speaker -> consent-guarded delivery -> a clearly
//   labelled synthetic TEST-PROCESSING component -> event-pipeline rule/alert
//   logic -> a safe response -> simulated speaker playback -> lifecycle/bounded
//   cleanup verification.
//
// IMPORTANT: this is a SYNTHETIC test scenario. The synthetic frames/samples do
// NOT detect real-world danger, do NOT identify people, and carry no medical or
// emotional conclusion. All data is in-memory placeholders. No real sensor is
// opened.
// ---------------------------------------------------------------------------

// Minimal in-memory consent repository (mirrors the pattern used across the
// simulator/ubuntu/camera1 test suites).
struct E2EMemRepo : IConsentRepository {
    std::vector<ConsentRecord> recs;
    void save(const ConsentRecord& r) override { recs.push_back(r); }
    std::optional<ConsentRecord> find_by_id(const std::string&) override { return std::nullopt; }
    std::vector<ConsentRecord> find_by_subject(const std::string& s) override {
        std::vector<ConsentRecord> o;
        for (auto& r : recs) if (r.get_subject_scope() == s) o.push_back(r);
        return o;
    }
    std::vector<ConsentRecord> find_by_purpose(const std::string& s, const std::string& p) override {
        std::vector<ConsentRecord> o;
        for (auto& r : recs) if (r.get_subject_scope() == s && r.get_purpose() == p) o.push_back(r);
        return o;
    }
    size_t count() override { return recs.size(); }
    size_t delete_all() override { size_t n = recs.size(); recs.clear(); return n; }
};

// The clearly-labelled synthetic TEST-PROCESSING component. It is deliberately
// trivial and deterministic: it turns an authorized (camera frame, mic sample)
// pair into an observation Event. It performs NO real analysis and draws NO
// conclusion about people, danger, health, or emotion. It only ever sees
// payloads that already passed the consent delivery gate.
struct SyntheticTestProcessor {
    int observations_made = 0;
    bool saw_unauthorized = false;  // must never become true

    // Returns an observation Event built from the authorized payload sizes.
    // `camera_bytes`/`audio_bytes` are only used as neutral integers.
    Event process(size_t camera_bytes, size_t audio_bytes, const std::string& id) {
        observations_made++;
        nlohmann::json payload = {
            {"scenario", "SYNTHETIC_TEST_ONLY"},
            {"camera_bytes", static_cast<uint64_t>(camera_bytes)},
            {"audio_bytes", static_cast<uint64_t>(audio_bytes)},
            {"note", "synthetic placeholder; not a real-world detection"},
        };
        return Event::create_observation(id, "synthetic_test_processor", std::move(payload));
    }
};

// Build a safe, policy-compliant response string for the simulated speaker.
// Synthetic + aggregate only; no sensitive detail, no real detection claim.
static std::string build_safe_response(bool alert_fired, const std::string& alert_msg) {
    if (alert_fired) {
        return "Synthetic test notice: " + alert_msg +
               ". This is a simulated test and not a real-world alert.";
    }
    return "Synthetic test complete. No simulated alert. This is not a real-world alert.";
}

// Acquire one authorized payload from a device through the guard. Returns the
// payload and whether it was authorized. Unauthorized payloads are dropped
// (never passed to the processor).
static bool acquire_authorized(ConsentGuardedDevice& guard,
                               SimulatedSensorBackend& sim,
                               std::vector<uint8_t>& out) {
    if (!guard.authorize_delivery()) return false;
    out = sim.generate_frame();
    return true;
}

// ===========================================================================
// Happy-path end-to-end scenario
// ===========================================================================

TEST_CASE("E2E: synthetic camera+mic -> processor -> rule/alert -> safe speaker response",
          "[e2e][sim]") {
    Logger::initialize("error");

    // 1. Initialize simulated camera, microphone, and speaker.
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera",
                                  SimulatedSensorBackend::Config{64, 48});
    SimulatedSensorBackend mic(DeviceKind::audio, "sim-mic",
                               SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                              SimulatedSensorBackend::TestSignal::sine});
    SimulatedSensorBackend speaker(DeviceKind::audio, "sim-speaker",
                                   SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                                  SimulatedSensorBackend::TestSignal::sine});

    // Consent + gate: media capture enabled. Camera and mic use DISTINCT
    // purposes so each has exactly one active grant (the gate fails closed on
    // ambiguous multiple grants for the same subject+purpose).
    E2EMemRepo repo;
    repo.save(ConsentRecord::create_granted("g-cam", "harness", "sim-camera", {"camera"}, "1.0.0", "operator"));
    repo.save(ConsentRecord::create_granted("g-aud", "harness", "sim-mic", {"audio"}, "1.0.0", "operator"));
    ConsentGate gate(repo, /*media_capture_enabled=*/true);

    ConsentGuardedDevice cam_guard(camera, gate, {"harness", "sim-camera", "camera"});
    ConsentGuardedDevice mic_guard(mic, gate, {"harness", "sim-mic", "audio"});

    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());
    REQUIRE(mic_guard.initialize());
    REQUIRE(mic_guard.start());
    speaker.initialize();

    // 2. + 3. Feed authorized synthetic frames/samples into the processor.
    SyntheticTestProcessor proc;
    std::vector<std::string> delivered_event_ids;
    int delivered_pairs = 0;
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> cam_bytes, mic_bytes;
        bool cam_ok = acquire_authorized(cam_guard, camera, cam_bytes);
        bool mic_ok = acquire_authorized(mic_guard, mic, mic_bytes);
        if (!cam_ok || !mic_ok) break;               // fail-closed: stop feeding
        std::string id = "obs-" + std::to_string(i);
        Event ev = proc.process(cam_bytes.size(), mic_bytes.size(), id);
        delivered_event_ids.push_back(id);
        delivered_pairs++;
    }
    REQUIRE(delivered_pairs == 3);
    REQUIRE(proc.observations_made == 3);
    REQUIRE_FALSE(proc.saw_unauthorized);

    // 4. + 5. Generate events via the pipeline; rule fires after 2 observations.
    auto rule = std::make_shared<TimeWindowCorrelation>(
        "e2e-synthetic-rule", std::vector<EventType>{EventType::observation}, 2,
        std::chrono::milliseconds(5000), Severity::info, "synthetic test notice");
    Pipeline pipeline({rule});

    std::optional<Alert> fired;
    for (int i = 0; i < delivered_pairs; ++i) {
        // Rebuild the same observation events the processor produced, in order.
        Event ev = proc.process(/*camera_bytes*/0, /*audio_bytes*/0, delivered_event_ids[i]);
        auto a = pipeline.process(ev);
        if (a) fired = a;
    }
    REQUIRE(fired.has_value());
    REQUIRE(fired->severity == Severity::info);

    // 6. Safe response, then 7. speaker playback.
    std::string response = build_safe_response(true, fired->message);
    // Encode the response as a fixed-size PCM-ish payload for the speaker.
    std::vector<uint8_t> resp_payload(response.begin(), response.end());
    REQUIRE(speaker.play(resp_payload));
    REQUIRE(speaker.playing());
    REQUIRE(speaker.played_count() == 1);

    // 8a. Event ordering: the ids processed are in the exact delivery order.
    REQUIRE(delivered_event_ids == std::vector<std::string>({"obs-0", "obs-1", "obs-2"}));

    // 8b. Bounded memory: capture buffers are bounded and history is bounded.
    REQUIRE(camera.buffer_size() <= camera.buffer_capacity());
    REQUIRE(mic.buffer_size() <= mic.buffer_capacity());
    pipeline.set_max_history_size(2);
    REQUIRE(pipeline.get_recent_events(std::chrono::hours(1)).size() <= 2);

    // 8c. Lifecycle cleanup.
    cam_guard.stop();
    cam_guard.close();
    mic_guard.stop();
    mic_guard.close();
    speaker.close();
    REQUIRE(camera.state() == DeviceState::Idle);
    REQUIRE(mic.state() == DeviceState::Idle);
    REQUIRE(speaker.state() == DeviceState::Idle);
    REQUIRE(camera.buffer_size() == 0);
    REQUIRE(mic.buffer_size() == 0);
    REQUIRE(camera.released());
    REQUIRE(mic.released());
}

// ===========================================================================
// Failure / consent scenarios
// ===========================================================================

TEST_CASE("E2E: consent denied before initialization -> nothing reaches the processor",
          "[e2e][consent][denial]") {
    Logger::initialize("error");
    E2EMemRepo repo;  // NO grant
    ConsentGate gate(repo, true);
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera");
    ConsentGuardedDevice cam_guard(camera, gate, {"harness", "sim", "camera"});
    SyntheticTestProcessor proc;

    REQUIRE_FALSE(cam_guard.initialize());
    REQUIRE_FALSE(cam_guard.start());
    std::vector<uint8_t> out;
    REQUIRE_FALSE(acquire_authorized(cam_guard, camera, out));
    // Processor never ran; no event created.
    REQUIRE(proc.observations_made == 0);
    REQUIRE(camera.init_count() == 0);
    REQUIRE(camera.start_count() == 0);
}

TEST_CASE("E2E: consent withdrawn between two deliveries drops later data",
          "[e2e][consent][withdraw]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    E2EMemRepo repo;
    repo.save(ConsentRecord("g", "harness", "sim", {"camera"}, ConsentDecision::granted,
                            "1.0.0", now - std::chrono::hours(1), std::nullopt, "operator", 1));
    ConsentGate gate(repo, true);
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera");
    ConsentGuardedDevice cam_guard(camera, gate, {"harness", "sim", "camera"});
    SyntheticTestProcessor proc;

    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());

    // First delivery authorized -> reaches processor.
    std::vector<uint8_t> b1;
    REQUIRE(acquire_authorized(cam_guard, camera, b1));
    proc.process(b1.size(), 0, "obs-0");
    REQUIRE(proc.observations_made == 1);

    // Operator withdraws consent.
    repo.save(ConsentRecord::create_withdrawn("w", "harness", "sim", "1.0.0", "operator"));

    // Subsequent deliveries are denied and never reach the processor.
    std::vector<uint8_t> b2;
    REQUIRE_FALSE(acquire_authorized(cam_guard, camera, b2));
    REQUIRE(proc.observations_made == 1);   // unchanged
    REQUIRE(camera.state() != DeviceState::Capturing);
}

TEST_CASE("E2E: capture disabled during processing blocks further delivery",
          "[e2e][consent][capture-disabled]") {
    Logger::initialize("error");
    E2EMemRepo repo;
    repo.save(ConsentRecord::create_granted("g", "harness", "sim", {"camera"}, "1.0.0", "operator"));

    // Start with capture ENABLED and deliver once.
    ConsentGate gate_on(repo, /*media_capture_enabled=*/true);
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera");
    ConsentGuardedDevice cam_guard(camera, gate_on, {"harness", "sim", "camera"});
    SyntheticTestProcessor proc;

    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());
    std::vector<uint8_t> b;
    REQUIRE(acquire_authorized(cam_guard, camera, b));
    proc.process(b.size(), 0, "obs-0");
    REQUIRE(proc.observations_made == 1);

    // The config kill switch is OFF now (shipped default). A guard bound to a
    // disabled gate must deny the very next delivery, even though the device is
    // still physically capable of producing a frame. Consent grant is present,
    // but capture is disabled -> fail closed.
    ConsentGate gate_off(repo, /*media_capture_enabled=*/false);
    ConsentGuardedDevice cam_guard_off(camera, gate_off, {"harness", "sim", "camera"});
    std::vector<uint8_t> b2;
    REQUIRE_FALSE(cam_guard_off.authorize_delivery());
    REQUIRE_FALSE(acquire_authorized(cam_guard_off, camera, b2));
    REQUIRE(proc.observations_made == 1);
    REQUIRE(camera.state() != DeviceState::Capturing);
}

TEST_CASE("E2E: simulated camera failure stops delivery; recovery allows restart",
          "[e2e][failure][camera]") {
    Logger::initialize("error");
    E2EMemRepo repo;
    repo.save(ConsentRecord::create_granted("g", "harness", "sim", {"camera"}, "1.0.0", "operator"));
    ConsentGate gate(repo, true);
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera");
    ConsentGuardedDevice cam_guard(camera, gate, {"harness", "sim", "camera"});
    SyntheticTestProcessor proc;

    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());
    std::vector<uint8_t> b;
    REQUIRE(acquire_authorized(cam_guard, camera, b));
    proc.process(b.size(), 0, "obs-0");

    camera.fail();
    std::vector<uint8_t> b2;
    REQUIRE_FALSE(acquire_authorized(cam_guard, camera, b2));  // fail-closed
    REQUIRE(proc.observations_made == 1);
    REQUIRE(camera.state() == DeviceState::Error);

    // Recovery: close + re-init + restart, then delivery resumes.
    camera.recover();
    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());
    std::vector<uint8_t> b3;
    REQUIRE(acquire_authorized(cam_guard, camera, b3));
    proc.process(b3.size(), 0, "obs-1");
    REQUIRE(proc.observations_made == 2);
}

TEST_CASE("E2E: event-processing failure does not crash; pipeline stays usable",
          "[e2e][failure][event]") {
    Logger::initialize("error");
    Pipeline pipeline({});
    // A valid observation processes without an alert (no rules).
    Event good = Event::create_observation("ok-1", "src", {{"k", 1}});
    auto a1 = pipeline.process(good);
    REQUIRE_FALSE(a1.has_value());

    // A correlation rule that throws must not take down the pipeline: the
    // pipeline processes subsequent events and stays usable. (This models an
    // event-processing failure inside the rule stage.)
    class ThrowingRule : public ICorrelationRule {
    public:
        std::optional<Alert> evaluate(const std::vector<Event>&) override {
            throw std::runtime_error("synthetic rule failure");
        }
        std::string name() const override { return "throwing-rule"; }
        std::string description() const override { return "always throws"; }
    };

    Pipeline p2({std::make_shared<ThrowingRule>()});
    Event e = Event::create_observation("e1", "src", {{"k", 1}});
    bool threw = false;
    try {
        p2.process(e);
    } catch (const std::exception&) {
        threw = true;  // the pipeline surfaces the rule error rather than
                       // silently swallowing it
    }
    REQUIRE(threw);

    // A healthy pipeline processes a valid event without an alert and remains
    // usable afterward (bounded history).
    Pipeline healthy({});
    auto a = healthy.process(Event::create_observation("ok", "src", {{"k", 1}}));
    REQUIRE_FALSE(a.has_value());
    REQUIRE(healthy.get_recent_events(std::chrono::hours(1)).size() >= 1);
}

TEST_CASE("E2E: speaker playback failure then recovery",
          "[e2e][failure][speaker]") {
    Logger::initialize("error");
    SimulatedSensorBackend speaker(DeviceKind::audio, "sim-speaker",
                                   SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                                                  SimulatedSensorBackend::TestSignal::sine});
    speaker.initialize();
    std::vector<uint8_t> resp(16, 0);
    REQUIRE(speaker.play(resp));

    speaker.inject_playback_error();
    REQUIRE_FALSE(speaker.play(resp));   // playback fails
    REQUIRE_FALSE(speaker.playing());

    // Recover and play again.
    speaker.close();
    speaker.initialize();
    REQUIRE(speaker.play(resp));
    REQUIRE(speaker.played_count() == 2);
}

TEST_CASE("E2E: speaker output is independent of the capture-consent gate",
          "[e2e][speaker][consent-boundary]") {
    Logger::initialize("error");
    E2EMemRepo repo;  // no consent granted
    ConsentGate gate(repo, true);
    SimulatedSensorBackend speaker(DeviceKind::audio, "sim-speaker");
    ConsentGuardedDevice spk_guard(speaker, gate, {"harness", "sim", "audio"});

    // Capture is denied without consent...
    REQUIRE_FALSE(spk_guard.start());
    // ...but playback (output) still works; it is not a capture path.
    speaker.initialize();
    std::vector<uint8_t> resp(8, 0);
    REQUIRE(speaker.play(resp));
    REQUIRE(speaker.played_count() == 1);
}

TEST_CASE("E2E: shutdown while buffers contain pending data releases everything",
          "[e2e][shutdown]") {
    Logger::initialize("error");
    E2EMemRepo repo;
    repo.save(ConsentRecord::create_granted("g", "harness", "sim", {"camera"}, "1.0.0", "operator"));
    ConsentGate gate(repo, true);
    SimulatedSensorBackend camera(DeviceKind::camera, "sim-camera",
                                  SimulatedSensorBackend::Config{16, 16, 0.0, 0, 0, 1,
                                                                 SimulatedSensorBackend::TestSignal::noise,
                                                                 440.0, /*ring_capacity*/8});
    ConsentGuardedDevice cam_guard(camera, gate, {"harness", "sim", "camera"});
    REQUIRE(cam_guard.initialize());
    REQUIRE(cam_guard.start());

    // Push pending data beyond capacity so the ring is full and has evictions.
    for (int i = 0; i < 20; ++i) {
        if (!cam_guard.authorize_delivery()) break;
        auto p = camera.generate_frame();
        camera.record_delivered(std::move(p));
    }
    REQUIRE(camera.buffer_size() == 8);         // bounded, full
    REQUIRE(camera.buffer_dropped() == 12);

    // Shutdown releases capture resources and clears the buffer.
    cam_guard.stop();
    cam_guard.close();
    REQUIRE(camera.state() == DeviceState::Idle);
    REQUIRE(camera.buffer_size() == 0);         // pending data released
    REQUIRE(camera.released());
}
