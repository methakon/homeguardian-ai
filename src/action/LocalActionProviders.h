#pragma once

#include "action/IActionProvider.h"
#include "action/ActionAuditLog.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian::action {

// 1. Dashboard Notification Provider (Local In-Memory / Display Queue)
class DashboardNotificationProvider : public IActionProvider {
public:
    DashboardNotificationProvider(size_t max_history = 100);
    ~DashboardNotificationProvider() override = default;

    ActionType get_type() const override { return ActionType::DashboardNotification; }
    std::string get_name() const override { return "local_dashboard_notification_provider"; }
    bool is_online() const override;
    void set_online(bool online);

    ActionResult execute(const ActionRequest& request) override;
    void cancel(const std::string& request_id) override;

    struct Notification {
        std::string request_id;
        std::string alert_id;
        std::string title;
        std::string message;
        std::chrono::system_clock::time_point timestamp;
        nlohmann::json payload;
    };

    std::vector<Notification> get_notifications() const;
    void clear();

private:
    mutable std::mutex mutex_;
    bool is_online_ = true;
    size_t max_history_;
    std::vector<Notification> notifications_;
};

// 2. Local Audit Log Provider
class LocalAuditLogProvider : public IActionProvider {
public:
    explicit LocalAuditLogProvider(std::shared_ptr<ActionAuditLog> audit_log);
    ~LocalAuditLogProvider() override = default;

    ActionType get_type() const override { return ActionType::AuditEventGeneration; }
    std::string get_name() const override { return "local_audit_log_provider"; }
    bool is_online() const override { return true; }

    ActionResult execute(const ActionRequest& request) override;
    void cancel(const std::string& request_id) override;

private:
    std::shared_ptr<ActionAuditLog> audit_log_;
};

// 3. Simulated Voice Announcement Provider
class SimulatedVoiceAnnouncementProvider : public IActionProvider {
public:
    SimulatedVoiceAnnouncementProvider();
    ~SimulatedVoiceAnnouncementProvider() override = default;

    ActionType get_type() const override { return ActionType::SimulatedVoiceAnnouncement; }
    std::string get_name() const override { return "simulated_voice_announcement_provider"; }
    bool is_online() const override;
    void set_online(bool online);

    ActionResult execute(const ActionRequest& request) override;
    void cancel(const std::string& request_id) override;

    struct SpokenAnnouncement {
        std::string request_id;
        std::string speaker_target;
        std::string spoken_text;
        std::chrono::system_clock::time_point timestamp;
    };

    std::vector<SpokenAnnouncement> get_announcements() const;
    void clear();

private:
    mutable std::mutex mutex_;
    bool is_online_ = true;
    std::vector<SpokenAnnouncement> announcements_;
};

// 4. Sandboxed Local Automation Hook Provider
class AutomationHookProvider : public IActionProvider {
public:
    using HookCallback = std::function<nlohmann::json(const ActionRequest&)>;

    AutomationHookProvider();
    ~AutomationHookProvider() override = default;

    ActionType get_type() const override { return ActionType::AutomationHook; }
    std::string get_name() const override { return "sandboxed_automation_hook_provider"; }
    bool is_online() const override;
    void set_online(bool online);

    // Register a sandboxed local automation hook
    void register_hook(const std::string& hook_name, HookCallback callback);
    void unregister_hook(const std::string& hook_name);

    ActionResult execute(const ActionRequest& request) override;
    void cancel(const std::string& request_id) override;

private:
    mutable std::mutex mutex_;
    bool is_online_ = true;
    std::unordered_map<std::string, HookCallback> hooks_;
};

// 5. Configurable Mock Provider with Failure & Latency Injection (for testing)
class MockActionProvider : public IActionProvider {
public:
    explicit MockActionProvider(ActionType type = ActionType::DashboardNotification,
                                const std::string& name = "mock_provider");
    ~MockActionProvider() override = default;

    ActionType get_type() const override { return type_; }
    std::string get_name() const override { return name_; }
    bool is_online() const override;
    void set_online(bool online);

    void set_should_fail(bool fail, const std::string& reason = "Injected mock failure");
    void fail_first_n_times(uint32_t count, const std::string& reason = "Transient mock network failure");
    void set_simulated_delay_ms(uint32_t delay_ms);

    ActionResult execute(const ActionRequest& request) override;
    void cancel(const std::string& request_id) override;

    // Inspection
    size_t get_call_count() const;
    std::vector<ActionRequest> get_received_requests() const;
    std::vector<std::string> get_cancelled_request_ids() const;
    void reset();

private:
    mutable std::mutex mutex_;
    ActionType type_;
    std::string name_;
    bool is_online_ = true;
    bool should_fail_ = false;
    std::string failure_reason_;
    uint32_t fail_countdown_ = 0;
    std::string transient_reason_;
    uint32_t delay_ms_ = 0;
    size_t call_count_ = 0;
    std::vector<ActionRequest> received_requests_;
    std::vector<std::string> cancelled_requests_;
};

} // namespace homeguardian::action
