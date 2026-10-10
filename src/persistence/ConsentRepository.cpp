#include "ConsentRepository.h"
#include <stdexcept>
#include <sstream>

namespace homeguardian {

ConsentRepository::ConsentRepository(std::shared_ptr<SQLiteDatabase> db) : db_(std::move(db)) {
    if (!db_) throw std::invalid_argument("ConsentRepository: db is null");
    ensure_table();
}

void ConsentRepository::ensure_table() {
    // Table is created by the SchemaManager migration (v2). ensure_table is a
    // defensive fallback so the repository is usable in isolation/tests.
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS consent_records (
            consent_id TEXT PRIMARY KEY,
            subject_scope TEXT NOT NULL,
            purpose TEXT NOT NULL,
            data_categories TEXT NOT NULL,
            decision TEXT NOT NULL,
            policy_version TEXT NOT NULL,
            recorded_at INTEGER NOT NULL,
            expires_at INTEGER,
            provenance TEXT NOT NULL,
            schema_version INTEGER NOT NULL
        );
    )");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_consent_subject ON consent_records(subject_scope);");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_consent_purpose ON consent_records(subject_scope, purpose);");
}

void ConsentRepository::save(const ConsentRecord& record) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = R"(
        INSERT INTO consent_records (
            consent_id, subject_scope, purpose, data_categories, decision,
            policy_version, recorded_at, expires_at, provenance, schema_version
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ConsentRepository::save prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    // Serialize data_categories as a JSON array string.
    std::ostringstream cats;
    cats << "[";
    const auto& categories = record.get_data_categories();
    for (size_t i = 0; i < categories.size(); ++i) {
        if (i > 0) cats << ",";
        cats << "\"" << categories[i] << "\"";
    }
    cats << "]";
    std::string cats_str = cats.str();

    const char* decision_str = record.get_decision() == ConsentDecision::granted ? "granted" :
                               record.get_decision() == ConsentDecision::denied ? "denied" : "withdrawn";

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, record.get_consent_id().c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, idx++, record.get_subject_scope().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, record.get_purpose().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, cats_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, idx++, decision_str, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, idx++, record.get_policy_version().c_str(), -1, SQLITE_TRANSIENT);

    auto recorded_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        record.get_recorded_at().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, recorded_ms);

    if (record.get_expires_at().has_value()) {
        auto expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            record.get_expires_at().value().time_since_epoch()).count();
        sqlite3_bind_int64(stmt, idx++, expires_ms);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    sqlite3_bind_text(stmt, idx++, record.get_provenance().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(record.get_schema_version()));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // With extended result codes enabled, a constraint violation surfaces as an
    // extended SQLITE_CONSTRAINT_* code rather than the base SQLITE_CONSTRAINT.
    if (rc == SQLITE_CONSTRAINT_PRIMARYKEY || rc == SQLITE_CONSTRAINT_UNIQUE) {
        throw DuplicateConsentException("Consent with ID '" + record.get_consent_id() + "' already exists");
    }
    if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
        throw DatabaseException("ConsentRepository::save constraint failed: " + std::string(sqlite3_errmsg(raw)));
    }
    if (rc != SQLITE_DONE) {
        throw DatabaseException("ConsentRepository::save failed: " + std::string(sqlite3_errmsg(raw)));
    }
}

std::optional<ConsentRecord> ConsentRepository::find_by_id(const std::string& consent_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT consent_id, subject_scope, purpose, data_categories, decision, policy_version, recorded_at, expires_at, provenance, schema_version FROM consent_records WHERE consent_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ConsentRepository::find_by_id prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, consent_id.c_str(), -1, SQLITE_STATIC);

    std::optional<ConsentRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = row_to_consent(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<ConsentRecord> ConsentRepository::find_by_subject(const std::string& subject_scope) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT consent_id, subject_scope, purpose, data_categories, decision, policy_version, recorded_at, expires_at, provenance, schema_version FROM consent_records WHERE subject_scope = ? ORDER BY recorded_at ASC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ConsentRepository::find_by_subject prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, subject_scope.c_str(), -1, SQLITE_STATIC);

    std::vector<ConsentRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_consent(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<ConsentRecord> ConsentRepository::find_by_purpose(const std::string& subject_scope, const std::string& purpose) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT consent_id, subject_scope, purpose, data_categories, decision, policy_version, recorded_at, expires_at, provenance, schema_version FROM consent_records WHERE subject_scope = ? AND purpose = ? ORDER BY recorded_at ASC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ConsentRepository::find_by_purpose prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    sqlite3_bind_text(stmt, 1, subject_scope.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, purpose.c_str(), -1, SQLITE_STATIC);

    std::vector<ConsentRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_consent(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

size_t ConsentRepository::count() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM consent_records;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("ConsentRepository::count prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    size_t cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cnt = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return cnt;
}

size_t ConsentRepository::delete_all() {
    db_->execute("DELETE FROM consent_records;");
    return static_cast<size_t>(db_->rows_modified());
}

ConsentRecord ConsentRepository::row_to_consent(sqlite3_stmt* stmt) {
    std::string consent_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    std::string subject_scope = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    std::string purpose = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    std::string cats_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    std::vector<std::string> data_categories;
    if (!cats_str.empty() && cats_str != "[]" && cats_str != "null") {
        auto cats_json = nlohmann::json::parse(cats_str);
        data_categories = cats_json.get<std::vector<std::string>>();
    }

    std::string decision_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    ConsentDecision decision = decision_str == "granted" ? ConsentDecision::granted :
                               decision_str == "denied" ? ConsentDecision::denied : ConsentDecision::withdrawn;

    std::string policy_version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    auto recorded_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 6));

    std::optional<std::chrono::system_clock::time_point> expires_at;
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        expires_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(sqlite3_column_int64(stmt, 7)));
    }

    std::string provenance = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    auto schema_version = static_cast<uint32_t>(sqlite3_column_int64(stmt, 9));

    return ConsentRecord(
        consent_id, subject_scope, purpose, data_categories, decision, policy_version,
        std::chrono::system_clock::time_point(recorded_ms), expires_at, provenance, schema_version
    );
}

} // namespace homeguardian
