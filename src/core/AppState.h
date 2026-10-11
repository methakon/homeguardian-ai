#pragma once

// Explicit application lifecycle state (observable, testable). Kept in its own
// header so RuntimeStatus and Application can both use it without a circular
// include.

namespace homeguardian {

enum class AppState {
    Constructed,   // created, not started
    Starting,      // run() in progress before the main loop
    Running,       // main loop active
    Stopping,      // shutdown requested
    Stopped,       // cleanly exited
    Failed         // startup or runtime failure; not healthy
};

inline const char* app_state_to_string(AppState s) {
    switch (s) {
        case AppState::Constructed: return "Constructed";
        case AppState::Starting: return "Starting";
        case AppState::Running: return "Running";
        case AppState::Stopping: return "Stopping";
        case AppState::Stopped: return "Stopped";
        case AppState::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace homeguardian
