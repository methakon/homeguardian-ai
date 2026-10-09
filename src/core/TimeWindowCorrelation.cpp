#include "TimeWindowCorrelation.h"
#include "UUID.h"
#include <algorithm>

namespace homeguardian {

TimeWindowCorrelation::TimeWindowCorrelation(std::string name, std::vector<EventType> trigger_types,
                                             size_t min_count, std::chrono::milliseconds window,
                                             Severity alert_severity, std::string alert_message)
    : name_(std::move(name)), trigger_types_(std::move(trigger_types)),
      min_count_(min_count), window_(window), alert_severity_(alert_severity),
      alert_message_(std::move(alert_message)) {}

std::string TimeWindowCorrelation::name() const {
    return name_;
}

std::string TimeWindowCorrelation::description() const {
    return "Time window correlation rule: " + name_;
}

std::optional<Alert> TimeWindowCorrelation::evaluate(const std::vector<Event>& window) {
    if (window.size() < min_count_) return std::nullopt;

    std::vector<Event> sorted_window;
    for (const auto& ev : window) {
        if (std::find(trigger_types_.begin(), trigger_types_.end(), ev.get_type()) != trigger_types_.end()) {
            sorted_window.push_back(ev);
        }
    }
    
    if (sorted_window.size() < min_count_) return std::nullopt;

    std::sort(sorted_window.begin(), sorted_window.end(), [](const Event& a, const Event& b) {
        return a.get_observation_timestamp() < b.get_observation_timestamp();
    });

    for (size_t i = 0; i <= sorted_window.size() - min_count_; ++i) {
        auto start_time = sorted_window[i].get_observation_timestamp();
        size_t count = 1;
        double sum_conf = sorted_window[i].get_confidence().value_or(1.0);
        std::vector<std::string> contributing_ids = {sorted_window[i].get_event_id()};

        for (size_t j = i + 1; j < sorted_window.size(); ++j) {
            auto current_time = sorted_window[j].get_observation_timestamp();
            if (current_time - start_time <= window_) {
                count++;
                sum_conf += sorted_window[j].get_confidence().value_or(1.0);
                contributing_ids.push_back(sorted_window[j].get_event_id());
            } else {
                break;
            }
        }

        if (count >= min_count_) {
            double avg_conf = sum_conf / count;
            nlohmann::json payload = {
                {"rule_name", name_},
                {"contributing_events", contributing_ids},
                {"description", description()}
            };
            
            nlohmann::json meta = {
                {"window_start", std::chrono::duration_cast<std::chrono::milliseconds>(start_time.time_since_epoch()).count()},
                {"window_end", std::chrono::duration_cast<std::chrono::milliseconds>((start_time + window_).time_since_epoch()).count()},
                {"rule", name_}
            };
            
            return Alert::create(
                "alert-" + UUID::generate(),
                alert_severity_,
                alert_message_,
                contributing_ids,
                avg_conf,
                name_,
                payload
            );
        }
    }

    return std::nullopt;
}

} // namespace homeguardian
