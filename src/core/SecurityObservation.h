#pragma once

// Security sensor observations: the ingestion model for home-security sources.
//
// WHY (from the `home-security-sensor-nodes` skill): the skill's architecture is
// phones-as-sensor-terminals streaming to a hub, with events - not psychology -
// as the target. HomeGuardian already owns the acquisition path (IMediaDevice,
// ConsentGuardedDevice, AAudio, Camera1/Camera2) and the event pipeline, so the
// gap this file fills is the OBSERVATION VOCABULARY between them: a validated,
// provenance-tagged, consent-context-carrying record for each security-relevant
// happening, converted into the EXISTING Observation -> Event -> Pipeline path.
//
// EXCLUDED ON PURPOSE (the skill's own boundary, kept): facial-emotion
// inference, voice stress/deception analysis, tremor/"stress" profiling and any
// identification of a person. Nothing here names, recognises or scores a human.
//
// SEVERITY: `suggested_severity()` can return info or warning ONLY. Critical is
// reserved for specifically validated high-priority conditions and the explicitly
// designated synthetic demonstration; a single sensor report is never that.

#include "core/Event.h"
#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

enum class SecuritySourceType {
    motion,
    contact,                     // door / window state
    presence,                    // occupancy change
    camera_availability,
    microphone_availability,
    device_availability,         // node/sensor online-offline
    tamper,                      // tamper or unexpected disconnect
    environmental_threshold      // an authorized environmental threshold crossing
};

inline const char* security_source_type_to_string(SecuritySourceType t) {
    switch (t) {
        case SecuritySourceType::motion: return "motion";
        case SecuritySourceType::contact: return "contact";
        case SecuritySourceType::presence: return "presence";
        case SecuritySourceType::camera_availability: return "camera_availability";
        case SecuritySourceType::microphone_availability: return "microphone_availability";
        case SecuritySourceType::device_availability: return "device_availability";
        case SecuritySourceType::tamper: return "tamper";
        case SecuritySourceType::environmental_threshold: return "environmental_threshold";
    }
    return "unknown";
}

std::optional<SecuritySourceType> security_source_type_from_string(const std::string& value);

struct SecurityObservation {
    std::string device_id;    // node id, entity id or synthetic fixture id (bounded)
    std::string source;       // producing system: node | home-assistant | simulator
    SecuritySourceType source_type = SecuritySourceType::motion;

    std::chrono::system_clock::time_point acquired_at{};  // when the sensor observed it
    std::chrono::system_clock::time_point ingested_at{};  // when HomeGuardian received it

    Provenance provenance = Provenance::synthetic;

    std::optional<double> confidence;  // [0,1] when the source reports one
    std::optional<double> quality;     // [0,1] input quality when meaningful

    std::string state;                 // on|off|open|closed|present|absent|online|offline|...
    std::string consent_subject;       // e.g. "node:hg-1234"
    std::string consent_purpose;       // e.g. "security_monitor"
    bool authorized = false;           // consent decision at ingestion time

    nlohmann::json features = nlohmann::json::object();  // bounded, non-sensitive
    std::string note;                                    // bounded explanation

    static constexpr size_t kMaxDeviceIdChars = 96;
    static constexpr size_t kMaxSourceChars = 32;
    static constexpr size_t kMaxStateChars = 32;
    static constexpr size_t kMaxNoteChars = 200;
    static constexpr size_t kMaxFeatureKeys = 16;
    static constexpr size_t kMaxFeatureJsonBytes = 1024;

    // Throws std::invalid_argument on any violation (same fail-fast contract as
    // Event/Observation).
    void validate() const;

    // Convert into the standard detection Observation so the record flows through
    // the EXISTING pipeline. detector_id is "sensor.<source_type>".
    Observation to_observation() const;

    // Conservative severity. info or warning ONLY - never critical.
    Severity suggested_severity() const;

    // Strict wire ingestion for a hub/transport path: every field is type-checked
    // and bounded, unknown values are rejected rather than defaulted.
    static std::optional<SecurityObservation> from_json(const nlohmann::json& value,
                                                        std::string* reason);
    static nlohmann::json to_json(const SecurityObservation& observation);
};

// Tracks device liveness WITHOUT fabricating events.
//
// The skill's own warning is the reason this exists: "Set a Last Will so a dead
// sensor is distinguishable from a silent one - that distinction is the entire
// safety argument". A tracker that emitted an event on every heartbeat would
// drown that distinction, so mark_seen() is silent unless the state CHANGED, and
// a reconnect produces an availability transition only - never a replay or an
// invented state event.
class DeviceAvailabilityTracker {
public:
    struct Config {
        std::chrono::seconds offline_after{90};  // no contact for this long -> offline
        size_t max_devices = 64;                 // bounded registry
        Config() = default;
    };

    struct Transition {
        std::string device_id;
        bool online = false;
        bool first_seen = false;
        std::chrono::system_clock::time_point at{};
        std::string reason;
    };

    static Config defaults() { return Config{}; }
    explicit DeviceAvailabilityTracker(Config cfg = defaults()) : cfg_(cfg) {}

    // Returns a transition ONLY when the online state changed (or on first sight).
    std::optional<Transition> mark_seen(const std::string& device_id,
                                        std::chrono::system_clock::time_point now);

    // Devices silent past `offline_after` become offline ONCE each.
    std::vector<Transition> sweep(std::chrono::system_clock::time_point now);

    bool online(const std::string& device_id) const;
    size_t device_count() const { return devices_.size(); }
    void clear();

private:
    struct Entry {
        std::string device_id;
        bool online = true;
        std::chrono::system_clock::time_point last_seen{};
        bool offline_reported = false;
    };

    Entry* find(const std::string& device_id);

    Config cfg_;
    std::vector<Entry> devices_{};
};

// Suppresses duplicate observations inside a cooldown window. Bounded: the oldest
// entry is evicted, so a hostile source cannot grow the table.
class SensorDeduplicator {
public:
    struct Key {
        std::string device_id;
        SecuritySourceType source_type = SecuritySourceType::motion;
        std::string state;
    };

    struct Config {
        std::chrono::seconds cooldown{60};
        size_t max_entries = 128;
        Config() = default;
    };

    static Config defaults() { return Config{}; }
    explicit SensorDeduplicator(Config cfg = defaults()) : cfg_(cfg) {}

    // True when this key was already seen within the cooldown (caller suppresses).
    bool duplicate(const Key& key, std::chrono::system_clock::time_point now);

    size_t size() const { return entries_.size(); }
    void clear();

private:
    struct Entry {
        Key key;
        std::chrono::system_clock::time_point seen_at{};
    };

    Config cfg_;
    std::vector<Entry> entries_{};
};

}  // namespace homeguardian
