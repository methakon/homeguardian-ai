#pragma once

#include "IDatabase.h"
#include "core/FamilyProfile.h"
#include <vector>
#include <optional>

namespace homeguardian {

class DuplicateProfileException : public std::runtime_error {
public:
    explicit DuplicateProfileException(const std::string& msg) : std::runtime_error(msg) {}
};

class IProfileRepository {
public:
    virtual ~IProfileRepository() = default;

    virtual void save(const FamilyProfile& profile) = 0;
    virtual std::optional<FamilyProfile> find_by_id(const std::string& profile_id) = 0;
    virtual std::vector<FamilyProfile> find_all() = 0;
    virtual size_t count() = 0;
    // Deletes the profile. Dependent routines and consent records are removed
    // by the database via ON DELETE CASCADE. Returns rows affected by the
    // profile delete itself (0 or 1).
    virtual size_t remove(const std::string& profile_id) = 0;
    virtual size_t delete_all() = 0;
};

} // namespace homeguardian
