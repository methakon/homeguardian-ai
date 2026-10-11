#pragma once

#include "Config.h"
#include "Pipeline.h"
#include "SimulatorRuntime.h"
#include "AlertPolicy.h"
#include <atomic>
#include <memory>

namespace homeguardian {

// Explicit application lifecycle state (observable, testable).
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

class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run(const std::string& config_path);
    void request_shutdown();
    
    void process_event(const Event& event);
    Pipeline& get_pipeline();

    // Simulator runtime accessors (simulator-only; never opens real hardware).
    SimulatorRuntime& get_simulator_runtime();
    AlertPolicy& get_alert_policy();

    // Lifecycle status + control (simulator-only; no hardware enable path).
    AppState state() const;
    bool is_healthy() const;   // true only while Running/Stopping cleanly

    static void signal_handler(int signal);

private:
    Config config_;
    std::atomic<bool> shutdown_flag_{false};
    std::atomic<AppState> app_state_{AppState::Constructed};
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<SimulatorRuntime> simulator_;
    std::unique_ptr<AlertPolicy> alert_policy_;
    static Application* instance_;
};

} // namespace homeguardian
