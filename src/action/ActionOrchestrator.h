#pragma once

#include "action/ActionContract.h"
#include "action/IActionProvider.h"
#include "action/ActionPolicyEngine.h"
#include "action/ActionAuditLog.h"
#include "core/Alert.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian::action {

class ActionOrchestrator {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    explicit ActionOrchestrator(
        std::shared_ptr<ActionPolicyEngine> policy_engine = nullptr,
        std::shared_ptr<ActionAuditLog> audit_log = nullptr,
        Clock clock = [] { return std::chrono::system_clock::now(); }
    );
    ~ActionOrchestrator() = default;

    // Component access
    std::shared_ptr<ActionPolicyEngine> policy_engine() const { return policy_engine_; }
    std::shared_ptr<ActionAuditLog> audit_log() const { return audit_log_; }

    // Provider registration
    void register_provider(std::shared_ptr<IActionProvider> provider);
    void unregister_provider(ActionType type);
    std::shared_ptr<IActionProvider> get_provider(ActionType type) const;

    // Direct Action Request submission
    ActionResult dispatch_action(const ActionRequest& request);

    // Event-to-Action consumption: converts an Alert into an authorized Action
    ActionResult process_alert(
        const Alert& alert,
        ActionType action_type,
        const std::string& target = "",
        const nlohmann::json& custom_payload = nlohmann::json::object(),
        std::chrono::milliseconds ttl = std::chrono::milliseconds(300000) // 5 min TTL
    );

    // Cancellation & Revocation
    bool cancel_action(const std::string& request_id);
    size_t revoke_device_actions(const std::string& device_id);

    // Diagnostics & In-flight inspection
    size_t active_in_flight_count() const;
    std::vector<ActionRequest> get_in_flight_requests() const;

private:
    std::string generate_request_id();
    AuthorizationScope map_default_scope(ActionType type) const;

    mutable std::mutex mutex_;
    Clock clock_;
    std::shared_ptr<ActionPolicyEngine> policy_engine_;
    std::shared_ptr<ActionAuditLog> audit_log_;
    std::unordered_map<ActionType, std::shared_ptr<IActionProvider>> providers_;

    uint64_t request_counter_ = 1000;
    std::unordered_map<std::string, ActionRequest> in_flight_requests_;
};

} // namespace homeguardian::action
