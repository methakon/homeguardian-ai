#include "action/AlertActionAdapter.h"
#include <sstream>

namespace homeguardian::action {

AlertActionAdapter::AlertActionAdapter(
    std::shared_ptr<ActionOrchestrator> orchestrator,
    AlertActionMappingPolicy policy)
    : orchestrator_(std::move(orchestrator)), policy_(std::move(policy)) {
    if (!orchestrator_) {
        throw std::invalid_argument("AlertActionAdapter requires a non-null ActionOrchestrator");
    }
}

std::string AlertActionAdapter::resolve_source_identity(const Alert& alert) {
    if (alert.details.is_object()) {
        if (alert.details.contains("device_id") && alert.details["device_id"].is_string()) {
            std::string id = alert.details["device_id"].get<std::string>();
            if (!id.empty()) return id;
        }
        if (alert.details.contains("source") && alert.details["source"].is_string()) {
            std::string id = alert.details["source"].get<std::string>();
            if (!id.empty()) return id;
        }
        if (alert.details.contains("source_identity") && alert.details["source_identity"].is_string()) {
            std::string id = alert.details["source_identity"].get<std::string>();
            if (!id.empty()) return id;
        }
    }
    if (!alert.contributing_event_ids.empty()) {
        // Associate causal event prefix if recognizable
        const std::string& first_evt = alert.contributing_event_ids.front();
        if (first_evt.rfind("cam-", 0) == 0 || first_evt.rfind("node-", 0) == 0 || first_evt.rfind("hg-net", 0) == 0) {
            return first_evt;
        }
    }
    return "system-sensor-bridge";
}

bool AlertActionAdapter::is_action_permissible_for_severity(Severity severity, ActionType action_type) {
    // Invariant: INFO alerts can NEVER trigger external, physical, or voice actions
    if (severity == Severity::info) {
        if (action_type == ActionType::SimulatedVoiceAnnouncement ||
            action_type == ActionType::AutomationHook ||
            action_type == ActionType::EmergencyEscalation) {
            return false;
        }
        return true;
    }

    // Invariant: EmergencyEscalation is strictly fenced across all severities
    if (action_type == ActionType::EmergencyEscalation) {
        return false;
    }

    return true;
}

std::vector<ActionResult> AlertActionAdapter::on_alert(const Alert& alert, const nlohmann::json& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.alerts_received++;

    std::vector<ActionResult> results;

    // 1. Determine primary action type according to severity mapping
    ActionType primary_action = policy_.info_action_type;
    std::string primary_target;
    switch (alert.severity) {
        case Severity::info:
            primary_action = policy_.info_action_type;
            primary_target = (primary_action == ActionType::DashboardNotification)
                             ? policy_.default_dashboard_target : "";
            break;
        case Severity::warning:
            primary_action = policy_.warning_action_type;
            primary_target = policy_.default_dashboard_target;
            break;
        case Severity::critical:
            primary_action = policy_.critical_action_type;
            primary_target = policy_.default_dashboard_target;
            break;
    }

    // 2. Invariant Check: Verify action permissibility for severity
    if (!is_action_permissible_for_severity(alert.severity, primary_action)) {
        stats_.info_alerts_quarantined++;
        stats_.actions_refused++;

        ActionResult quarantined_res;
        quarantined_res.contract_version = ACTION_CONTRACT_VERSION;
        quarantined_res.request_id = "quarantine-" + alert.alert_id;
        quarantined_res.idempotency_key = alert.alert_id + ":quarantined";
        quarantined_res.action_type = primary_action;
        quarantined_res.state = ExecutionState::RefusedUnauthorized;
        quarantined_res.success = false;
        quarantined_res.failure_reason = "Safety Invariant Refusal: Severity level does not authorize requested action type";
        quarantined_res.executed_at = std::chrono::system_clock::now();

        // Record security audit event
        orchestrator_->audit_log()->record_security_event(
            "ACTION_QUARANTINED_SEVERITY_MISMATCH",
            quarantined_res.request_id,
            quarantined_res.failure_reason,
            {{"alert_id", alert.alert_id}, {"severity", alert.severity}}
        );

        results.push_back(quarantined_res);
        return results;
    }

    // 3. Prepare contextual payload
    nlohmann::json custom_payload = {
        {"adapter_version", ALERT_ACTION_ADAPTER_CONTRACT_VERSION},
        {"source_identity", resolve_source_identity(alert)},
        {"severity", alert.severity},
        {"rule_name", alert.rule_name},
        {"context", context}
    };

    // 4. Dispatch primary action
    stats_.actions_dispatched++;
    ActionResult primary_res = orchestrator_->process_alert(
        alert,
        primary_action,
        primary_target,
        custom_payload,
        policy_.default_ttl
    );

    if (primary_res.success) {
        stats_.actions_completed++;
    } else {
        stats_.actions_refused++;
    }
    results.push_back(primary_res);

    // 5. If Critical alert and secondary action is configured, dispatch secondary
    if (alert.severity == Severity::critical && policy_.dispatch_secondary_for_critical) {
        ActionType secondary_action = policy_.critical_secondary_action_type;
        if (is_action_permissible_for_severity(alert.severity, secondary_action)) {
            std::string secondary_target = (secondary_action == ActionType::SimulatedVoiceAnnouncement)
                                           ? policy_.default_voice_target : "";
            
            nlohmann::json secondary_payload = custom_payload;
            secondary_payload["secondary_dispatch"] = true;
            if (secondary_action == ActionType::SimulatedVoiceAnnouncement) {
                secondary_payload["spoken_text"] = "Critical Alert: " + alert.message;
            }

            stats_.actions_dispatched++;
            ActionResult secondary_res = orchestrator_->process_alert(
                alert,
                secondary_action,
                secondary_target,
                secondary_payload,
                policy_.default_ttl
            );

            if (secondary_res.success) {
                stats_.actions_completed++;
            } else {
                stats_.actions_refused++;
            }
            results.push_back(secondary_res);
        }
    }

    return results;
}

std::function<void(const Alert&)> AlertActionAdapter::make_alert_sink() {
    return [this](const Alert& alert) {
        (void)this->on_alert(alert);
    };
}

void AlertActionAdapter::set_policy(const AlertActionMappingPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = policy;
}

AlertActionMappingPolicy AlertActionAdapter::get_policy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_;
}

AdapterStatistics AlertActionAdapter::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void AlertActionAdapter::reset_statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = AdapterStatistics{};
}

} // namespace homeguardian::action
