#include "RoutineRepository.h"
#include <stdexcept>

namespace homeguardian {

RoutineRepository::RoutineRepository(std::shared_ptr<SQLiteDatabase> db) : db_(std::move(db)) {
    if (!db_) throw std::invalid_argument("RoutineRepository: db is null");
    ensure_table();
}

void RoutineRepository::ensure_table() {
    // Table is created by the SchemaManager migration (v2) with a foreign key
    // to profiles and ON DELETE CASCADE. ensure_table is a defensive fallback
    // so the repository is usable in isolation/tests.
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS routines (
            routine_id TEXT PRIMARY KEY,
            profile_id TEXT NOT NULL,
            label TEXT NOT NULL,
            schedule TEXT,
            time_zone TEXT,
            enabled INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            schema_version INTEGER NOT NULL,
            FOREIGN KEY (profile_id) REFERENCES profiles(profile_id) ON DELETE CASCADE
        );
    )");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_routines_profile ON routines(profile_id);");
}

void RoutineRepository::save(const Routine& routine) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = R"(
        INSERT INTO routines (
            routine_id, profile_id, label, schedule, time_zone,
            enabled, created_at, updated_at, schema_version
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("RoutineRepository::save prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, routine.get_routine_id().c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, idx++, routine.get_profile_id().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, routine.get_label().c_str(), -1, SQLITE_TRANSIENT);

    if (routine.get_schedule().has_value()) {
        sqlite3_bind_text(stmt, idx++, routine.get_schedule().value().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    if (routine.get_time_zone().has_value()) {
        sqlite3_bind_text(stmt, idx++, routine.get_time_zone().value().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    sqlite3_bind_int(stmt, idx++, routine.is_enabled() ? 1 : 0);

    auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        routine.get_created_at().time_since_epoch()).count();
    auto updated_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        routine.get_updated_at().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, created_ms);
    sqlite3_bind_int64(stmt, idx++, updated_ms);
    sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(routine.get_schema_version()));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // With extended result codes enabled, a constraint violation surfaces as an
    // extended SQLITE_CONSTRAINT_* code rather than the base SQLITE_CONSTRAINT.
    if (rc == SQLITE_CONSTRAINT_PRIMARYKEY || rc == SQLITE_CONSTRAINT_UNIQUE) {
        throw DuplicateRoutineException("Routine with ID '" + routine.get_routine_id() + "' already exists");
    }
    if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
        // Foreign-key or other constraint violation (e.g. unknown profile_id).
        throw DatabaseException("RoutineRepository::save constraint failed: " + std::string(sqlite3_errmsg(raw)));
    }
    if (rc != SQLITE_DONE) {
        throw DatabaseException("RoutineRepository::save failed: " + std::string(sqlite3_errmsg(raw)));
    }
}

std::optional<Routine> RoutineRepository::find_by_id(const std::string& routine_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT routine_id, profile_id, label, schedule, time_zone, enabled, created_at, updated_at, schema_version FROM routines WHERE routine_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("RoutineRepository::find_by_id prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, routine_id.c_str(), -1, SQLITE_STATIC);

    std::optional<Routine> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = row_to_routine(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<Routine> RoutineRepository::find_by_profile(const std::string& profile_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT routine_id, profile_id, label, schedule, time_zone, enabled, created_at, updated_at, schema_version FROM routines WHERE profile_id = ? ORDER BY created_at ASC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("RoutineRepository::find_by_profile prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, profile_id.c_str(), -1, SQLITE_STATIC);

    std::vector<Routine> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_routine(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

size_t RoutineRepository::count() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM routines;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("RoutineRepository::count prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    size_t cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cnt = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return cnt;
}

size_t RoutineRepository::delete_all() {
    db_->execute("DELETE FROM routines;");
    return static_cast<size_t>(db_->rows_modified());
}

Routine RoutineRepository::row_to_routine(sqlite3_stmt* stmt) {
    std::string routine_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    std::string profile_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    std::string label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    std::optional<std::string> schedule;
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        schedule = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    }

    std::optional<std::string> time_zone;
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        time_zone = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    }

    bool enabled = sqlite3_column_int(stmt, 5) != 0;

    auto created_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 6));
    auto updated_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 7));
    auto schema_version = static_cast<uint32_t>(sqlite3_column_int64(stmt, 8));

    return Routine(
        routine_id, profile_id, label, schedule, time_zone, enabled,
        std::chrono::system_clock::time_point(created_ms),
        std::chrono::system_clock::time_point(updated_ms),
        schema_version
    );
}

} // namespace homeguardian
