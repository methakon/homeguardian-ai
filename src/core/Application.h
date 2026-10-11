#pragma once

#include "Config.h"
#include "Pipeline.h"
#include "SimulatorRuntime.h"
#include "AlertPolicy.h"
#include "PersistenceAudit.h"
#include "RuntimeStatus.h"
#include "AppState.h"
#include <atomic>
#include <memory>

namespace homeguardian {

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
    PersistenceAudit* get_persistence();   // nullptr if persistence unavailable
    RuntimeStatus& get_status();

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
    std::unique_ptr<PersistenceAudit> persistence_;   // may be null (degraded)
    RuntimeStatus status_;
    static Application* instance_;
};

} // namespace homeguardian
