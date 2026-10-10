#pragma once

// Minimal media-device interface for real camera/audio acquisition.
//
// This is an abstraction only: it defines the lifecycle contract that a real
// Android Camera2 / AAudio implementation must satisfy. It contains no
// hardware code. Concrete real-device backends are a later milestone and are
// NOT implemented here; mocks behind this interface are used to test lifecycle
// behavior without hardware.
//
// Lifecycle contract:
//   Idle -> (open/initialize) -> Initialized -> (start) -> Capturing
//   Capturing -> (stop) -> Stopped ; any state -> (close) -> Idle
//   Any state -> (fail) -> Error
// Resource cleanup is deterministic: stop() releases capture resources and the
// destructor guarantees stop()+close() so a device is never left capturing.

#include <cstdint>
#include <string>

namespace homeguardian {

enum class DeviceKind {
    camera,
    audio
};

enum class DeviceState {
    Idle,        // constructed, not initialized
    Initialized, // opened/initialized, not capturing
    Capturing,   // actively acquiring frames/samples
    Stopped,     // capture stopped, resources may still be held
    Error        // device failed; must be closed before reuse
};

inline const char* device_state_to_string(DeviceState s) {
    switch (s) {
        case DeviceState::Idle: return "Idle";
        case DeviceState::Initialized: return "Initialized";
        case DeviceState::Capturing: return "Capturing";
        case DeviceState::Stopped: return "Stopped";
        case DeviceState::Error: return "Error";
    }
    return "Unknown";
}

inline const char* device_kind_to_string(DeviceKind k) {
    switch (k) {
        case DeviceKind::camera: return "camera";
        case DeviceKind::audio: return "audio";
    }
    return "Unknown";
}

class IMediaDevice {
public:
    virtual ~IMediaDevice() = default;

    virtual DeviceKind kind() const = 0;

    // Initialize / open the device. Transitions Idle -> Initialized.
    // Throws std::runtime_error on failure and moves to Error.
    virtual void initialize() = 0;

    // Begin capture. Requires Initialized. Transitions Initialized -> Capturing.
    // Throws std::runtime_error if not initialized or on failure.
    virtual void start() = 0;

    // Stop capture and release capture resources. Transitions Capturing ->
    // Stopped. Safe to call when not capturing (no-op). Never throws.
    virtual void stop() noexcept = 0;

    // Close the device and release all resources. Transitions to Idle.
    // Safe to call from any state. Never throws.
    virtual void close() noexcept = 0;

    // Inject a failure (simulates device error / OS revocation). Moves to Error
    // and releases capture resources. Used to exercise failure handling.
    virtual void fail() noexcept = 0;

    virtual DeviceState state() const = 0;
    virtual std::string name() const = 0;
};

} // namespace homeguardian
