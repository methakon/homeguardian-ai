#pragma once

#include "Config.h"
#include "Pipeline.h"
#include "SimulatorRuntime.h"
#include "AlertPolicy.h"
#include "PersistenceAudit.h"
#include "RuntimeStatus.h"
#include "AppState.h"
#include <atomic>
#include <functional>
#include <memory>

namespace homeguardian {

class DetectionFusionRule;

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

    // Optional observer invoked whenever an alert is DELIVERED (i.e. passed the
    // dedup/cooldown policy) by process_event(). Passive: it never changes
    // delivery, ordering, persistence, or status. Non-owning; pass nullptr to
    // detach. Used by the demo scenario runner to capture the REAL alerts so
    // they (and only they) are persisted and shown. Never receives raw media.
    void set_alert_sink(std::function<void(const Alert&)> sink);

    // L-12: the detector ids the detection/fusion rule currently treats as
    // fusion EVIDENCE. Empty means "all detectors". The default is the activity
    // detector only, so statistical anomaly evidence is advisory unless it is
    // explicitly added here.
    std::vector<std::string> evidence_detector_ids() const;
    void set_evidence_detectors(std::vector<std::string> ids);

    // Lifecycle status + control (simulator-only; no hardware enable path).
    AppState state() const;
    bool is_healthy() const;   // true only while Running/Stopping cleanly

    static void signal_handler(int signal);

    // L-11: run one detection step because an AUTHORIZED acquisition occurred.
    // Feeds the detector's Observation through process_event() so the
    // detection/fusion rule can decide whether to alert. Simulator-only.
    void run_detection_step(int index);

private:
    Config config_;
    std::atomic<bool> shutdown_flag_{false};
    std::atomic<AppState> app_state_{AppState::Constructed};
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<SimulatorRuntime> simulator_;
    std::unique_ptr<AlertPolicy> alert_policy_;
    std::shared_ptr<DetectionFusionRule> detection_rule_;  // L-12 evidence allowlist
    std::unique_ptr<PersistenceAudit> persistence_;   // may be null (degraded)
    RuntimeStatus status_;
    std::function<void(const Alert&)> alert_sink_;    // optional delivered-alert observer
    static Application* instance_;
};

} // namespace homeguardian
