#include "catch2/catch.hpp"
#include "backend/simulator/LogicalDevice.h"
#include "backend/simulator/AudioBroker.h"
#include "backend/simulator/SimulatedSensorBackend.h"
#include "core/ConsentGate.h"
#include "core/ConsentGuardedDevice.h"
#include "core/ConsentRecord.h"
#include "core/Logger.h"
#include "persistence/IConsentRepository.h"
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Dual logical-device simulation over a shared Ubuntu audio broker.
//
// Host, deterministic, non-capture. Two INDEPENDENT logical devices
// (simulated_alexa_enabled_device, simulated_alexa_assistant) share ONE
// simulated microphone source and ONE simulated speaker via the AudioBroker.
// Each consumer has its own consent gate; fan-out is authorized per consumer at
// the delivery boundary; speaker output is bounded and arbitrated (no overlap).
//
// These logical devices are HomeGuardian simulation entities. They do NOT run
// Amazon's original Alexa OS and make no Alexa-product claim.
// ---------------------------------------------------------------------------

// Minimal in-memory consent repository (same pattern as the other sim tests).
struct MemRepo : IConsentRepository {
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

// Audio config helpers.
static SimulatedSensorBackend::Config mic_cfg() {
    return SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                          SimulatedSensorBackend::TestSignal::sine};
}
static SimulatedSensorBackend::Config spk_cfg() {
    return SimulatedSensorBackend::Config{0, 0, 0.0, 16000, 320, 1,
                                          SimulatedSensorBackend::TestSignal::sine};
}

// Build a logical device with typical Alexa-sim capabilities.
static LogicalDevice make_logical_device(const std::string& id, const std::string& display) {
    LogicalDeviceConfig cfg;
    cfg.display_name = display;
    cfg.capabilities.consumes_microphone = true;
    cfg.capabilities.drives_speaker = true;
    cfg.capabilities.has_camera = true;   // separate simulated camera peripheral
    cfg.max_event_history = 8;
    return LogicalDevice(id, cfg);
}

// ===========================================================================
// Logical device independence
// ===========================================================================

TEST_CASE("Dual-device: two registered logical devices have independent identity and state",
          "[dual][device]") {
    Logger::initialize("error");
    LogicalDevice a = make_logical_device(kLogicalDeviceAlexaEnabled, "Alexa-Enabled Device");
    LogicalDevice b = make_logical_device(kLogicalDeviceAlexaAssistant, "Alexa Assistant");

    REQUIRE(a.id() == "simulated_alexa_enabled_device");
    REQUIRE(b.id() == "simulated_alexa_assistant");
    REQUIRE(a.id() != b.id());

    // Advance A only; B must remain Idle (independence).
    a.initialize();
    a.activate();
    REQUIRE(a.state() == LogicalDeviceState::Active);
    REQUIRE(b.state() == LogicalDeviceState::Idle);

    // A's event history does not appear in B's.
    a.record_event(Event::create_observation("a-0", a.id(), {{"k", 1}}));
    REQUIRE(a.event_history_size() == 1);
    REQUIRE(b.event_history_size() == 0);
}

TEST_CASE("Dual-device: per-device bounded event history drops oldest, never unbounded",
          "[dual][device][bounded]") {
    Logger::initialize("error");
    LogicalDevice a = make_logical_device(kLogicalDeviceAlexaEnabled, "A");
    REQUIRE(a.config().max_event_history == 8);
    for (int i = 0; i < 20; ++i) {
        a.record_event(Event::create_observation("e-" + std::to_string(i), a.id(), {{"i", i}}));
    }
    REQUIRE(a.event_history_size() == 8);   // bounded
    auto hist = a.recent_events();
    REQUIRE(hist.front().get_event_id() == "e-12");  // oldest kept (12..19)
    REQUIRE(hist.back().get_event_id() == "e-19");
}

TEST_CASE("Dual-device: one device failing does not affect the other",
          "[dual][device][failure]") {
    Logger::initialize("error");
    LogicalDevice a = make_logical_device(kLogicalDeviceAlexaEnabled, "A");
    LogicalDevice b = make_logical_device(kLogicalDeviceAlexaAssistant, "B");
    a.initialize(); a.activate();
    b.initialize(); b.activate();

    a.fail();
    REQUIRE(a.state() == LogicalDeviceState::Error);
    REQUIRE(b.state() == LogicalDeviceState::Active);   // unaffected

    a.recover();
    REQUIRE(a.state() == LogicalDeviceState::Idle);
    REQUIRE(b.state() == LogicalDeviceState::Active);
}

// ===========================================================================
// Shared source fan-out + per-consumer consent
// ===========================================================================

TEST_CASE("Dual-device: single shared mic source fans out to both authorized consumers",
          "[dual][broker][fanout]") {
    Logger::initialize("error");
    MemRepo repo;
    repo.save(ConsentRecord::create_granted("g-enabled", "harness", "alexa_enabled", {"audio"}, "1.0.0", "operator"));
    repo.save(ConsentRecord::create_granted("g-assistant", "harness", "alexa_assistant", {"audio"}, "1.0.0", "operator"));
    ConsentGate gate(repo, /*media_capture_enabled=*/true);

    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});
    REQUIRE(broker.consumer_count() == 2);

    broker.start_capture();
    for (int i = 0; i < 5; ++i) {
        auto payload = broker.capture_and_fanout();
        REQUIRE_FALSE(payload.empty());            // shared source produced bytes
        REQUIRE(broker.last_fanout_blocked() == 0);
    }
    // Both consumers received all 5 authorized fan-outs.
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaEnabled) == 5);
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaAssistant) == 5);
    // ONE capture session: the shared mic advanced 5 times, not 10.
    REQUIRE(broker.mic().sequence() == 5);
}

TEST_CASE("Dual-device: per-consumer consent denial blocks only that consumer",
          "[dual][broker][consent][denial]") {
    Logger::initialize("error");
    MemRepo repo;
    // Only the enabled device has a grant; the assistant has none.
    repo.save(ConsentRecord::create_granted("g-enabled", "harness", "alexa_enabled", {"audio"}, "1.0.0", "operator"));
    ConsentGate gate(repo, true);

    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});

    broker.start_capture();
    auto payload = broker.capture_and_fanout();
    REQUIRE_FALSE(payload.empty());               // at least one consumer authorized
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaEnabled) == 1);
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaAssistant) == 0);  // blocked
    REQUIRE(broker.last_fanout_blocked() == 1);
}

TEST_CASE("Dual-device: consent withdrawal blocks only the withdrawn consumer",
          "[dual][broker][consent][withdraw]") {
    Logger::initialize("error");
    auto now = std::chrono::system_clock::now();
    MemRepo repo;
    repo.save(ConsentRecord("g-enabled", "harness", "alexa_enabled", {"audio"}, ConsentDecision::granted,
                            "1.0.0", now - std::chrono::hours(1), std::nullopt, "operator", 1));
    repo.save(ConsentRecord("g-assistant", "harness", "alexa_assistant", {"audio"}, ConsentDecision::granted,
                            "1.0.0", now - std::chrono::hours(1), std::nullopt, "operator", 1));
    ConsentGate gate(repo, true);

    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});
    broker.start_capture();

    // Both receive the first fan-out.
    broker.capture_and_fanout();
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaEnabled) == 1);
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaAssistant) == 1);

    // Withdraw the assistant's consent only.
    repo.save(ConsentRecord::create_withdrawn("w-assistant", "harness", "alexa_assistant", "1.0.0", "operator"));

    // Next fan-out: enabled still receives, assistant is blocked immediately.
    broker.capture_and_fanout();
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaEnabled) == 2);   // unaffected
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaAssistant) == 1); // no further delivery
    REQUIRE(broker.last_fanout_blocked() == 1);
}

TEST_CASE("Dual-device: no consumer authorized -> shared source produces nothing",
          "[dual][broker][consent][none]") {
    Logger::initialize("error");
    MemRepo repo;  // no grants at all
    ConsentGate gate(repo, true);
    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});
    broker.start_capture();

    auto payload = broker.capture_and_fanout();
    REQUIRE(payload.empty());                     // nothing generated
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaEnabled) == 0);
    REQUIRE(broker.delivered_count(kLogicalDeviceAlexaAssistant) == 0);
    REQUIRE(broker.mic().sequence() == 0);        // shared source never advanced
}

// ===========================================================================
// Speaker output: bounded queue + arbitration
// ===========================================================================

TEST_CASE("Dual-device: speaker arbitration prevents overlapping responses",
          "[dual][broker][speaker][arbitration]") {
    Logger::initialize("error");
    AudioBroker broker(mic_cfg(), spk_cfg());
    std::vector<uint8_t> job(16, 0);

    // Enqueue two responses from the two devices.
    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job));
    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaAssistant, job));
    REQUIRE(broker.speaker_queue_size() == 2);

    // Play the first; the speaker is now busy.
    REQUIRE(broker.play_next());
    REQUIRE(broker.speaker_busy());
    REQUIRE(broker.speaker_played_count() == 1);

    // A second play while busy must be refused (no overlap).
    REQUIRE_FALSE(broker.play_next());

    // Finish playback, then the next job can play.
    broker.finish_playback();
    REQUIRE_FALSE(broker.speaker_busy());
    REQUIRE(broker.play_next());
    REQUIRE(broker.speaker_played_count() == 2);
}

TEST_CASE("Dual-device: speaker output queue is bounded; overflow drops oldest",
          "[dual][broker][speaker][bounded]") {
    Logger::initialize("error");
    AudioBroker broker(mic_cfg(), spk_cfg(), /*output_queue_capacity=*/2);
    std::vector<uint8_t> job(8, 0);

    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job));
    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaAssistant, job));
    REQUIRE(broker.speaker_queue_size() == 2);

    // Third enqueue exceeds capacity -> oldest dropped, new one rejected.
    REQUIRE_FALSE(broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job));
    REQUIRE(broker.speaker_queue_size() == 2);   // still bounded
    REQUIRE(broker.speaker_queue_dropped() == 1);
}

TEST_CASE("Dual-device: speaker playback failure then recovery",
          "[dual][broker][speaker][failure]") {
    Logger::initialize("error");
    AudioBroker broker(mic_cfg(), spk_cfg());
    std::vector<uint8_t> job(8, 0);
    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job));

    broker.inject_speaker_error();
    REQUIRE_FALSE(broker.play_next());           // playback fails
    REQUIRE_FALSE(broker.speaker_busy());

    broker.recover_speaker();
    REQUIRE(broker.play_next());                 // recovered
    REQUIRE(broker.speaker_played_count() == 1);
}

TEST_CASE("Dual-device: speaker output is independent of the capture-consent gate",
          "[dual][broker][speaker][consent-boundary]") {
    Logger::initialize("error");
    MemRepo repo;  // no capture consent
    ConsentGate gate(repo, true);
    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});
    broker.start_capture();

    // Capture fan-out is denied for everyone (no consent)...
    auto payload = broker.capture_and_fanout();
    REQUIRE(payload.empty());
    // ...but speaker OUTPUT still works; playback is not a capture path.
    std::vector<uint8_t> job(8, 0);
    REQUIRE(broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job));
    REQUIRE(broker.play_next());
    REQUIRE(broker.speaker_played_count() == 1);
}

// ===========================================================================
// Shutdown
// ===========================================================================

TEST_CASE("Dual-device: deterministic shutdown stops capture and clears everything",
          "[dual][broker][shutdown]") {
    Logger::initialize("error");
    MemRepo repo;
    repo.save(ConsentRecord::create_granted("g-enabled", "harness", "alexa_enabled", {"audio"}, "1.0.0", "operator"));
    repo.save(ConsentRecord::create_granted("g-assistant", "harness", "alexa_assistant", {"audio"}, "1.0.0", "operator"));
    ConsentGate gate(repo, true);
    AudioBroker broker(mic_cfg(), spk_cfg());
    broker.register_consumer(kLogicalDeviceAlexaEnabled, gate, {"harness", "alexa_enabled", "audio"});
    broker.register_consumer(kLogicalDeviceAlexaAssistant, gate, {"harness", "alexa_assistant", "audio"});
    broker.start_capture();
    broker.capture_and_fanout();

    // Leave a pending speaker job in the queue.
    std::vector<uint8_t> job(8, 0);
    broker.enqueue_speaker(kLogicalDeviceAlexaEnabled, job);
    REQUIRE(broker.speaker_queue_size() == 1);

    broker.shutdown();

    // Capture stopped: further fan-out is a no-op (returns empty, no throw).
    REQUIRE(broker.capture_and_fanout().empty());
    // Queue cleared and speaker released.
    REQUIRE(broker.speaker_queue_size() == 0);
    REQUIRE_FALSE(broker.speaker_busy());
    REQUIRE(broker.mic().state() == DeviceState::Idle);
    REQUIRE(broker.speaker().state() == DeviceState::Idle);

    // Shutdown is idempotent.
    broker.shutdown();
    REQUIRE(broker.speaker_queue_size() == 0);
}
