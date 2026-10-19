#pragma once

#include "action/ActionContract.h"
#include "action/ActionOrchestrator.h"
#include "core/Alert.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace homeguardian::action {

// Semantic version for the Alert-to-Action adapter contract
inline constexpr const char* ALERT_ACTION_ADAPTER_CONTRACT_VERSION = "hg-alert-action/1.0.0";

// Configuration for mapping HomeGuardian Alert severity to local Action types
struct AlertActionMappingPolicy {
    // Invariant: info alerts can NEVER map to safety-critical or external actions
    ActionType info_action_type = ActionType::AuditEventGeneration;
    ActionType warning_action_type = ActionType::DashboardNotification;
    ActionType critical_action_type = ActionType::DashboardNotification;

    // Optional secondary action for critical alerts (e.g. simulated voice announcement)
    bool dispatch_secondary_for_critical = true;
    ActionType critical_secondary_action_type = ActionType::SimulatedVoiceAnnouncement;

    // Minimum confidence required for automated action dispatch
    double min_confidence = 0.70;

    // Default Time-To-Live for generated ActionRequests
    std::chrono::milliseconds default_ttl{300000}; // 5 minutes

    // Default target channels
    std::string default_dashboard_target = "local-dashboard";
    std::string default_voice_target = "local_default_speaker";
};

struct AdapterStatistics {
    uint64_t alerts_received = 0;
    uint64_t actions_dispatched = 0;
    uint64_t actions_completed = 0;
    uint64_t actions_refused = 0;
    uint64_t info_alerts_quarantined = 0;
};

// AlertActionAdapter: Bridges genuine HomeGuardian Alert instances to the
// ActionOrchestrator with strict severity gating, source identity resolution,
// and invariant enforcement.
class AlertActionAdapter {
public:
    explicit AlertActionAdapter(
        std::shared_ptr<ActionOrchestrator> orchestrator,
        AlertActionMappingPolicy policy = AlertActionMappingPolicy{}
    );
    ~AlertActionAdapter() = default;

    // Primary entry point: Consumes an Alert and dispatches authorized actions
    std::vector<ActionResult> on_alert(const Alert& alert, const nlohmann::json& context = nlohmann::json::object());

    // Provide a callable std::function suitable for Application::set_alert_sink
    std::function<void(const Alert&)> make_alert_sink();

    // Policy and configuration
    void set_policy(const AlertActionMappingPolicy& policy);
    AlertActionMappingPolicy get_policy() const;

    // Statistics and diagnostics
    AdapterStatistics get_statistics() const;
    void reset_statistics();

    // Helper: Resolves device source identity from Alert details or contributing events
    static std::string resolve_source_identity(const Alert& alert);

    // Invariant check: Verifies if a proposed action is permissible for the given severity
    static bool is_action_permissible_for_severity(Severity severity, ActionType action_type);

private:
    mutable std::mutex mutex_;
    std::shared_ptr<ActionOrchestrator> orchestrator_;
    AlertActionMappingPolicy policy_;
    AdapterStatistics stats_;
};

} // namespace homeguardian::action
