#pragma once

#include "IDatabase.h"
#include "core/Alert.h"
#include <vector>
#include <optional>

namespace homeguardian {

class DuplicateAlertException : public std::runtime_error {
public:
    explicit DuplicateAlertException(const std::string& msg) : std::runtime_error(msg) {}
};

class IAlertRepository {
public:
    virtual ~IAlertRepository() = default;

    virtual void save(const Alert& alert) = 0;
    virtual std::optional<Alert> find_by_id(const std::string& alert_id) = 0;
    virtual std::vector<Alert> find_recent(size_t limit) = 0;
    virtual std::vector<Alert> find_by_severity(Severity severity) = 0;
    virtual size_t count() = 0;
    virtual size_t delete_older_than(std::chrono::system_clock::time_point cutoff) = 0;
    virtual size_t delete_all() = 0;
};

} // namespace homeguardian
