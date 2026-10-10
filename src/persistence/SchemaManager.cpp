#include "SchemaManager.h"

namespace homeguardian {

SchemaManager::SchemaManager(std::shared_ptr<SQLiteDatabase> db)
    : db_(std::move(db)), current_version_(0) {
    if (!db_) throw std::invalid_argument("SchemaManager: db is null");
}

void SchemaManager::initialize() {
    current_version_ = db_->get_user_version();
    if (current_version_ == 0) {
        db_->begin_transaction();
        try {
            create_v1_schema();
            db_->set_user_version(1);
            db_->commit();
            current_version_ = 1;
        } catch (...) {
            db_->rollback();
            throw;
        }
    }
}

void SchemaManager::migrate() {
    current_version_ = db_->get_user_version();
    if (current_version_ < 1) {
        initialize();
    }
}

void SchemaManager::create_v1_schema() {
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

} // namespace homeguardian
