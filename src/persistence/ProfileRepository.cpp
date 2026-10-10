#include "ProfileRepository.h"
#include <stdexcept>

namespace homeguardian {

ProfileRepository::ProfileRepository(std::shared_ptr<SQLiteDatabase> db) : db_(std::move(db)) {
    if (!db_) throw std::invalid_argument("ProfileRepository: db is null");
    ensure_table();
}

void ProfileRepository::ensure_table() {
    // Table is created by the SchemaManager migration (v2). ensure_table is a
    // defensive fallback so the repository is usable in isolation/tests.
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS profiles (
            profile_id TEXT PRIMARY KEY,
            display_name TEXT NOT NULL,
            age_band TEXT,
            enabled INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL,
            schema_version INTEGER NOT NULL
        );
    )");
}

void ProfileRepository::save(const FamilyProfile& profile) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = R"(
        INSERT INTO profiles (
            profile_id, display_name, age_band, enabled,
            created_at, updated_at, schema_version
        ) VALUES (?, ?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ProfileRepository::save prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, profile.get_profile_id().c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, idx++, profile.get_display_name().c_str(), -1, SQLITE_TRANSIENT);

    if (profile.get_age_band().has_value()) {
        const char* band = profile.get_age_band().value() == AgeBand::child ? "child" :
                           profile.get_age_band().value() == AgeBand::teen ? "teen" :
                           profile.get_age_band().value() == AgeBand::adult ? "adult" : "senior";
        sqlite3_bind_text(stmt, idx++, band, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    sqlite3_bind_int(stmt, idx++, profile.is_enabled() ? 1 : 0);

    auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        profile.get_created_at().time_since_epoch()).count();
    auto updated_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        profile.get_updated_at().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, created_ms);
    sqlite3_bind_int64(stmt, idx++, updated_ms);
    sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(profile.get_schema_version()));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // With extended result codes enabled, a constraint violation surfaces as an
    // extended SQLITE_CONSTRAINT_* code rather than the base SQLITE_CONSTRAINT.
    if (rc == SQLITE_CONSTRAINT_PRIMARYKEY || rc == SQLITE_CONSTRAINT_UNIQUE) {
        throw DuplicateProfileException("Profile with ID '" + profile.get_profile_id() + "' already exists");
    }
    if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
        throw DatabaseException("ProfileRepository::save constraint failed: " + std::string(sqlite3_errmsg(raw)));
    }
    if (rc != SQLITE_DONE) {
        throw DatabaseException("ProfileRepository::save failed: " + std::string(sqlite3_errmsg(raw)));
    }
}

std::optional<FamilyProfile> ProfileRepository::find_by_id(const std::string& profile_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT profile_id, display_name, age_band, enabled, created_at, updated_at, schema_version FROM profiles WHERE profile_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ProfileRepository::find_by_id prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, profile_id.c_str(), -1, SQLITE_STATIC);

    std::optional<FamilyProfile> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = row_to_profile(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<FamilyProfile> ProfileRepository::find_all() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT profile_id, display_name, age_band, enabled, created_at, updated_at, schema_version FROM profiles ORDER BY created_at ASC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ProfileRepository::find_all prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    std::vector<FamilyProfile> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_profile(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

size_t ProfileRepository::count() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM profiles;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ProfileRepository::count prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    size_t cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cnt = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return cnt;
}

size_t ProfileRepository::remove(const std::string& profile_id) {
    // Rely on ON DELETE CASCADE (declared in the v2 migration with
    // PRAGMA foreign_keys=ON) to remove dependent routines and consent records
    // in the same statement.
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "DELETE FROM profiles WHERE profile_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ProfileRepository::remove prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, profile_id.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw DatabaseException("ProfileRepository::remove failed: " + std::string(sqlite3_errmsg(raw)));
    }

    return static_cast<size_t>(db_->rows_modified());
}

size_t ProfileRepository::delete_all() {
    db_->execute("DELETE FROM profiles;");
    return static_cast<size_t>(db_->rows_modified());
}

FamilyProfile ProfileRepository::row_to_profile(sqlite3_stmt* stmt) {
    std::string profile_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    std::string display_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

    std::optional<AgeBand> age_band;
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
        std::string band = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        age_band = band == "child" ? AgeBand::child :
                   band == "teen" ? AgeBand::teen :
                   band == "adult" ? AgeBand::adult : AgeBand::senior;
    }

    bool enabled = sqlite3_column_int(stmt, 3) != 0;

    auto created_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 4));
    auto updated_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 5));
    auto schema_version = static_cast<uint32_t>(sqlite3_column_int64(stmt, 6));

    return FamilyProfile(
        profile_id, display_name, age_band, enabled,
        std::chrono::system_clock::time_point(created_ms),
        std::chrono::system_clock::time_point(updated_ms),
        schema_version
    );
}

} // namespace homeguardian
