#include "net/IngestDetectors.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace homeguardian::net {

namespace {

std::chrono::system_clock::time_point from_ms(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// SecurityObservation carries no id of its own (the wire id lives in the Event
// path). A derived observation needs a deterministic, unique id, so it is built
// from the device, the acquisition time and a per-process counter.
std::string derived_id(const SecurityObservation& observation, const char* kind) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1);
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           observation.acquired_at.time_since_epoch())
                           .count();
    return std::string("derived-") + kind + "-" + observation.device_id + "-" +
           std::to_string(ms) + "-" + std::to_string(n);
}

int64_t to_ms(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

// Read a numeric feature, refusing anything that is not a finite number.
bool numeric_feature(const nlohmann::json& features, const char* key, double* out) {
    if (!features.contains(key)) return false;
    const nlohmann::json& value = features[key];
    if (!value.is_number()) return false;
    const double d = value.get<double>();
    if (!std::isfinite(d)) return false;
    if (out != nullptr) *out = d;
    return true;
}

}  // namespace

IngestDetectors::Clock IngestDetectors::system_clock_ms() {
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
}

IngestDetectors::Config IngestDetectors::defaults() {
    Config c;
    c.motion_activity.feature = "motion-changed-fraction";
    c.motion_activity.detector_id = "motion-activity";
    c.motion_activity.model_id = "ema-baseline-v1";
    c.audio_activity.feature = "audio-rms";
    c.audio_activity.detector_id = "audio-activity";
    c.audio_activity.model_id = "ema-baseline-v1";
    c.audio_anomaly.feature = "audio-rms";
    c.audio_anomaly.key = "audio-rms";
    c.audio_anomaly.detector_id = "audio-activity-deviation";
    c.audio_anomaly.min_samples = 8;
    c.audio_anomaly.max_window = 32;
    return c;
}

IngestDetectors::IngestDetectors(Config cfg, Clock clock)
    : cfg_(std::move(cfg)), clock_(std::move(clock)) {
    SensorDeduplicator::Config d;
    d.cooldown = cfg_.dedup_cooldown;
    d.max_entries = cfg_.max_devices * 4;
    dedup_ = SensorDeduplicator(d);

    DeviceAvailabilityTracker::Config a;
    a.offline_after = cfg_.offline_after;
    a.max_devices = cfg_.max_devices;
    availability_ = DeviceAvailabilityTracker(a);
}

IngestDetectors::~IngestDetectors() = default;

void IngestDetectors::reset_counters() {
    derived_ = 0;
    duplicates_ = 0;
    overflow_ = 0;
}

IngestDetectors::DeviceState* IngestDetectors::state_for(const std::string& device_id) {
    const auto it = devices_.find(device_id);
    if (it != devices_.end()) return it->second.get();
    if (devices_.size() >= cfg_.max_devices) return nullptr;  // bounded: refuse new state
    auto state = std::make_unique<DeviceState>(cfg_.motion_activity, cfg_.audio_activity,
                                               cfg_.audio_anomaly);
    DeviceState* raw = state.get();
    devices_.emplace(device_id, std::move(state));
    return raw;
}

void IngestDetectors::enqueue(Observation observation) {
    if (pending_.size() >= cfg_.max_pending) {
        ++overflow_;  // bounded queue: drop the OLDEST derived observation, count it
        pending_.erase(pending_.begin());
    }
    pending_.push_back(std::move(observation));
    ++derived_;
}

IngestDetectors::Outcome IngestDetectors::observe(const SecurityObservation& observation) {
    Outcome outcome;
    const int64_t now_ms = clock_();
    const auto now = from_ms(observation.ingested_at.time_since_epoch().count() > 0
                                 ? to_ms(observation.ingested_at)
                                 : now_ms);

    // Liveness first: an observation from a device IS contact with that device.
    const auto transition = availability_.mark_seen(observation.device_id, now);
    if (transition.has_value() && !transition->first_seen) {
        SecurityObservation back = observation;
        back.source_type = SecuritySourceType::device_availability;
        back.state = "online";
        back.note = transition->reason;
        back.features = nlohmann::json::object();
        back.features["entity_id"] = observation.device_id;
        back.features["transition"] = "offline->online";
        enqueue(back.to_observation());
    }

    // NOTE ON ORDER: de-duplication must NEVER suppress detector INPUT. A repeated
    // motion "on" is not a new announcement, but the activity detector still needs
    // every sample to learn a baseline - suppressing the feed would freeze the
    // baseline at one sample and make every later change unconfirmable (this was a
    // real defect, caught by the sustained-rise test). So the detector is fed
    // first, and the cooldown only marks the observation as a repeat.
    DeviceState* state = state_for(observation.device_id);
    if (state == nullptr) {
        outcome.reason = "per-device detector state is at capacity";
        return outcome;
    }
    state->provenance = observation.provenance;
    state->consent_subject = observation.consent_subject;
    state->consent_purpose = observation.consent_purpose;

    // Repeat accounting, applied AFTER the feed decision and never as a gate.
    SensorDeduplicator::Key key;
    key.device_id = observation.device_id;
    key.source_type = observation.source_type;
    key.state = observation.state;
    if (dedup_.duplicate(key, now)) {
        ++duplicates_;
        outcome.duplicate = true;
    }

    // Deterministic, single-scalar detections, keyed off the observation type.
    double value = 0.0;
    if (numeric_feature(observation.features, "motion-changed-fraction", &value)) {
        const auto result = state->motion.observe(value, now);
        auto derived = state->motion.to_observation(result, derived_id(observation, "motion"),
                                                    observation.device_id, observation.provenance);
        derived.observation_timestamp = now;
        enqueue(std::move(derived));
        outcome.fed = true;
        outcome.detector = "motion-activity";
        outcome.reason = result.reason;
        return outcome;
    }
    if (numeric_feature(observation.features, "audio-rms", &value)) {
        const auto activity = state->audio.observe(value, now);
        auto derived = state->audio.to_observation(activity, derived_id(observation, "audio"),
                                                   observation.device_id, observation.provenance);
        derived.observation_timestamp = now;
        enqueue(std::move(derived));
        const auto anomaly = state->anomaly.observe(value, now);
        enqueue(state->anomaly.to_observation(anomaly, derived_id(observation, "anomaly"), now,
                                              cfg_.audio_anomaly.min_samples));
        outcome.fed = true;
        outcome.detector = "audio-activity";
        outcome.reason = activity.reason;
        return outcome;
    }

    // Environmental readings are measurements: validated, bounded, and carried as
    // observations. No threshold is invented and no correlation is claimed here.
    if (observation.source_type == SecuritySourceType::environmental_threshold) {
        enqueue(observation.to_observation());
        outcome.fed = true;
        outcome.detector = "environmental";
        outcome.reason = "authorized environmental reading carried as an observation";
        return outcome;
    }

    outcome.reason = "no detector is wired for this observation type";
    return outcome;
}

size_t IngestDetectors::drain(std::vector<Observation>& out) {
    size_t moved = 0;
    while (!pending_.empty() && out.size() < 4096) {
        out.push_back(std::move(pending_.front()));
        pending_.erase(pending_.begin());
        ++moved;
    }
    return moved;
}

std::vector<SecurityObservation> IngestDetectors::sweep_availability() {
    const int64_t now_ms = clock_();
    std::vector<SecurityObservation> out;
    for (const DeviceAvailabilityTracker::Transition& t : availability_.sweep(from_ms(now_ms))) {
        const auto state = devices_.find(t.device_id);
        if (state == devices_.end()) continue;
        SecurityObservation o;
        o.device_id = t.device_id;
        o.source = "node";
        o.source_type = SecuritySourceType::device_availability;
        o.acquired_at = t.at;
        o.ingested_at = t.at;
        o.provenance = state->second->provenance;
        o.state = "offline";
        o.consent_subject = state->second->consent_subject;
        o.consent_purpose = state->second->consent_purpose;
        o.authorized = true;
        o.features = nlohmann::json::object();
        o.features["entity_id"] = t.device_id;
        o.features["transition"] = "online->offline";
        o.note = t.reason;
        out.push_back(std::move(o));
    }
    return out;
}

}  // namespace homeguardian::net
