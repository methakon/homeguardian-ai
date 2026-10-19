#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace homeguardian::action {

// Contract version string following semantic versioning
inline constexpr const char* ACTION_CONTRACT_VERSION = "hg-action/1.0.0";

// Supported Action Types
enum class ActionType {
    DashboardNotification,      // In-browser local UI notification
    AuditEventGeneration,       // Append-only tamper-evident local audit log
    SimulatedVoiceAnnouncement, // Local text-to-speech announcement on authorized speaker
    AutomationHook,             // Local sandboxed automation callback
    EmergencyEscalation         // Fenced emergency escalation (disabled by default)
};

inline std::string action_type_to_string(ActionType type) {
    switch (type) {
        case ActionType::DashboardNotification:      return "dashboard_notification";
        case ActionType::AuditEventGeneration:       return "audit_event_generation";
        case ActionType::SimulatedVoiceAnnouncement: return "simulated_voice_announcement";
        case ActionType::AutomationHook:             return "automation_hook";
        case ActionType::EmergencyEscalation:        return "emergency_escalation";
    }
    return "unknown";
}

inline ActionType action_type_from_string(const std::string& str) {
    if (str == "dashboard_notification")      return ActionType::DashboardNotification;
    if (str == "audit_event_generation")       return ActionType::AuditEventGeneration;
    if (str == "simulated_voice_announcement") return ActionType::SimulatedVoiceAnnouncement;
    if (str == "automation_hook")             return ActionType::AutomationHook;
    if (str == "emergency_escalation")        return ActionType::EmergencyEscalation;
    throw std::invalid_argument("Unknown ActionType: " + str);
}

// Required Authorization Scope (Owner-granted permissions)
enum class AuthorizationScope {
    ScopeNone = 0,               // Default deny
    ScopeLocalNotify = 1 << 0,   // Permission to dispatch local dashboard notifications
    ScopeLocalAudit = 1 << 1,    // Permission to append to audit ledger
    ScopeLocalVoice = 1 << 2,    // Permission to trigger local voice announcements
    ScopeLocalHook = 1 << 3,     // Permission to invoke local automation hooks
    ScopeEmergency = 1 << 4      // Explicit elevated permission for emergency escalation
};

inline uint32_t operator|(AuthorizationScope a, AuthorizationScope b) {
    return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}

inline bool has_scope(uint32_t granted_mask, AuthorizationScope required) {
    return (granted_mask & static_cast<uint32_t>(required)) == static_cast<uint32_t>(required);
}

inline std::string scope_to_string(AuthorizationScope scope) {
    switch (scope) {
        case AuthorizationScope::ScopeNone:        return "scope_none";
        case AuthorizationScope::ScopeLocalNotify: return "scope_local_notify";
        case AuthorizationScope::ScopeLocalAudit:  return "scope_local_audit";
        case AuthorizationScope::ScopeLocalVoice:  return "scope_local_voice";
        case AuthorizationScope::ScopeLocalHook:   return "scope_local_hook";
        case AuthorizationScope::ScopeEmergency:   return "scope_emergency";
    }
    return "unknown";
}

inline AuthorizationScope scope_from_string(const std::string& str) {
    if (str == "scope_none")        return AuthorizationScope::ScopeNone;
    if (str == "scope_local_notify") return AuthorizationScope::ScopeLocalNotify;
    if (str == "scope_local_audit")  return AuthorizationScope::ScopeLocalAudit;
    if (str == "scope_local_voice")  return AuthorizationScope::ScopeLocalVoice;
    if (str == "scope_local_hook")   return AuthorizationScope::ScopeLocalHook;
    if (str == "scope_emergency")    return AuthorizationScope::ScopeEmergency;
    throw std::invalid_argument("Unknown AuthorizationScope: " + str);
}

// Lifecycle Execution State
enum class ExecutionState {
    PendingAuthorization,
    Authorized,
    RefusedUnauthorized,
    RefusedConsentRevoked,
    RefusedLowConfidence,
    RefusedExpired,
    RefusedDuplicate,
    RefusedOffline,
    Executing,
    Completed,
    Failed,
    Cancelled
};

inline std::string execution_state_to_string(ExecutionState state) {
    switch (state) {
        case ExecutionState::PendingAuthorization: return "pending_authorization";
        case ExecutionState::Authorized:           return "authorized";
        case ExecutionState::RefusedUnauthorized:   return "refused_unauthorized";
        case ExecutionState::RefusedConsentRevoked: return "refused_consent_revoked";
        case ExecutionState::RefusedLowConfidence: return "refused_low_confidence";
        case ExecutionState::RefusedExpired:       return "refused_expired";
        case ExecutionState::RefusedDuplicate:     return "refused_duplicate";
        case ExecutionState::RefusedOffline:       return "refused_offline";
        case ExecutionState::Executing:            return "executing";
        case ExecutionState::Completed:            return "completed";
        case ExecutionState::Failed:               return "failed";
        case ExecutionState::Cancelled:            return "cancelled";
    }
    return "unknown";
}

inline ExecutionState execution_state_from_string(const std::string& str) {
    if (str == "pending_authorization") return ExecutionState::PendingAuthorization;
    if (str == "authorized")           return ExecutionState::Authorized;
    if (str == "refused_unauthorized")   return ExecutionState::RefusedUnauthorized;
    if (str == "refused_consent_revoked") return ExecutionState::RefusedConsentRevoked;
    if (str == "refused_low_confidence") return ExecutionState::RefusedLowConfidence;
    if (str == "refused_expired")       return ExecutionState::RefusedExpired;
    if (str == "refused_duplicate")     return ExecutionState::RefusedDuplicate;
    if (str == "refused_offline")       return ExecutionState::RefusedOffline;
    if (str == "executing")            return ExecutionState::Executing;
    if (str == "completed")            return ExecutionState::Completed;
    if (str == "failed")               return ExecutionState::Failed;
    if (str == "cancelled")            return ExecutionState::Cancelled;
    throw std::invalid_argument("Unknown ExecutionState: " + str);
}

// Versioned ActionRequest Contract
struct ActionRequest {
    std::string contract_version = ACTION_CONTRACT_VERSION;
    std::string request_id;
    std::string alert_id;
    std::string event_id;
    std::string source_identity;
    ActionType action_type = ActionType::DashboardNotification;
    std::string target;
    nlohmann::json payload;
    AuthorizationScope required_scope = AuthorizationScope::ScopeLocalNotify;
    std::string consent_version;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point expires_at;
    std::string idempotency_key;
    std::string provenance;
    double confidence = 1.0;
    uint32_t max_retries = 3;
    uint32_t retry_count = 0;

    inline void validate() const {
        if (request_id.empty()) throw std::invalid_argument("request_id cannot be empty");
        if (alert_id.empty()) throw std::invalid_argument("alert_id cannot be empty");
        if (source_identity.empty()) throw std::invalid_argument("source_identity cannot be empty");
        if (idempotency_key.empty()) throw std::invalid_argument("idempotency_key cannot be empty");
        if (confidence < 0.0 || confidence > 1.0) throw std::invalid_argument("confidence must be in [0.0, 1.0]");
        if (expires_at <= created_at) throw std::invalid_argument("expires_at must be after created_at");
    }

    inline nlohmann::json to_json() const {
        return {
            {"contract_version", contract_version},
            {"request_id", request_id},
            {"alert_id", alert_id},
            {"event_id", event_id},
            {"source_identity", source_identity},
            {"action_type", action_type_to_string(action_type)},
            {"target", target},
            {"payload", payload},
            {"required_scope", scope_to_string(required_scope)},
            {"consent_version", consent_version},
            {"created_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(created_at.time_since_epoch()).count()},
            {"expires_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(expires_at.time_since_epoch()).count()},
            {"idempotency_key", idempotency_key},
            {"provenance", provenance},
            {"confidence", confidence},
            {"max_retries", max_retries},
            {"retry_count", retry_count}
        };
    }

    static inline ActionRequest from_json(const nlohmann::json& j) {
        ActionRequest req;
        req.contract_version = j.value("contract_version", ACTION_CONTRACT_VERSION);
        req.request_id = j.at("request_id").get<std::string>();
        req.alert_id = j.at("alert_id").get<std::string>();
        req.event_id = j.value("event_id", "");
        req.source_identity = j.at("source_identity").get<std::string>();
        req.action_type = action_type_from_string(j.at("action_type").get<std::string>());
        req.target = j.value("target", "");
        req.payload = j.value("payload", nlohmann::json::object());
        req.required_scope = scope_from_string(j.value("required_scope", "scope_local_notify"));
        req.consent_version = j.value("consent_version", "v1.0");
        req.created_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("created_at_ms").get<int64_t>()));
        req.expires_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.at("expires_at_ms").get<int64_t>()));
        req.idempotency_key = j.at("idempotency_key").get<std::string>();
        req.provenance = j.value("provenance", "");
        req.confidence = j.value("confidence", 1.0);
        req.max_retries = j.value("max_retries", 3u);
        req.retry_count = j.value("retry_count", 0u);
        req.validate();
        return req;
    }
};

// Versioned ActionResult Contract
struct ActionResult {
    std::string contract_version = ACTION_CONTRACT_VERSION;
    std::string request_id;
    std::string idempotency_key;
    ActionType action_type = ActionType::DashboardNotification;
    ExecutionState state = ExecutionState::PendingAuthorization;
    bool success = false;
    std::string failure_reason;
    std::chrono::system_clock::time_point executed_at;
    uint64_t duration_ms = 0;
    nlohmann::json output = nlohmann::json::object();
    std::string audit_id;
    uint32_t attempts = 1;

    inline nlohmann::json to_json() const {
        return {
            {"contract_version", contract_version},
            {"request_id", request_id},
            {"idempotency_key", idempotency_key},
            {"action_type", action_type_to_string(action_type)},
            {"state", execution_state_to_string(state)},
            {"success", success},
            {"failure_reason", failure_reason},
            {"executed_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(executed_at.time_since_epoch()).count()},
            {"duration_ms", duration_ms},
            {"output", output},
            {"audit_id", audit_id},
            {"attempts", attempts}
        };
    }

    static inline ActionResult from_json(const nlohmann::json& j) {
        ActionResult res;
        res.contract_version = j.value("contract_version", ACTION_CONTRACT_VERSION);
        res.request_id = j.at("request_id").get<std::string>();
        res.idempotency_key = j.at("idempotency_key").get<std::string>();
        res.action_type = action_type_from_string(j.at("action_type").get<std::string>());
        res.state = execution_state_from_string(j.at("state").get<std::string>());
        res.success = j.value("success", false);
        res.failure_reason = j.value("failure_reason", "");
        res.executed_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(j.value("executed_at_ms", int64_t(0))));
        res.duration_ms = j.value("duration_ms", uint64_t(0));
        res.output = j.value("output", nlohmann::json::object());
        res.audit_id = j.value("audit_id", "");
        res.attempts = j.value("attempts", 1u);
        return res;
    }
};

} // namespace homeguardian::action
