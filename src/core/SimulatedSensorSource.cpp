#include "SimulatedSensorSource.h"

namespace homeguardian {

SimulatedSensorSource::SimulatedSensorSource() = default;

void SimulatedSensorSource::enqueue(const SimulatedReading& reading) {
    queue_.push_back(reading);
}

void SimulatedSensorSource::enqueue_malformed(const std::string& event_id, const std::string& source) {
    // A malformed reading is represented as a custom reading whose payload is a
    // JSON array (not an object). The Event constructor rejects non-object
    // payloads, so build_event will throw; callers that feed events through a
    // validator will see it rejected. We still record the intent deterministically.
    SimulatedReading r;
    r.event_id = event_id;
    r.source = source;
    r.kind = SimulatedSensorKind::custom;
    r.payload = nlohmann::json::array();  // invalid: must be object or null
    queue_.push_back(r);
}

void SimulatedSensorSource::disconnect() {
    connected_ = false;
}

void SimulatedSensorSource::reconnect() {
    connected_ = true;
}

Event SimulatedSensorSource::build_event(const SimulatedReading& reading) {
    auto obs_ts = reading.observation_timestamp.value_or(std::chrono::system_clock::now());

    nlohmann::json payload = reading.payload;
    // Always mark the event as synthetic so it can never be confused with a
    // real hardware reading.
    if (!payload.is_object()) {
        // build the synthetic marker on an object; if the caller supplied an
        // invalid payload we still tag it, and validation happens downstream.
        nlohmann::json tagged = nlohmann::json::object();
        tagged["synthetic"] = true;
        tagged["sensor_kind"] = (reading.kind == SimulatedSensorKind::motion) ? "motion"
                            : (reading.kind == SimulatedSensorKind::presence) ? "presence"
                            : "custom";
        tagged["payload"] = payload;
        payload = tagged;
    } else {
        payload["synthetic"] = true;
        payload["sensor_kind"] = (reading.kind == SimulatedSensorKind::motion) ? "motion"
                            : (reading.kind == SimulatedSensorKind::presence) ? "presence"
                            : "custom";
    }

    return Event(
        reading.event_id, 1, EventType::observation, reading.source,
        obs_ts, std::chrono::system_clock::now(),
        std::nullopt, std::nullopt, std::nullopt, payload,
        std::nullopt, std::nullopt, std::nullopt
    );
}

Event SimulatedSensorSource::build_malformed_event(const std::string& event_id, const std::string& source) {
    // Directly construct an event with an invalid (array) payload. The Event
    // constructor throws std::invalid_argument, which is the deterministic
    // malformed-input behaviour we want to exercise.
    nlohmann::json bad = nlohmann::json::array();
    return Event(
        event_id, 1, EventType::observation, source,
        std::chrono::system_clock::now(), std::chrono::system_clock::now(),
        std::nullopt, std::nullopt, std::nullopt, bad,
        std::nullopt, std::nullopt, std::nullopt
    );
}

std::optional<Event> SimulatedSensorSource::next() {
    // While disconnected, the source yields nothing (simulated outage).
    if (!connected_) {
        return std::nullopt;
    }
    if (index_ >= queue_.size()) {
        return std::nullopt;
    }

    const SimulatedReading& reading = queue_[index_++];
    emitted_++;

    // Malformed custom readings (array payload) throw on construction, matching
    // how a validator would reject bad input.
    if (reading.kind == SimulatedSensorKind::custom && reading.payload.is_array()) {
        return build_malformed_event(reading.event_id, reading.source);
    }

    return build_event(reading);
}

std::string SimulatedSensorSource::name() const {
    return "simulated_sensor";
}

} // namespace homeguardian
