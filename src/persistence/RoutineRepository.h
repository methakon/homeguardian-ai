#pragma once

#include "IRoutineRepository.h"
#include "SQLiteDatabase.h"
#include <sqlite3.h>
#include <memory>

namespace homeguardian {

class RoutineRepository : public IRoutineRepository {
public:
    explicit RoutineRepository(std::shared_ptr<SQLiteDatabase> db);
    ~RoutineRepository() override = default;

    void save(const Routine& routine) override;
    std::optional<Routine> find_by_id(const std::string& routine_id) override;
    std::vector<Routine> find_by_profile(const std::string& profile_id) override;
    size_t count() override;
    size_t delete_all() override;

private:
    std::shared_ptr<SQLiteDatabase> db_;

    Routine row_to_routine(sqlite3_stmt* stmt);
    void ensure_table();
};

} // namespace homeguardian
