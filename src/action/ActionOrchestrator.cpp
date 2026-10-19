#include "action/ActionOrchestrator.h"
#include <sstream>

namespace homeguardian::action {

ActionOrchestrator::ActionOrchestrator(
    std::shared_ptr<ActionPolicyEngine> policy_engine,
    std::shared_ptr<ActionAuditLog> audit_log,
    Clock clock)
    : clock_(std::move(clock)),
      policy_engine_(policy_engine ? policy_engine : std::make_shared<ActionPolicyEngine>()),
      audit_log_(audit_log ? audit_log : std::make_shared<ActionAuditLog>()) {}

void ActionOrchestrator::register_provider(std::shared_ptr<IActionProvider> provider) {
    if (!provider) return;
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[provider->get_type()] = provider;
}

void ActionOrchestrator::unregister_provider(ActionType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.erase(type);
}

std::shared_ptr<IActionProvider> ActionOrchestrator::get_provider(ActionType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(type);
    if (it != providers_.end()) {
        return it->second;
    }
    return nullptr;
}

std::string ActionOrchestrator::generate_request_id() {
    std::ostringstream ss;
    ss << "act-req-" << clock_().time_since_epoch().count() << "-" << (++request_counter_);
    return ss.str();
}

AuthorizationScope ActionOrchestrator::map_default_scope(ActionType type) const {
    switch (type) {
        case ActionType::DashboardNotification:      return AuthorizationScope::ScopeLocalNotify;
        case ActionType::AuditEventGeneration:       return AuthorizationScope::ScopeLocalAudit;
        case ActionType::SimulatedVoiceAnnouncement: return AuthorizationScope::ScopeLocalVoice;
        case ActionType::AutomationHook:             return AuthorizationScope::ScopeLocalHook;
        case ActionType::EmergencyEscalation:        return AuthorizationScope::ScopeEmergency;
    }
    return AuthorizationScope::ScopeNone;
}

ActionResult ActionOrchestrator::dispatch_action(const ActionRequest& request) {
    // 1. Audit log receipt of action request
    audit_log_->record_request(request, "ACTION_REQUEST_RECEIVED");

    // 2. Validate provider availability
    std::shared_ptr<IActionProvider> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = providers_.find(request.action_type);
        if (it != providers_.end()) {
            provider = it->second;
        }
    }

    if (!provider) {
        ActionResult res;
        res.request_id = request.request_id;
        res.idempotency_key = request.idempotency_key;
        res.action_type = request.action_type;
        res.state = ExecutionState::RefusedUnauthorized;
        res.success = false;
        res.failure_reason = "No provider registered for action type: " + action_type_to_string(request.action_type);
        audit_log_->record_result(res, "ACTION_REFUSED_NO_PROVIDER");
        return res;
    }

    // 3. Policy Engine Evaluation (default-deny, owner scopes, confidence, consent, expiration, cooldown)
    const bool is_online = provider->is_online();
    ActionEvaluationResult eval = policy_engine_->evaluate(request, is_online);

    if (!eval.allowed) {
        ActionResult res;
        res.request_id = request.request_id;
        res.idempotency_key = request.idempotency_key;
        res.action_type = request.action_type;
        res.state = eval.state;
        res.success = false;
        res.failure_reason = eval.reason;
        audit_log_->record_result(res, "ACTION_EVALUATION_REFUSED");
        return res;
    }

    // 4. Record Authorization Approval
    audit_log_->record_event(
        request.request_id,
        request.alert_id,
        request.action_type,
        "ACTION_AUTHORIZED_BY_POLICY",
        ExecutionState::Authorized,
        eval.reason
    );

    // Track in-flight request
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_requests_[request.request_id] = request;
    }

    // 5. Execution & Retry Loop
    audit_log_->record_event(
        request.request_id,
        request.alert_id,
        request.action_type,
        "ACTION_DISPATCHED_TO_PROVIDER",
        ExecutionState::Executing,
        "Dispatching to " + provider->get_name()
    );

    ActionResult final_res;
    uint32_t attempts = 0;
    const uint32_t max_attempts = (request.max_retries == 0) ? 1 : (request.max_retries + 1);

    while (attempts < max_attempts) {
        attempts++;
        final_res = provider->execute(request);
        final_res.attempts = attempts;

        if (final_res.success) {
            break;
        }

        // Check if request was cancelled while executing
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (in_flight_requests_.find(request.request_id) == in_flight_requests_.end()) {
                final_res.state = ExecutionState::Cancelled;
                final_res.success = false;
                final_res.failure_reason = "Action was cancelled during retry execution";
                break;
            }
        }

        if (attempts < max_attempts) {
            audit_log_->record_event(
                request.request_id,
                request.alert_id,
                request.action_type,
                "ACTION_RETRY_SCHEDULED",
                ExecutionState::Executing,
                "Attempt " + std::to_string(attempts) + " failed: " + final_res.failure_reason
            );
        }
    }

    // 6. Record Cooldown and Audit Result
    if (final_res.success) {
        // Record successful execution cooldown
        policy_engine_->record_execution(request.idempotency_key, std::chrono::milliseconds(60000));
        audit_log_->record_result(final_res, "ACTION_COMPLETED_SUCCESSFULLY");
    } else if (final_res.state == ExecutionState::Cancelled) {
        audit_log_->record_result(final_res, "ACTION_CANCELLED");
    } else {
        final_res.state = ExecutionState::Failed;
        audit_log_->record_result(final_res, "ACTION_FAILED_EXHAUSTED_RETRIES");
    }

    // Clean up in-flight tracking
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_requests_.erase(request.request_id);
    }

    return final_res;
}

ActionResult ActionOrchestrator::process_alert(
    const Alert& alert,
    ActionType action_type,
    const std::string& target,
    const nlohmann::json& custom_payload,
    std::chrono::milliseconds ttl) {
    const auto now = clock_();

    ActionRequest req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        req.request_id = generate_request_id();
    }
    req.alert_id = alert.alert_id;
    req.event_id = alert.contributing_event_ids.empty() ? "" : alert.contributing_event_ids.front();
    req.source_identity = alert.details.value("device_id", "system-sensor-bridge");
    req.action_type = action_type;
    req.target = target;
    req.required_scope = map_default_scope(action_type);
    req.consent_version = "v1.0";
    req.created_at = now;
    req.expires_at = now + ttl;
    req.idempotency_key = alert.alert_id + ":" + action_type_to_string(action_type);
    req.provenance = alert.rule_name;
    req.confidence = alert.confidence;
    req.max_retries = 3;
    req.retry_count = 0;

    // Merge alert payload with custom payload
    req.payload = {
        {"alert_id", alert.alert_id},
        {"rule_name", alert.rule_name},
        {"severity", alert.severity},
        {"message", alert.message},
        {"confidence", alert.confidence},
        {"details", alert.details}
    };
    for (auto it = custom_payload.begin(); it != custom_payload.end(); ++it) {
        req.payload[it.key()] = it.value();
    }

    return dispatch_action(req);
}

bool ActionOrchestrator::cancel_action(const std::string& request_id) {
    std::shared_ptr<IActionProvider> provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = in_flight_requests_.find(request_id);
        if (it == in_flight_requests_.end()) {
            return false;
        }
        auto prov_it = providers_.find(it->second.action_type);
        if (prov_it != providers_.end()) {
            provider = prov_it->second;
        }
        in_flight_requests_.erase(it);
    }

    if (provider) {
        provider->cancel(request_id);
    }

    audit_log_->record_event(
        request_id,
        "",
        ActionType::DashboardNotification,
        "ACTION_EXPLICITLY_CANCELLED",
        ExecutionState::Cancelled,
        "Action cancelled by operator or revocation"
    );
    return true;
}

size_t ActionOrchestrator::revoke_device_actions(const std::string& device_id) {
    std::vector<std::string> to_cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [req_id, req] : in_flight_requests_) {
            if (req.source_identity == device_id || req.target == device_id) {
                to_cancel.push_back(req_id);
            }
        }
    }

    size_t cancelled = 0;
    for (const auto& id : to_cancel) {
        if (cancel_action(id)) {
            cancelled++;
        }
    }
    return cancelled;
}

size_t ActionOrchestrator::active_in_flight_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_requests_.size();
}

std::vector<ActionRequest> ActionOrchestrator::get_in_flight_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActionRequest> list;
    list.reserve(in_flight_requests_.size());
    for (const auto& [_, req] : in_flight_requests_) {
        list.push_back(req);
    }
    return list;
}

} // namespace homeguardian::action
