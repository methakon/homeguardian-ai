#include "Pipeline.h"
#include "EventValidator.h"
#include "Logger.h"
#include <algorithm>

namespace homeguardian {

Pipeline::Pipeline(std::vector<std::shared_ptr<ICorrelationRule>> rules,
                   std::optional<std::chrono::milliseconds> correlation_window)
    : rules_(std::move(rules)), correlation_window_(correlation_window) {}

void Pipeline::set_max_history_size(size_t max_size) {
    if (max_size == 0) max_size = 1;
    max_history_size_ = max_size;
    if (history_.size() > max_history_size_) {
        history_.erase(history_.begin(), history_.begin() + (history_.size() - max_history_size_));
    }
}

void Pipeline::clear_history() {
    history_.clear();
}

std::vector<Event> Pipeline::get_recent_events(std::chrono::milliseconds window) const {
    auto now = std::chrono::system_clock::now();
    std::vector<Event> recent;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (now - it->get_observation_timestamp() <= window) {
            recent.push_back(*it);
        }
        // History is bounded arrival order, not timestamp order. A late expired
        // observation must not hide newer evidence inserted before it.
    }
    std::reverse(recent.begin(), recent.end());
    return recent;
}

namespace {

// Severity ordering used for prioritisation (H-01s): info < warning < critical.
int severity_rank(Severity s) {
    switch (s) {
        case Severity::critical: return 2;
        case Severity::warning: return 1;
        case Severity::info: return 0;
    }
    return 0;
}

}  // namespace

std::optional<Alert> Pipeline::process(const Event& event) {
    auto validation_error = EventValidator::validate(event);
    if (validation_error) {
        Logger::get()->warn("Event validation failed: {} (event_id: {})", *validation_error, event.get_event_id());
        return std::nullopt;
    }

    Event normalized = EventValidator::normalize(event);
    history_.push_back(normalized);

    if (history_.size() > max_history_size_) {
        history_.erase(history_.begin());
    }

    auto window_duration = correlation_window_.value_or(std::chrono::milliseconds(5000));
    auto recent_events = get_recent_events(window_duration);

    // Evaluate EVERY registered rule and return the highest-severity alert
    // (info < warning < critical); ties go to the first rule. This is the
    // severity-prioritisation step (H-01s): a critical impact alert is not lost
    // behind an unrelated info/warning alert firing on the same event. With a
    // single rule the behaviour is identical to returning the first alert.
    std::optional<Alert> best;
    for (const auto& rule : rules_) {
        auto alert_opt = rule->evaluate(recent_events);
        if (!alert_opt) continue;
        if (!best || severity_rank(alert_opt->severity) > severity_rank(best->severity)) {
            best = std::move(alert_opt);
        }
    }
    return best;
}

std::vector<std::string> Pipeline::rule_names() const {
    std::vector<std::string> names;
    names.reserve(rules_.size());
    for (const auto& rule : rules_) {
        if (rule) names.push_back(rule->name());
    }
    return names;
}

} // namespace homeguardian
