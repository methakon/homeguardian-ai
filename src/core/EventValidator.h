#pragma once
#include "Event.h"
#include <optional>
#include <string>

namespace homeguardian {

class EventValidator {
public:
    static std::optional<std::string> validate(const Event& event);
    static Event normalize(const Event& event);
};

} // namespace homeguardian
