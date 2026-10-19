#include "action/ActionPolicyEngine.h"

namespace homeguardian::action {

ActionPolicyEngine::ActionPolicyEngine(Clock clock, ConsentChecker consent_checker)
    : clock_(std::move(clock)), consent_checker_(std::move(consent_checker)) {}

void ActionPolicyEngine::set_granted_scopes(uint32_t scope_mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    granted_scopes_ = scope_mask;
}

uint32_t ActionPolicyEngine::get_granted_scopes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return granted_scopes_;
}

void ActionPolicyEngine::grant_scope(AuthorizationScope scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    granted_scopes_ |= static_cast<uint32_t>(scope);
}

void ActionPolicyEngine::revoke_scope(AuthorizationScope scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    granted_scopes_ &= ~static_cast<uint32_t>(scope);
}

void ActionPolicyEngine::add_rule(const ActionPolicyRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.push_back(rule);
}

void ActionPolicyEngine::clear_rules() {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.clear();
}

void ActionPolicyEngine::set_consent_checker(ConsentChecker checker) {
    std::lock_guard<std::mutex> lock(mutex_);
    consent_checker_ = std::move(checker);
}

ActionEvaluationResult ActionPolicyEngine::evaluate(const ActionRequest& request, bool is_target_online) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = clock_();

    // 1. Expiration check: expired authorizations are strictly rejected
    if (now >= request.expires_at) {
        return {false, ExecutionState::RefusedExpired, "Action request has expired"};
    }

    // 2. Offline check: if target device/service is offline, fail closed
    if (!is_target_online) {
        return {false, ExecutionState::RefusedOffline, "Target action provider or device is offline"};
    }

    // 3. Duplicate / Cooldown check: suppress re-execution of identical action
    auto it = cooldown_cache_.find(request.idempotency_key);
    if (it != cooldown_cache_.end()) {
        const auto elapsed = now - it->second.last_executed;
        if (elapsed < it->second.cooldown) {
            return {false, ExecutionState::RefusedDuplicate,
                    "Duplicate action suppressed: within cooldown interval"};
        }
    }

    // 4. Rule matching: find matching enabled policy rule
    const ActionPolicyRule* matching_rule = nullptr;
    for (const auto& r : rules_) {
        if (!r.enabled) continue;
        if (r.action_type != request.action_type) continue;
        if (r.rule_name.empty() || r.rule_name == "*" ||
            r.rule_name == request.provenance ||
            r.rule_name == request.payload.value("rule_name", "") ||
            r.rule_name == request.payload.value("rule", "")) {
            matching_rule = &r;
            break;
        }
    }

    // Default-deny: If no rule authorizes this action type, refuse
    if (!matching_rule) {
        return {false, ExecutionState::RefusedUnauthorized, "Default-deny: No policy rule permits this action type"};
    }

    // 5. Owner Scope Authorization check: owner must have granted the required scope
    if (!has_scope(granted_scopes_, matching_rule->required_scope)) {
        return {false, ExecutionState::RefusedUnauthorized,
                "Action refused: Required owner scope '" + scope_to_string(matching_rule->required_scope) + "' is not granted"};
    }

    // Also verify request's required_scope is satisfied
    if (!has_scope(granted_scopes_, request.required_scope)) {
        return {false, ExecutionState::RefusedUnauthorized,
                "Action refused: Request required scope '" + scope_to_string(request.required_scope) + "' is not granted"};
    }

    // 6. Confidence check: low-confidence experimental detections must not trigger automatic actions
    if (request.confidence < matching_rule->min_confidence) {
        return {false, ExecutionState::RefusedLowConfidence,
                "Confidence " + std::to_string(request.confidence) + " below policy threshold " + std::to_string(matching_rule->min_confidence)};
    }

    // 7. Consent Gate check: source device and target must have active consent grant
    if (matching_rule->requires_explicit_consent && consent_checker_) {
        if (!consent_checker_(request.source_identity)) {
            return {false, ExecutionState::RefusedConsentRevoked,
                    "Consent revoked or missing for source device: " + request.source_identity};
        }
        if (!request.target.empty() && !consent_checker_(request.target)) {
            return {false, ExecutionState::RefusedConsentRevoked,
                    "Consent revoked or missing for target device: " + request.target};
        }
    }

    return {true, ExecutionState::Authorized, "Action authorization approved by policy engine"};
}

bool ActionPolicyEngine::is_duplicate(const std::string& idempotency_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cooldown_cache_.find(idempotency_key);
    if (it == cooldown_cache_.end()) return false;
    const auto now = clock_();
    return (now - it->second.last_executed) < it->second.cooldown;
}

void ActionPolicyEngine::record_execution(const std::string& idempotency_key, std::chrono::milliseconds cooldown) {
    std::lock_guard<std::mutex> lock(mutex_);
    cooldown_cache_[idempotency_key] = {clock_(), cooldown};
}

void ActionPolicyEngine::clear_duplicate_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cooldown_cache_.clear();
}

} // namespace homeguardian::action
