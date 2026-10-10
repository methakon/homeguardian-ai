#pragma once

#include "IProfileRepository.h"
#include "SQLiteDatabase.h"
#include <sqlite3.h>
#include <memory>

namespace homeguardian {

class ProfileRepository : public IProfileRepository {
public:
    explicit ProfileRepository(std::shared_ptr<SQLiteDatabase> db);
    ~ProfileRepository() override = default;

    void save(const FamilyProfile& profile) override;
    std::optional<FamilyProfile> find_by_id(const std::string& profile_id) override;
    std::vector<FamilyProfile> find_all() override;
    size_t count() override;
    size_t remove(const std::string& profile_id) override;
    size_t delete_all() override;

private:
    std::shared_ptr<SQLiteDatabase> db_;

    FamilyProfile row_to_profile(sqlite3_stmt* stmt);
    void ensure_table();
};

} // namespace homeguardian
