#include "EventRepository.h"
#include <stdexcept>
#include <cstring>

namespace homeguardian {

EventRepository::EventRepository(std::shared_ptr<SQLiteDatabase> db) : db_(std::move(db)) {
    if (!db_) throw std::invalid_argument("EventRepository: db is null");
    ensure_table();
}

void EventRepository::ensure_table() {
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS events (
            event_id TEXT PRIMARY KEY,
            schema_version INTEGER NOT NULL,
            type TEXT NOT NULL,
            source TEXT NOT NULL,
            observation_timestamp INTEGER NOT NULL,
            ingestion_timestamp INTEGER NOT NULL,
            monotonic_timestamp INTEGER,
            confidence REAL,
            severity TEXT,
            payload TEXT NOT NULL,
            evidence_ref TEXT,
            correlation_id TEXT,
            processing_metadata TEXT
        );
    )");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_events_correlation ON events(correlation_id);");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_events_observation_ts ON events(observation_timestamp);");
}

void EventRepository::save(const Event& event) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = R"(
        INSERT INTO events (
            event_id, schema_version, type, source,
            observation_timestamp, ingestion_timestamp, monotonic_timestamp,
            confidence, severity, payload, evidence_ref, correlation_id, processing_metadata
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::save prepare failed: " + std::string(sqlite3_errmsg(raw)));
    }

    int idx = 1;
    sqlite3_bind_text(stmt, idx++, event.get_event_id().c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, idx++, static_cast<int64_t>(event.get_schema_version()));

    const char* type_str = event.get_type() == EventType::observation ? "observation" :
                           event.get_type() == EventType::inference ? "inference" : "alert";
    sqlite3_bind_text(stmt, idx++, type_str, -1, SQLITE_STATIC);

    sqlite3_bind_text(stmt, idx++, event.get_source().c_str(), -1, SQLITE_STATIC);

    auto obs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.get_observation_timestamp().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, obs_ms);

    auto ing_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        event.get_ingestion_timestamp().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, idx++, ing_ms);

    if (event.get_monotonic_timestamp().has_value()) {
        auto mono_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            event.get_monotonic_timestamp().value().time_since_epoch()).count();
        sqlite3_bind_int64(stmt, idx++, mono_ms);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    if (event.get_confidence().has_value()) {
        sqlite3_bind_double(stmt, idx++, event.get_confidence().value());
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    if (event.get_severity().has_value()) {
        const char* sev_str = event.get_severity().value() == Severity::info ? "info" :
                              event.get_severity().value() == Severity::warning ? "warning" : "critical";
        sqlite3_bind_text(stmt, idx++, sev_str, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    std::string payload_str = event.get_payload().dump();
    sqlite3_bind_text(stmt, idx++, payload_str.c_str(), -1, SQLITE_TRANSIENT);

    if (event.get_evidence_ref().has_value()) {
        sqlite3_bind_text(stmt, idx++, event.get_evidence_ref().value().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    if (event.get_correlation_id().has_value()) {
        sqlite3_bind_text(stmt, idx++, event.get_correlation_id().value().c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    if (event.get_processing_metadata().has_value()) {
        std::string meta_str = event.get_processing_metadata().value().dump();
        sqlite3_bind_text(stmt, idx++, meta_str.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, idx++);
    }

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT) {
        throw DuplicateEventException("Event with ID '" + event.get_event_id() + "' already exists");
    }
    if (rc != SQLITE_DONE) {
        throw DatabaseException("EventRepository::save failed: " + std::string(sqlite3_errmsg(raw)));
    }
}

std::optional<Event> EventRepository::find_by_id(const std::string& event_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT * FROM events WHERE event_id = ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::find_by_id prepare failed");
    }

    sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_STATIC);

    std::optional<Event> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = row_to_event(stmt);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<Event> EventRepository::find_recent(size_t limit) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT * FROM events ORDER BY observation_timestamp DESC LIMIT ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::find_recent prepare failed");
    }

    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));

    std::vector<Event> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_event(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<Event> EventRepository::find_by_correlation_id(const std::string& correlation_id) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT * FROM events WHERE correlation_id = ? ORDER BY observation_timestamp ASC;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::find_by_correlation_id prepare failed");
    }

    sqlite3_bind_text(stmt, 1, correlation_id.c_str(), -1, SQLITE_STATIC);

    std::vector<Event> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(row_to_event(stmt));
    }

    sqlite3_finalize(stmt);
    return results;
}

size_t EventRepository::count() {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM events;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::count prepare failed");
    }

    size_t cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cnt = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return cnt;
}

size_t EventRepository::delete_older_than(std::chrono::system_clock::time_point cutoff) {
    sqlite3* raw = db_->raw();
    sqlite3_stmt* stmt = nullptr;

    auto cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        cutoff.time_since_epoch()).count();

    const char* sql = "DELETE FROM events WHERE observation_timestamp < ?;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw DatabaseException("EventRepository::delete_older_than prepare failed");
    }

    sqlite3_bind_int64(stmt, 1, cutoff_ms);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw DatabaseException("EventRepository::delete_older_than failed");
    }

    return static_cast<size_t>(db_->rows_modified());
}

size_t EventRepository::delete_all() {
    db_->execute("DELETE FROM events;");
    return static_cast<size_t>(db_->rows_modified());
}

Event EventRepository::row_to_event(sqlite3_stmt* stmt) {
    std::string event_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    uint32_t schema_version = static_cast<uint32_t>(sqlite3_column_int64(stmt, 1));

    std::string type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    EventType type = type_str == "observation" ? EventType::observation :
                     type_str == "inference" ? EventType::inference : EventType::alert;

    std::string source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

    auto obs_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 4));
    auto ing_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 5));

    std::optional<std::chrono::steady_clock::time_point> mono_ts;
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
        auto mono_ms = std::chrono::milliseconds(sqlite3_column_int64(stmt, 6));
        mono_ts = std::chrono::steady_clock::time_point(mono_ms);
    }

    std::optional<double> confidence;
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        confidence = sqlite3_column_double(stmt, 7);
    }

    std::optional<Severity> severity;
    if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
        std::string sev_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        severity = sev_str == "info" ? Severity::info :
                   sev_str == "warning" ? Severity::warning : Severity::critical;
    }

    std::string payload_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    nlohmann::json payload = nlohmann::json::parse(payload_str);

    std::optional<std::string> evidence_ref;
    if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
        evidence_ref = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    }

    std::optional<std::string> correlation_id;
    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
        correlation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    }

    std::optional<nlohmann::json> processing_metadata;
    if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
        std::string meta_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        processing_metadata = nlohmann::json::parse(meta_str);
    }

    return Event(
        event_id, schema_version, type, source,
        std::chrono::system_clock::time_point(obs_ms),
        std::chrono::system_clock::time_point(ing_ms),
        mono_ts, confidence, severity, payload,
        evidence_ref, correlation_id, processing_metadata
    );
}

} // namespace homeguardian
