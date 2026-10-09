#pragma once
#include "EventSource.h"
#include <vector>

namespace homeguardian {

class SimulatedEventSource : public IEventSource {
public:
    explicit SimulatedEventSource(std::vector<Event> events);
    std::optional<Event> next() override;
    std::string name() const override;

private:
    std::vector<Event> events_;
    size_t index_;
};

} // namespace homeguardian
