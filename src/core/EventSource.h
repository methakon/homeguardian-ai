#pragma once
#include "Event.h"
#include <optional>
#include <string>

namespace homeguardian {

class IEventSource {
public:
    virtual ~IEventSource() = default;
    virtual std::optional<Event> next() = 0;
    virtual std::string name() const = 0;
};

} // namespace homeguardian
