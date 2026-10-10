#pragma once

#include "SQLiteDatabase.h"
#include "EventRepository.h"
#include "AlertRepository.h"
#include <memory>

namespace homeguardian {

class SchemaManager {
public:
    explicit SchemaManager(std::shared_ptr<SQLiteDatabase> db);

    // Initializes the schema to the latest version, applying migrations as
    // needed. Safe to call repeatedly (idempotent).
    void initialize();
    void migrate();
    int64_t current_version() const { return current_version_; }

    static constexpr int64_t LATEST_VERSION = 2;

private:
    std::shared_ptr<SQLiteDatabase> db_;
    int64_t current_version_;

    void create_v1_schema();
    void migrate_v1_to_v2();
};

} // namespace homeguardian
