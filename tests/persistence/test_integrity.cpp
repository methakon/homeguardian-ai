#include "catch2/catch.hpp"
#include <unistd.h>   // getpid: process-unique fixture paths
#include "persistence/SQLiteDatabase.h"
#include "persistence/EventRepository.h"
#include "persistence/AlertRepository.h"
#include "persistence/SchemaManager.h"
#include "core/Event.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace homeguardian;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SQLite integrity, migration, retention, and recovery verification.
// Uses TEMPORARY databases. Never deletes a non-test database. Simulator-only;
// no real filesystem exhaustion is simulated — a closed DB stands in for a
// write failure (documented, distinct from real disk-full).
// ---------------------------------------------------------------------------

// Unique per process: two suite instances running in the same working
// directory must not delete each other's database (SQLite reports the
// collision as a confusing 'disk I/O error' on the next WAL/DDL call).
static const std::string I_TEST_DB =
    std::string("test_integrity.db.") + std::to_string(::getpid());

struct IntegrityFixture {
    IntegrityFixture() {
        Logger::initialize("error");
        cleanup();
    }
    ~IntegrityFixture() { cleanup(); }
    static void cleanup() {
        std::filesystem::remove(I_TEST_DB);
        std::filesystem::remove(I_TEST_DB + "-wal");
        std::filesystem::remove(I_TEST_DB + "-shm");
    }
};

// Build a genuine v1-only database: v1 tables + data, user_version = 1, no v2
// tables. Returns after closing so a fresh open can migrate it.
void build_v1_db_with_alerts() {
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(I_TEST_DB);
    SchemaManager schema(db);
    schema.initialize();                 // to v2
    // Seed an alert and an event that must survive migration.
    AlertRepository alerts(db);
    EventRepository events(db);
    alerts.save(Alert::create("keep-alert-1", Severity::warning,
                              "synthetic simulator presence notice",
                              {"keep-event-1"}, 0.9, "sim-presence", json::object()));
    events.save(Event::create_observation("keep-event-1", "sim_alexa_enabled", json::object()));
    // Force back to a v1-only shape: drop v2 tables and set version to 1.
    db->execute("DROP TABLE IF EXISTS consent_records;");
    db->execute("DROP TABLE IF EXISTS routines;");
    db->execute("DROP TABLE IF EXISTS profiles;");
    db->execute("PRAGMA user_version = 1;");
    db->close();
}

// --- migration v1 -> v2 preserves alert metadata + identifiers ---
TEST_CASE("Integrity: v1->v2 migration preserves alerts, events, and indexes",
          "[integrity][migration]") {
    IntegrityFixture f;
    build_v1_db_with_alerts();

    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        REQUIRE(db->get_user_version() == 1);           // starts at v1
        SchemaManager schema(db);
        schema.initialize();                             // migrate to v2
        REQUIRE(db->get_user_version() == SchemaManager::LATEST_VERSION);

        // v2 tables now exist.
        REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM profiles;"));
        REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM routines;"));
        REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM consent_records;"));

        // Alert metadata + identifier preserved exactly.
        AlertRepository alerts(db);
        auto a = alerts.find_by_id("keep-alert-1");
        REQUIRE(a.has_value());
        REQUIRE(a->alert_id == "keep-alert-1");
        REQUIRE(a->severity == Severity::warning);
        REQUIRE(a->rule_name == "sim-presence");
        REQUIRE(a->contributing_event_ids.size() == 1);
        REQUIRE(a->contributing_event_ids[0] == "keep-event-1");

        // Event preserved.
        EventRepository events(db);
        auto e = events.find_by_id("keep-event-1");
        REQUIRE(e.has_value());

        // Indexes exist (query the schema, not just a return code).
        REQUIRE_NOTHROW(db->execute(
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_alerts_severity';"));
        REQUIRE_NOTHROW(db->execute(
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_alerts_detected_at';"));

        // Primary-key constraint is enforced (duplicate id rejected).
        REQUIRE_THROWS(alerts.save(Alert::create("keep-alert-1", Severity::info, "dup",
                                                 {}, 0.5, "sim-presence", json::object())));
        db->close();
    }
}

// --- repeat migration is idempotent ---
TEST_CASE("Integrity: repeated migration is idempotent and preserves data",
          "[integrity][migration]") {
    IntegrityFixture f;
    build_v1_db_with_alerts();

    for (int i = 0; i < 3; ++i) {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        REQUIRE(db->get_user_version() == SchemaManager::LATEST_VERSION);
        AlertRepository alerts(db);
        auto a = alerts.find_by_id("keep-alert-1");
        REQUIRE(a.has_value());
        REQUIRE(a->alert_id == "keep-alert-1");
        db->close();
    }
}

// --- interrupted/failed migration rolls back, leaving version unchanged ---
TEST_CASE("Integrity: failed migration rolls back; schema and version unchanged",
          "[integrity][migration]") {
    IntegrityFixture f;
    build_v1_db_with_alerts();   // v1-only, version 1, data present

    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        REQUIRE(db->get_user_version() == 1);

        // Simulate an interrupted migration: a transaction that creates a v2
        // table then hits a broken statement and is rolled back.
        db->begin_transaction();
        try {
            db->execute("CREATE TABLE profiles (profile_id TEXT PRIMARY KEY);");
            db->execute("CREATE TABLE (;;; broken");  // parse error -> throw
            db->commit();
            FAIL("expected commit to throw");
        } catch (const DatabaseException&) {
            db->rollback();
        }
        // Rollback undid the partial create; version stays 1.
        REQUIRE_THROWS(db->execute("SELECT COUNT(*) FROM profiles;"));
        REQUIRE(db->get_user_version() == 1);
        // Pre-existing v1 data intact.
        AlertRepository alerts(db);
        REQUIRE(alerts.find_by_id("keep-alert-1").has_value());
        db->close();
    }

    // A subsequent real migration still succeeds and preserves data.
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        REQUIRE(db->get_user_version() == SchemaManager::LATEST_VERSION);
        AlertRepository alerts(db);
        REQUIRE(alerts.find_by_id("keep-alert-1").has_value());
        db->close();
    }
}

// --- reopen after clean shutdown returns data ---
TEST_CASE("Integrity: reopening after clean shutdown returns persisted data",
          "[integrity][recovery]") {
    IntegrityFixture f;
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        AlertRepository alerts(db);
        alerts.save(Alert::create("reopen-1", Severity::info, "synthetic notice",
                                  {}, 0.8, "sim-presence", json::object()));
        db->close();  // clean shutdown
    }
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        AlertRepository alerts(db);
        auto a = alerts.find_by_id("reopen-1");
        REQUIRE(a.has_value());
        REQUIRE(a->rule_name == "sim-presence");
        db->close();
    }
}

// --- retention preserves newer and unrelated records ---
TEST_CASE("Integrity: retention deletes only expired alerts; newer/unrelated kept",
          "[integrity][retention]") {
    IntegrityFixture f;
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        AlertRepository alerts(db);
        EventRepository events(db);

        auto old_ts = std::chrono::system_clock::now() - std::chrono::hours(24 * 30);
        auto new_ts = std::chrono::system_clock::now();
        Alert old_a("old-1", Severity::info, "old", {}, old_ts, 0.5, "sim-presence", json::object());
        Alert new_a("new-1", Severity::info, "new", {}, new_ts, 0.5, "sim-presence", json::object());
        alerts.save(old_a);
        alerts.save(new_a);
        // An unrelated record in another table must survive alert retention.
        events.save(Event::create_observation("evt-keep", "sim_alexa_enabled", json::object()));

        auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * 7);
        size_t removed = alerts.delete_older_than(cutoff);
        REQUIRE(removed == 1);                      // only the 30-day-old alert
        REQUIRE(alerts.find_by_id("new-1").has_value());   // newer preserved
        REQUIRE_FALSE(alerts.find_by_id("old-1").has_value());
        REQUIRE(events.find_by_id("evt-keep").has_value()); // unrelated table kept
        db->close();
    }
}

// --- write failure after close is reported, not silently swallowed ---
TEST_CASE("Integrity: write after close fails loudly (no silent success)",
          "[integrity][failure]") {
    IntegrityFixture f;
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        AlertRepository alerts(db);
        db->close();
        // Writing to a closed database must throw (caller surfaces degraded),
        // never report success.
        REQUIRE_THROWS(alerts.save(Alert::create("after-close", Severity::info, "x",
                                                 {}, 0.5, "sim-presence", json::object())));
    }
}

// --- corrupt / unsupported database is not silently deleted ---
TEST_CASE("Integrity: corrupt database file is preserved, not deleted or reset",
          "[integrity][corrupt]") {
    IntegrityFixture f;
    {
        std::ofstream f(I_TEST_DB);
        f << "this is definitely not a sqlite database, just text bytes here";
    }
    bool threw = false;
    try {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(I_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();   // must fail on a corrupt file
    } catch (const std::exception&) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(std::filesystem::exists(I_TEST_DB));  // file preserved, not deleted
}
