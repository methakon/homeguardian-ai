#pragma once
#include "CorrelationRule.h"
#include <chrono>

namespace homeguardian {

class TimeWindowCorrelation : public ICorrelationRule {
public:
    TimeWindowCorrelation(std::string name, std::vector<EventType> trigger_types,
                          size_t min_count, std::chrono::milliseconds window,
                          Severity alert_severity, std::string alert_message);

    std::optional<Alert> evaluate(const std::vector<Event>& window) override;
    std::string name() const override;
    std::string description() const override;

private:
    std::string name_;
    std::vector<EventType> trigger_types_;
    size_t min_count_;
    std::chrono::milliseconds window_;
    Severity alert_severity_;
    std::string alert_message_;
};

} // namespace homeguardian
