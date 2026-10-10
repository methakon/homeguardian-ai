#include "AlertRepository.h"
#include <stdexcept>

namespace homeguardian {

AlertRepository::AlertRepository(std::shared_ptr<SQLiteDatabase> db) : db_(std::move(db)) {
    if (!db_) throw std::invalid_argument("AlertRepository: db is null");
    ensure_table();
}

void AlertRepository::ensure_table() {
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS alerts (
            alert_id TEXT PRIMARY KEY,
            severity TEXT NOT NULL,
            message TEXT NOT NULL,
            contributing_event_ids TEXT NOT NULL,
            detected_at INTEGER NOT NULL,
            confidence REAL NOT NULL,
            rule_name TEXT NOT NULL,
            details TEXT NOT NULL
        );
    )");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_alerts_severity ON alerts(severity);");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_alerts_detected_at ON alerts(detected_at);");
}

void AlertRepository::save(const Alert& alert) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = R"(
        INSERT INTO alerts (
            alert_id, severity, message, contributing_event_ids,
            detected_at, confidence, rule_name, details
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::save prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, alert.alert_id.c_str(), -1, SQLITE_STATIC);

    const char* sev_str = alert.severity == Severity::info ? "info" :
                          alert.severity == Severity::warning ? "warning" : "critical";
    sqlite3_bind_text(stmt, idx++, sev_str, -1, SQLITE_STATIC);

    sqlite3_bind_text(stmt, idx++, alert.message.c_str(), -1, SQLITE_TRANSIENT);

    nlohmann::json contrib_ids = alert.contributing_event_ids;
    std::string contrib_str = contrib_ids.dump();
    sqlite3_bind_text(stmt, idx++, contrib_str.c_str(), -1, SQLITE_TRANSIENT);

    auto det_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        alert.detected_at.time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, det_ms);

    sqlite3_bind_double(stmt, idx++, alert.confidence);
    sqlite3_bind_text(stmt, idx++, alert.rule_name.c_str(), -1, SQLITE_TRANSIENT);

    std::string details_str = alert.details.dump();
    sqlite3_bind_text(stmt, idx++, details_str.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT) {
        throw DuplicateAlertException("Alert with ID '" + alert.alert_id + "' already exists");
    }
    if (rc != SQLITE_DONE) {
        throw DatabaseException("AlertRepository::save failed: " + std::string(sqlite3_errmsg(raw)));
    }
}

std::optional<Alert> AlertRepository::find_by_id(const std::string& alert_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT * FROM alerts WHERE alert_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::find_by_id prepare failed");
    }

    sqlite3_bind_text(stmt, 1, alert_id.c_str(), -1, SQLITE_STATIC);

    std::optional<Alert> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = row_to_alert(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<Alert> AlertRepository::find_recent(size_t limit) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT * FROM alerts ORDER BY detected_at DESC LIMIT ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::find_recent prepare failed");
    }

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));

    std::vector<Alert> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_alert(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<Alert> AlertRepository::find_by_severity(Severity severity) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sev_str = severity == Severity::info ? "info" :
                          severity == Severity::warning ? "warning" : "critical";

    const char* sql = "SELECT * FROM alerts WHERE severity = ? ORDER BY detected_at DESC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::find_by_severity prepare failed");
    }

    sqlite3_bind_text(stmt, 1, sev_str, -1, SQLITE_STATIC);

    std::vector<Alert> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_alert(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

size_t AlertRepository::count() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM alerts;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::count prepare failed");
    }

    size_t cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cnt = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return cnt;
}

size_t AlertRepository::delete_older_than(std::chrono::system_clock::time_point cutoff) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    auto cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        cutoff.time_since_epoch()).count();

    const char* sql = "DELETE FROM alerts WHERE detected_at < ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("AlertRepository::delete_older_than prepare failed");
    }

    sqlite3_bind_int64(stmt, 1, cutoff_ms);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw DatabaseException("AlertRepository::delete_older_than failed");
    }

    return static_cast<size_t>(db_->rows_modified());
}

size_t AlertRepository::delete_all() {
    db_->execute("DELETE FROM alerts;");
    return static_cast<size_t>(db_->rows_modified());
}

Alert AlertRepository::row_to_alert(sqlite3_stmt* stmt) {
    std::string alert_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

    std::string sev_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    Severity severity = sev_str == "info" ? Severity::info :
                        sev_str == "warning" ? Severity::warning : Severity::critical;

    std::string message = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

    std::string contrib_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    nlohmann::json contrib_json = nlohmann::json::parse(contrib_str);
    std::vector<std::string> contributing_event_ids = contrib_json.get<std::vector<std::string>>();

    auto det_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 4));
    double confidence = sqlite3_column_double(stmt, 5);
    std::string rule_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

    std::string details_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    nlohmann::json details = nlohmann::json::parse(details_str);

    return Alert(
        alert_id, severity, message, contributing_event_ids,
        std::chrono::system_clock::time_point(det_ms),
        confidence, rule_name, details
    );
}

} // namespace homeguardian
