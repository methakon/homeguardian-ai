#pragma once

#include "IDatabase.h"
#include "core/Routine.h"
#include <vector>
#include <optional>

namespace homeguardian {

class DuplicateRoutineException : public std::runtime_error {
public:
    explicit DuplicateRoutineException(const std::string& msg) : std::runtime_error(msg) {}
};

class IRoutineRepository {
public:
    virtual ~IRoutineRepository() = default;

    virtual void save(const Routine& routine) = 0;
    virtual std::optional<Routine> find_by_id(const std::string& routine_id) = 0;
    virtual std::vector<Routine> find_by_profile(const std::string& profile_id) = 0;
    virtual size_t count() = 0;
    virtual size_t delete_all() = 0;
};

} // namespace homeguardian
