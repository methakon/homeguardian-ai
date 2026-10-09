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
        } else {
            break;
        }
    }
    std::reverse(recent.begin(), recent.end());
    return recent;
}

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

    for (const auto& rule : rules_) {
        auto alert_opt = rule->evaluate(recent_events);
        if (alert_opt) {
            return alert_opt;
        }
    }

    return std::nullopt;
}

} // namespace homeguardian
