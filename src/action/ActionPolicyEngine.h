#pragma once

#include "action/ActionContract.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian::action {

struct ActionPolicyRule {
    std::string rule_name;              // Alert rule identifier (e.g., "CriticalFallRule", "InactivityRule", "*")
    ActionType action_type;             // Permitted action type
    AuthorizationScope required_scope;  // Required owner scope mask bit
    double min_confidence = 0.75;       // Minimum confidence required to trigger action
    std::chrono::milliseconds cooldown{60000}; // Action-specific cooldown to suppress duplicates
    bool requires_explicit_consent = true;     // Must verify active ConsentGate grant
    bool enabled = true;
};

struct ActionEvaluationResult {
    bool allowed = false;
    ExecutionState state = ExecutionState::RefusedUnauthorized;
    std::string reason;
};

class ActionPolicyEngine {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;
    using ConsentChecker = std::function<bool(const std::string& device_id)>;

    ActionPolicyEngine(Clock clock = [] { return std::chrono::system_clock::now(); },
                       ConsentChecker consent_checker = [](const std::string&) { return true; });
    ~ActionPolicyEngine() = default;

    // Grant or revoke owner authorization scopes (default: ScopeNone = default deny)
    void set_granted_scopes(uint32_t scope_mask);
    uint32_t get_granted_scopes() const;
    void grant_scope(AuthorizationScope scope);
    void revoke_scope(AuthorizationScope scope);

    // Register action policy rules
    void add_rule(const ActionPolicyRule& rule);
    void clear_rules();

    // Configure consent checker hook (typically backed by ConsentGate)
    void set_consent_checker(ConsentChecker checker);

    // Core policy evaluation: applies default-deny, owner scope, confidence, expiration, and consent checks
    ActionEvaluationResult evaluate(const ActionRequest& request, bool is_target_online = true);

    // Duplicate check: checks if an action for this idempotency_key was evaluated recently
    bool is_duplicate(const std::string& idempotency_key);
    void record_execution(const std::string& idempotency_key, std::chrono::milliseconds cooldown);

    // Clear duplicate cache (test cleanup)
    void clear_duplicate_cache();

private:
    mutable std::mutex mutex_;
    Clock clock_;
    ConsentChecker consent_checker_;
    uint32_t granted_scopes_ = static_cast<uint32_t>(AuthorizationScope::ScopeNone);
    std::vector<ActionPolicyRule> rules_;

    struct CooldownRecord {
        std::chrono::system_clock::time_point last_executed;
        std::chrono::milliseconds cooldown;
    };
    std::unordered_map<std::string, CooldownRecord> cooldown_cache_;
};

} // namespace homeguardian::action
