#pragma once

#include "SQLiteDatabase.h"
#include "EventRepository.h"
#include "AlertRepository.h"
#include <memory>

namespace homeguardian {

class SchemaManager {
public:
    explicit SchemaManager(std::shared_ptr<SQLiteDatabase> db);

    void initialize();
    void migrate();
    int64_t current_version() const { return current_version_; }

private:
    std::shared_ptr<SQLiteDatabase> db_;
    int64_t current_version_;

    void create_v1_schema();
};

} // namespace homeguardian
