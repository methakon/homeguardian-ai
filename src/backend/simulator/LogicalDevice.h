#pragma once

// Logical device model for the dual-device simulation.
//
// A "logical device" is a first-class identity that participates in the shared
// audio broker. Each has its own identity, capabilities, lifecycle, status,
// event history, and configuration. Logical devices are INDEPENDENT: the state
// of one never affects the other.
//
// IMPORTANT (honesty about what this is): a logical device is a HomeGuardian
// simulation entity. It does NOT run Amazon's original Alexa OS, and it makes no
// claim to be an Alexa product. "Alexa-enabled" here means only that the logical
// device is configured to consume the shared microphone and emit speaker output
// through the HomeGuardian audio broker — a local, simulated behavior. Any
// real Alexa integration is a separate, optional workstream (see alexa_skill/)
// and is not implied by these entities.

#include "core/Event.h"
#include "core/IMediaDevice.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace homeguardian {

// Well-known ids for the two registered logical devices.
inline constexpr const char* kLogicalDeviceAlexaEnabled = "simulated_alexa_enabled_device";
inline constexpr const char* kLogicalDeviceAlexaAssistant = "simulated_alexa_assistant";

// Capabilities a logical device may advertise. Deliberately coarse and local:
// none of these imply cloud or Amazon functionality.
struct DeviceCapabilities {
    bool consumes_microphone = false;   // subscribes to the shared mic source
    bool drives_speaker = false;        // may enqueue speaker output
    bool has_camera = false;            // separate simulated camera peripheral
};

// Lifecycle + status of a logical device, mirroring IMediaDevice's state model
// but at the logical (not hardware) layer.
enum class LogicalDeviceState {
    Idle,
    Initialized,
    Active,     // running and consuming/emitting
    Stopped,
    Error
};

inline const char* logical_device_state_to_string(LogicalDeviceState s) {
    switch (s) {
        case LogicalDeviceState::Idle: return "Idle";
        case LogicalDeviceState::Initialized: return "Initialized";
        case LogicalDeviceState::Active: return "Active";
        case LogicalDeviceState::Stopped: return "Stopped";
        case LogicalDeviceState::Error: return "Error";
    }
    return "Unknown";
}

// Per-device configuration (local only). Retained and exposed for inspection;
// changing it never touches another logical device.
struct LogicalDeviceConfig {
    std::string display_name;
    DeviceCapabilities capabilities;
    size_t max_event_history = 128;   // bounded per-device history
};

class LogicalDevice {
public:
    LogicalDevice(std::string id, LogicalDeviceConfig cfg);

    const std::string& id() const { return id_; }
    const LogicalDeviceConfig& config() const { return config_; }

    // --- Lifecycle (independent per device) ---
    void initialize();                 // Idle -> Initialized
    void activate();                   // Initialized -> Active
    void stop();                       // Active -> Stopped
    void close();                      // -> Idle (releases resources)
    void fail();                       // -> Error
    void recover();                    // Error -> Idle

    LogicalDeviceState state() const;

    // --- Bounded event history (per device) ---
    // Appends an observation to this device's history. Bounded by
    // config().max_event_history; overflow drops the oldest (never unbounded).
    void record_event(Event ev);
    size_t event_history_size() const;
    std::vector<Event> recent_events() const;
    void clear_event_history();

    // --- Introspection ---
    int init_count() const { return init_count_; }
    int activate_count() const { return activate_count_; }
    int stop_count() const { return stop_count_; }
    int close_count() const { return close_count_; }
    bool released() const { return released_; }

private:
    std::string id_;
    LogicalDeviceConfig config_;

    mutable std::mutex m_;
    LogicalDeviceState state_ = LogicalDeviceState::Idle;
    bool released_ = false;
    bool has_error_ = false;

    int init_count_ = 0;
    int activate_count_ = 0;
    int stop_count_ = 0;
    int close_count_ = 0;

    std::deque<Event> event_history_;
};

} // namespace homeguardian
