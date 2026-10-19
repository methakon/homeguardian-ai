#include "action/LocalActionProviders.h"
#include <thread>

namespace homeguardian::action {

// -----------------------------------------------------------------------------
// 1. DashboardNotificationProvider
// -----------------------------------------------------------------------------
DashboardNotificationProvider::DashboardNotificationProvider(size_t max_history)
    : max_history_(max_history) {}

bool DashboardNotificationProvider::is_online() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_online_;
}

void DashboardNotificationProvider::set_online(bool online) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_online_ = online;
}

ActionResult DashboardNotificationProvider::execute(const ActionRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto start = std::chrono::steady_clock::now();

    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = ActionType::DashboardNotification;
    res.executed_at = std::chrono::system_clock::now();

    if (!is_online_) {
        res.state = ExecutionState::RefusedOffline;
        res.success = false;
        res.failure_reason = "Dashboard notification queue is offline";
        return res;
    }

    Notification notif;
    notif.request_id = request.request_id;
    notif.alert_id = request.alert_id;
    notif.title = request.payload.value("title", "HomeGuardian Safety Notification");
    notif.message = request.payload.value("message", "Safety event observed");
    notif.timestamp = res.executed_at;
    notif.payload = request.payload;

    if (notifications_.size() >= max_history_) {
        notifications_.erase(notifications_.begin());
    }
    notifications_.push_back(notif);

    const auto end = std::chrono::steady_clock::now();
    res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    res.state = ExecutionState::Completed;
    res.success = true;
    res.output = {
        {"status", "delivered_to_dashboard_queue"},
        {"notification_count", notifications_.size()},
        {"title", notif.title}
    };
    return res;
}

void DashboardNotificationProvider::cancel(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = notifications_.begin(); it != notifications_.end(); ++it) {
        if (it->request_id == request_id) {
            notifications_.erase(it);
            break;
        }
    }
}

std::vector<DashboardNotificationProvider::Notification> DashboardNotificationProvider::get_notifications() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return notifications_;
}

void DashboardNotificationProvider::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    notifications_.clear();
}

// -----------------------------------------------------------------------------
// 2. LocalAuditLogProvider
// -----------------------------------------------------------------------------
LocalAuditLogProvider::LocalAuditLogProvider(std::shared_ptr<ActionAuditLog> audit_log)
    : audit_log_(std::move(audit_log)) {}

ActionResult LocalAuditLogProvider::execute(const ActionRequest& request) {
    const auto start = std::chrono::steady_clock::now();

    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = ActionType::AuditEventGeneration;
    res.executed_at = std::chrono::system_clock::now();

    if (!audit_log_) {
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = "AuditLog instance not configured";
        return res;
    }

    auto entry = audit_log_->record_event(
        request.request_id,
        request.alert_id,
        ActionType::AuditEventGeneration,
        "EXPLICIT_AUDIT_RECORD_GENERATED",
        ExecutionState::Completed,
        "Audit log provider recorded authorized audit event",
        request.payload
    );

    const auto end = std::chrono::steady_clock::now();
    res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    res.state = ExecutionState::Completed;
    res.success = true;
    res.audit_id = entry.hash;
    res.output = {
        {"audit_sequence", entry.sequence_num},
        {"audit_hash", entry.hash}
    };
    return res;
}

void LocalAuditLogProvider::cancel(const std::string& /*request_id*/) {}

// -----------------------------------------------------------------------------
// 3. SimulatedVoiceAnnouncementProvider
// -----------------------------------------------------------------------------
SimulatedVoiceAnnouncementProvider::SimulatedVoiceAnnouncementProvider() = default;

bool SimulatedVoiceAnnouncementProvider::is_online() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_online_;
}

void SimulatedVoiceAnnouncementProvider::set_online(bool online) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_online_ = online;
}

ActionResult SimulatedVoiceAnnouncementProvider::execute(const ActionRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto start = std::chrono::steady_clock::now();

    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = ActionType::SimulatedVoiceAnnouncement;
    res.executed_at = std::chrono::system_clock::now();

    if (!is_online_) {
        res.state = ExecutionState::RefusedOffline;
        res.success = false;
        res.failure_reason = "Simulated speaker device is offline";
        return res;
    }

    SpokenAnnouncement ann;
    ann.request_id = request.request_id;
    ann.speaker_target = request.target.empty() ? "local_default_speaker" : request.target;
    ann.spoken_text = request.payload.value("spoken_text", request.payload.value("message", "Attention: safety alert"));
    ann.timestamp = res.executed_at;

    announcements_.push_back(ann);

    const auto end = std::chrono::steady_clock::now();
    res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    res.state = ExecutionState::Completed;
    res.success = true;
    res.output = {
        {"status", "simulated_announcement_synthesized"},
        {"speaker_target", ann.speaker_target},
        {"spoken_text", ann.spoken_text}
    };
    return res;
}

void SimulatedVoiceAnnouncementProvider::cancel(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = announcements_.begin(); it != announcements_.end(); ++it) {
        if (it->request_id == request_id) {
            announcements_.erase(it);
            break;
        }
    }
}

std::vector<SimulatedVoiceAnnouncementProvider::SpokenAnnouncement>
SimulatedVoiceAnnouncementProvider::get_announcements() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return announcements_;
}

void SimulatedVoiceAnnouncementProvider::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    announcements_.clear();
}

// -----------------------------------------------------------------------------
// 4. AutomationHookProvider
// -----------------------------------------------------------------------------
AutomationHookProvider::AutomationHookProvider() = default;

bool AutomationHookProvider::is_online() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_online_;
}

void AutomationHookProvider::set_online(bool online) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_online_ = online;
}

void AutomationHookProvider::register_hook(const std::string& hook_name, HookCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_[hook_name] = std::move(callback);
}

void AutomationHookProvider::unregister_hook(const std::string& hook_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_.erase(hook_name);
}

ActionResult AutomationHookProvider::execute(const ActionRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto start = std::chrono::steady_clock::now();

    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = ActionType::AutomationHook;
    res.executed_at = std::chrono::system_clock::now();

    if (!is_online_) {
        res.state = ExecutionState::RefusedOffline;
        res.success = false;
        res.failure_reason = "Automation hook executor is offline";
        return res;
    }

    std::string hook_name = request.target;
    if (hook_name.empty() || hooks_.find(hook_name) == hooks_.end()) {
        if (request.payload.contains("hook_name")) {
            hook_name = request.payload.value("hook_name", "");
        }
    }

    auto it = hooks_.find(hook_name);
    if (it == hooks_.end()) {
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = "Automation hook '" + hook_name + "' not registered";
        return res;
    }

    try {
        nlohmann::json out = it->second(request);
        const auto end = std::chrono::steady_clock::now();
        res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        res.state = ExecutionState::Completed;
        res.success = true;
        res.output = out;
    } catch (const std::exception& ex) {
        const auto end = std::chrono::steady_clock::now();
        res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = std::string("Hook execution failed: ") + ex.what();
    }

    return res;
}

void AutomationHookProvider::cancel(const std::string& /*request_id*/) {}

// -----------------------------------------------------------------------------
// 5. MockActionProvider
// -----------------------------------------------------------------------------
MockActionProvider::MockActionProvider(ActionType type, const std::string& name)
    : type_(type), name_(name) {}

bool MockActionProvider::is_online() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_online_;
}

void MockActionProvider::set_online(bool online) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_online_ = online;
}

void MockActionProvider::set_should_fail(bool fail, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    should_fail_ = fail;
    failure_reason_ = reason;
}

void MockActionProvider::fail_first_n_times(uint32_t count, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_countdown_ = count;
    transient_reason_ = reason;
}

void MockActionProvider::set_simulated_delay_ms(uint32_t delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    delay_ms_ = delay_ms;
}

ActionResult MockActionProvider::execute(const ActionRequest& request) {
    std::unique_lock<std::mutex> lock(mutex_);
    call_count_++;
    received_requests_.push_back(request);

    const auto start = std::chrono::steady_clock::now();

    if (delay_ms_ > 0) {
        uint32_t d = delay_ms_;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(d));
        lock.lock();
    }

    ActionResult res;
    res.request_id = request.request_id;
    res.idempotency_key = request.idempotency_key;
    res.action_type = type_;
    res.executed_at = std::chrono::system_clock::now();

    if (!is_online_) {
        res.state = ExecutionState::RefusedOffline;
        res.success = false;
        res.failure_reason = "Mock provider is offline";
        return res;
    }

    if (fail_countdown_ > 0) {
        fail_countdown_--;
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = transient_reason_;
        return res;
    }

    if (should_fail_) {
        res.state = ExecutionState::Failed;
        res.success = false;
        res.failure_reason = failure_reason_;
        return res;
    }

    const auto end = std::chrono::steady_clock::now();
    res.duration_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    res.state = ExecutionState::Completed;
    res.success = true;
    res.output = {{"status", "mock_executed"}, {"call_index", call_count_}};
    return res;
}

void MockActionProvider::cancel(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_requests_.push_back(request_id);
}

size_t MockActionProvider::get_call_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return call_count_;
}

std::vector<ActionRequest> MockActionProvider::get_received_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_requests_;
}

std::vector<std::string> MockActionProvider::get_cancelled_request_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_requests_;
}

void MockActionProvider::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_online_ = true;
    should_fail_ = false;
    failure_reason_.clear();
    fail_countdown_ = 0;
    delay_ms_ = 0;
    call_count_ = 0;
    received_requests_.clear();
    cancelled_requests_.clear();
}

} // namespace homeguardian::action
