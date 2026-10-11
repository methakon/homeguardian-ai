#pragma once
#include "Event.h"
#include "Alert.h"
#include "CorrelationRule.h"
#include <vector>
#include <memory>
#include <optional>
#include <chrono>

namespace homeguardian {

class Pipeline {
public:
    Pipeline(std::vector<std::shared_ptr<ICorrelationRule>> rules,
             std::optional<std::chrono::milliseconds> correlation_window = std::chrono::milliseconds(5000));

    std::optional<Alert> process(const Event& event);
    std::vector<Event> get_recent_events(std::chrono::milliseconds window) const;
    void set_max_history_size(size_t max_size);
    void clear_history();

    // Names of the registered correlation rules, in registration order. Used by
    // lifecycle/notification code (e.g. re-arming AlertPolicy per rule) so it
    // never hardcodes a specific rule name.
    std::vector<std::string> rule_names() const;

private:
    std::vector<std::shared_ptr<ICorrelationRule>> rules_;
    std::optional<std::chrono::milliseconds> correlation_window_;
    std::vector<Event> history_;
    size_t max_history_size_ = 1000;
};

} // namespace homeguardian
