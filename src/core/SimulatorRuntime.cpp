#include "SimulatorRuntime.h"

#include "backend/simulator/LogicalDevice.h"

#include <algorithm>
#include <string>

namespace homeguardian {

namespace {
// Deterministic synthetic audio configs (fixed seeds -> reproducible payloads).
SimulatedSensorBackend::Config make_mic_cfg() {
    return SimulatedSensorBackend::Config{
        /*width*/0, /*height*/0, /*frame_rate*/0.0,
        /*sample_rate*/16000, /*samples_per_buffer*/320, /*channels*/1,
        SimulatedSensorBackend::TestSignal::sine, /*tone_hz*/440.0,
        /*ring_capacity*/256, /*seed*/0x51A1};
}
SimulatedSensorBackend::Config make_spk_cfg() {
    return SimulatedSensorBackend::Config{
        /*width*/0, /*height*/0, /*frame_rate*/0.0,
        /*sample_rate*/16000, /*samples_per_buffer*/320, /*channels*/1,
        SimulatedSensorBackend::TestSignal::sine, /*tone_hz*/440.0,
        /*ring_capacity*/64, /*seed*/0x5A2B};
}
}  // namespace

SimulatorRuntime::SimulatorRuntime(bool media_capture_enabled,
                                   size_t speaker_queue_capacity)
    : media_capture_enabled_(media_capture_enabled),
      gate_(std::make_unique<ConsentGate>(repo_, media_capture_enabled)),
      broker_(std::make_unique<AudioBroker>(make_mic_cfg(), make_spk_cfg(),
                                            speaker_queue_capacity)) {}

std::string SimulatorRuntime::enabled_device_id() {
    return kLogicalDeviceAlexaEnabled;
}
std::string SimulatorRuntime::assistant_device_id() {
    return kLogicalDeviceAlexaAssistant;
}

SimulatorRuntime::ConsumerRequest SimulatorRuntime::request_for(
    const std::string& consumer_id) const {
    // Distinct purpose per device so each has an independent grant. Audio is the
    // only data category consumed here (camera stays a separate peripheral).
    if (consumer_id == kLogicalDeviceAlexaEnabled) {
        return {"sim_alexa_enabled", "audio"};
    }
    if (consumer_id == kLogicalDeviceAlexaAssistant) {
        return {"sim_alexa_assistant", "audio"};
    }
    return {"sim_unknown", "audio"};
}

void SimulatorRuntime::register_default_devices() {
    if (shut_down_) return;

    // Two independent logical devices.
    LogicalDeviceConfig enabled_cfg;
    enabled_cfg.display_name = "Simulated Alexa-Enabled Device";
    enabled_cfg.capabilities.consumes_microphone = true;
    enabled_cfg.capabilities.drives_speaker = true;
    enabled_cfg.capabilities.has_camera = true;
    enabled_cfg.max_event_history = 128;
    enabled_device_ = std::make_unique<LogicalDevice>(kLogicalDeviceAlexaEnabled, enabled_cfg);

    LogicalDeviceConfig assistant_cfg;
    assistant_cfg.display_name = "Simulated Alexa Assistant";
    assistant_cfg.capabilities.consumes_microphone = true;
    assistant_cfg.capabilities.drives_speaker = true;
    assistant_cfg.capabilities.has_camera = true;
    assistant_cfg.max_event_history = 128;
    assistant_device_ = std::make_unique<LogicalDevice>(kLogicalDeviceAlexaAssistant, assistant_cfg);

    // Register both as consumers on the SINGLE shared mic/speaker broker. Each
    // gets its own consent gate + request (per-consumer authorization).
    if (broker_->consumer_count() == 0) {
        auto er = request_for(kLogicalDeviceAlexaEnabled);
        broker_->register_consumer(kLogicalDeviceAlexaEnabled, *gate_,
                                   {"harness", er.purpose, er.data_category});
        auto ar = request_for(kLogicalDeviceAlexaAssistant);
        broker_->register_consumer(kLogicalDeviceAlexaAssistant, *gate_,
                                   {"harness", ar.purpose, ar.data_category});
    }
}

void SimulatorRuntime::grant_consent(const std::string& consumer_id,
                                     const std::string& purpose) {
    (void)consumer_id;  // consent is keyed by purpose; id is for API clarity
    if (shut_down_) return;
    // Idempotent grant: mirror the gate's "the latest decision governs" rule.
    // Saving a second grant for an already-granted subject+purpose would create
    // two active grants, which the gate correctly rejects as `ambiguous` (fail
    // closed) — that must never be caused by simply re-granting the same
    // consent. A withdrawal recorded after an earlier grant is the latest
    // decision, so an explicit re-grant then still appends a fresh grant.
    auto existing = repo_.find_by_purpose("harness", purpose);
    if (!existing.empty()) {
        std::sort(existing.begin(), existing.end(),
                  [](const ConsentRecord& a, const ConsentRecord& b) {
                      return a.get_recorded_at() < b.get_recorded_at();
                  });
        const auto& latest = existing.back();
        if (latest.get_decision() == ConsentDecision::granted &&
            latest.is_active(std::chrono::system_clock::now()))
            return;  // already granted and active — no-op
    }
    // Synthetic grant. Provenance marks it as a simulator test grant, never a
    // real user consent record.
    repo_.save(ConsentRecord::create_granted(
        "grant-" + purpose, "harness", purpose, {"audio"},
        "sim-1.0.0", "simulator_runtime"));
}

void SimulatorRuntime::withdraw_consent(const std::string& consumer_id,
                                        const std::string& purpose) {
    (void)consumer_id;  // consent is keyed by purpose; id is for API clarity
    if (shut_down_) return;
    repo_.save(ConsentRecord::create_withdrawn(
        "withdraw-" + purpose, "harness", purpose, "sim-1.0.0", "simulator_runtime"));
}

void SimulatorRuntime::set_event_sink(std::function<void(const Event&)> sink) {
    event_sink_ = std::move(sink);
}

void SimulatorRuntime::set_media_capture_enabled(bool enabled) {
    // Rebuild the gate so the global kill switch takes effect immediately.
    media_capture_enabled_ = enabled;
    gate_ = std::make_unique<ConsentGate>(repo_, media_capture_enabled_);
    // Re-point consumers at the new gate via re-registration of their requests.
    // (The broker stores a pointer to the gate; rebuilding invalidates it, so we
    // recreate the broker's consumer bindings by rebuilding the broker.)
    // Simpler and safe: rebuild the broker preserving device identity.
    SimulatedSensorBackend::Config mic = make_mic_cfg();
    SimulatedSensorBackend::Config spk = make_spk_cfg();
    broker_ = std::make_unique<AudioBroker>(mic, spk, /*cap*/64);
    if (enabled_device_ && assistant_device_) {
        auto er = request_for(kLogicalDeviceAlexaEnabled);
        broker_->register_consumer(kLogicalDeviceAlexaEnabled, *gate_,
                                   {"harness", er.purpose, er.data_category});
        auto ar = request_for(kLogicalDeviceAlexaAssistant);
        broker_->register_consumer(kLogicalDeviceAlexaAssistant, *gate_,
                                   {"harness", ar.purpose, ar.data_category});
    }
    started_ = false;
}

void SimulatorRuntime::start() {
    if (shut_down_ || started_) return;
    if (!enabled_device_ || !assistant_device_) register_default_devices();
    // The broker starts the SINGLE shared capture session. Consumers that lack
    // consent simply do not receive media; they do not block the shared source.
    broker_->start_capture();
    started_ = true;
}

int SimulatorRuntime::tick() {
    if (shut_down_ || !started_) return 0;

    // One shared synthetic mic payload, fanned out to authorized consumers.
    auto payload = broker_->capture_and_fanout();
    if (payload.empty()) {
        // No consumer authorized this cycle -> no media generated, no delivery.
        return 0;
    }

    int delivered = 0;
    // For each consumer that received media this cycle, build an authorized
    // synthetic observation, record it on the logical device (bounded), route it
    // through the event sink (Application::process_event -> Pipeline), and
    // enqueue a synthetic speaker response. Denied/withdrawn consumers produce
    // no event and never reach the pipeline.
    const std::string ids[2] = {kLogicalDeviceAlexaEnabled, kLogicalDeviceAlexaAssistant};
    for (const auto& id : ids) {
        // Ask the gate directly for a deterministic per-consumer decision
        // consistent with the fan-out above.
        auto req = request_for(id);
        auto decision = gate_->check({"harness", req.purpose, req.data_category});
        // Privacy audit (H-03): record this delivery decision (authorized or
        // denied). Passive, metadata-only, best-effort — never changes delivery.
        if (privacy_audit_) {
            PrivacyAuditEntry pe;
            pe.timestamp = std::chrono::system_clock::now();
            pe.subject_scope = "harness";
            pe.purpose = req.purpose;
            pe.data_category = req.data_category;
            pe.authorized = decision.authorized;
            pe.decision_reason = ConsentGate::reason_to_string(decision.reason);
            pe.acquired = decision.authorized;  // media delivered iff authorized
            privacy_audit_->record(pe);
        }
        if (decision.authorized) {
            delivered++;
            // Build the synthetic observation (a valid Event: non-empty id +
            // source + object payload).
            Event ev = Event::create_observation(
                "sim-obs-" + std::to_string(seq_) + "-" + id, id,
                {{"scenario", "SYNTHETIC_SIMULATOR_ONLY"},
                 {"consumer", id},
                 {"audio_bytes", static_cast<uint64_t>(payload.size())},
                 {"note", "synthetic; not a real-world detection"}});

            // Route the AUTHORIZED observation through the real application
            // event path (validation + Pipeline correlation + alert). Only
            // authorized observations reach here; the sink is the integration
            // boundary and performs no capture of its own.
            if (event_sink_) {
                event_sink_(ev);
            }

            // Record the same observation on the logical device (bounded).
            if (id == kLogicalDeviceAlexaEnabled && enabled_device_) {
                enabled_device_->record_event(ev);
            } else if (id == kLogicalDeviceAlexaAssistant && assistant_device_) {
                assistant_device_->record_event(ev);
            }
            // Enqueue a synthetic response for the arbitrated speaker. The queue
            // is bounded; on overflow the new response is rejected (back-pressure).
            std::vector<uint8_t> resp(32, 0);
            broker_->enqueue_speaker(id, std::move(resp));
        }
    }

    // Drain the speaker queue through arbitration (no overlap). finish_playback
    // models the (instant, simulated) completion of each play.
    while (broker_->play_next()) {
        broker_->finish_playback();
    }

    seq_++;
    return delivered;
}

void SimulatorRuntime::shutdown() {
    if (shut_down_) return;
    if (broker_) broker_->shutdown();  // stops capture, clears queue, releases speaker
    if (enabled_device_) enabled_device_->close();
    if (assistant_device_) assistant_device_->close();
    started_ = false;
    shut_down_ = true;
}

void SimulatorRuntime::reset_for_reuse() {
    // Only meaningful when not currently running: a still-started runtime is
    // left untouched so an in-flight session is never disturbed.
    if (started_) return;
    shut_down_ = false;
}

size_t SimulatorRuntime::consumer_count() const {
    return broker_ ? broker_->consumer_count() : 0;
}
size_t SimulatorRuntime::delivered_count(const std::string& consumer_id) const {
    return broker_ ? broker_->delivered_count(consumer_id) : 0;
}
size_t SimulatorRuntime::last_fanout_blocked() const {
    return broker_ ? broker_->last_fanout_blocked() : 0;
}
bool SimulatorRuntime::speaker_busy() const {
    return broker_ ? broker_->speaker_busy() : false;
}
size_t SimulatorRuntime::speaker_queue_size() const {
    return broker_ ? broker_->speaker_queue_size() : 0;
}
size_t SimulatorRuntime::speaker_played_count() const {
    return broker_ ? broker_->speaker_played_count() : 0;
}
size_t SimulatorRuntime::speaker_queue_dropped() const {
    return broker_ ? broker_->speaker_queue_dropped() : 0;
}
bool SimulatorRuntime::capture_started() const { return started_; }
bool SimulatorRuntime::shut_down() const { return shut_down_; }

}  // namespace homeguardian
