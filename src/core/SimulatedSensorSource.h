#pragma once

// A deterministic, simulated sensor source for Phase F1. It emits synthetic
// observations through the existing IEventSource interface. There is no real
// camera, microphone, or biometric hardware involved.
//
// Every generated event is explicitly marked synthetic in its payload so that
// downstream consumers can never mistake it for a real sensor reading.

#include "EventSource.h"
#include "Event.h"
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace homeguardian {

enum class SimulatedSensorKind {
    motion,      // synthetic motion observation
    presence,    // synthetic presence observation
    custom       // caller-defined payload
};

struct SimulatedReading {
    std::string event_id;
    std::string source;                 // e.g. "sensor.simulated.motion"
    SimulatedSensorKind kind = SimulatedSensorKind::motion;
    std::optional<std::chrono::system_clock::time_point> observation_timestamp; // override; nullopt = now at emit
    nlohmann::json payload = nlohmann::json::object();
};

class SimulatedSensorSource : public IEventSource {
public:
    SimulatedSensorSource();

    // Enqueue a reading to be emitted by subsequent next() calls.
    void enqueue(const SimulatedReading& reading);

    // Enqueue a raw string that will be turned into a malformed observation
    // (used to exercise validation/robustness). The malformed event carries an
    // invalid payload so that validators reject it deterministically.
    void enqueue_malformed(const std::string& event_id, const std::string& source);

    // Simulated connection control.
    void disconnect();
    void reconnect();
    bool is_connected() const { return connected_; }

    std::optional<Event> next() override;
    std::string name() const override;

    // Test/observability helpers.
    size_t pending() const { return queue_.size(); }
    size_t emitted_count() const { return emitted_; }

private:
    std::vector<SimulatedReading> queue_;
    size_t index_ = 0;
    bool connected_ = true;
    size_t emitted_ = 0;

    Event build_event(const SimulatedReading& reading);
    Event build_malformed_event(const std::string& event_id, const std::string& source);
};

} // namespace homeguardian
