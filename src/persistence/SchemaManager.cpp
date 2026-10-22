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
    migrate();
}

void SchemaManager::migrate() {
    current_version_ = db_->get_user_version();
    while (current_version_ < LATEST_VERSION) {
        if (current_version_ == 1) {
            migrate_v1_to_v2();
        } else {
            break;
        }
    }
}

void SchemaManager::migrate_v1_to_v2() {
    // Additive migration: adds profiles, routines, and consent_records tables
    // for Phase E. Existing events and alerts tables are untouched, so all
    // prior data is preserved. The whole migration runs in a single
    // transaction with rollback on any failure, and is safe to rerun because
    // it only runs when user_version == 1.
    db_->begin_transaction();
    try {
        db_->execute(R"(
            CREATE TABLE IF NOT EXISTS home_profiles (
                profile_id TEXT PRIMARY KEY,
                display_name TEXT NOT NULL,
                age_band TEXT,
                enabled INTEGER NOT NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                schema_version INTEGER NOT NULL
            );
        )");

        db_->execute(R"(
            CREATE TABLE IF NOT EXISTS home_routines (
                routine_id TEXT PRIMARY KEY,
                profile_id TEXT NOT NULL,
                label TEXT NOT NULL,
                schedule TEXT,
                time_zone TEXT,
                enabled INTEGER NOT NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL,
                schema_version INTEGER NOT NULL,
                FOREIGN KEY (profile_id) REFERENCES home_profiles(profile_id) ON DELETE CASCADE
            );
        )");
        db_->execute("CREATE INDEX IF NOT EXISTS idx_home_routines_profile ON home_routines(profile_id);");

        db_->execute(R"(
            CREATE TABLE IF NOT EXISTS home_consent_records (
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
        db_->execute("CREATE INDEX IF NOT EXISTS idx_home_consent_subject ON home_consent_records(subject_scope);");
        db_->execute("CREATE INDEX IF NOT EXISTS idx_home_consent_purpose ON home_consent_records(subject_scope, purpose);");

        db_->set_user_version(2);
        db_->commit();
        current_version_ = 2;
    } catch (...) {
        db_->rollback();
        throw;
    }
}

void SchemaManager::create_v1_schema() {
    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS home_events (
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
    db_->execute("CREATE INDEX IF NOT EXISTS idx_home_events_correlation ON home_events(correlation_id);");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_home_events_observation_ts ON home_events(observation_timestamp);");

    db_->execute(R"(
        CREATE TABLE IF NOT EXISTS home_alerts (
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
    db_->execute("CREATE INDEX IF NOT EXISTS idx_home_alerts_severity ON home_alerts(severity);");
    db_->execute("CREATE INDEX IF NOT EXISTS idx_home_alerts_detected_at ON home_alerts(detected_at);");
}

} // namespace homeguardian
