#pragma once

#include "IDatabase.h"
#include "core/Event.h"
#include <vector>
#include <optional>

namespace homeguardian {

class DuplicateEventException : public std::runtime_error {
public:
    explicit DuplicateEventException(const std::string& msg) : std::runtime_error(msg) {}
};

class IEventRepository {
public:
    virtual ~IEventRepository() = default;

    virtual void save(const Event& event) = 0;
    virtual std::optional<Event> find_by_id(const std::string& event_id) = 0;
    virtual std::vector<Event> find_recent(size_t limit) = 0;
    virtual std::vector<Event> find_by_correlation_id(const std::string& correlation_id) = 0;
    virtual size_t count() = 0;
    virtual size_t delete_older_than(std::chrono::system_clock::time_point cutoff) = 0;
    virtual size_t delete_all() = 0;
};

} // namespace homeguardian
