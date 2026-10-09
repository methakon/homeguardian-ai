#pragma once
#include "Event.h"
#include "Alert.h"
#include <vector>
#include <optional>
#include <string>

namespace homeguardian {

class ICorrelationRule {
public:
    virtual ~ICorrelationRule() = default;
    virtual std::optional<Alert> evaluate(const std::vector<Event>& window) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
};

} // namespace homeguardian
