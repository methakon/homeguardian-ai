#include "SimulatedEventSource.h"

namespace homeguardian {

SimulatedEventSource::SimulatedEventSource(std::vector<Event> events)
    : events_(std::move(events)), index_(0) {}

std::optional<Event> SimulatedEventSource::next() {
    if (index_ < events_.size()) {
        return events_[index_++];
    }
    return std::nullopt;
}

std::string SimulatedEventSource::name() const {
    return "simulated";
}

} // namespace homeguardian
