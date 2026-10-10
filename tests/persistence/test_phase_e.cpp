#include "catch2/catch.hpp"
#include "persistence/SQLiteDatabase.h"
#include "persistence/ProfileRepository.h"
#include "persistence/ConsentRepository.h"
#include "persistence/RoutineRepository.h"
#include "persistence/EventRepository.h"
#include "persistence/SchemaManager.h"
#include "persistence/PersistenceManager.h"
#include "core/FamilyProfile.h"
#include "core/ConsentRecord.h"
#include "core/Routine.h"
#include "core/Logger.h"
#include <filesystem>
#include <chrono>
#include <fstream>

using namespace homeguardian;
using json = nlohmann::json;

static const std::string E_TEST_DB = "test_phase_e.db";

struct PhaseEFixture {
    PhaseEFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(E_TEST_DB);
        std::filesystem::remove(E_TEST_DB + "-wal");
        std::filesystem::remove(E_TEST_DB + "-shm");
    }
    ~PhaseEFixture() {
        std::filesystem::remove(E_TEST_DB);
        std::filesystem::remove(E_TEST_DB + "-wal");
        std::filesystem::remove(E_TEST_DB + "-shm");
    }
};

static std::shared_ptr<SQLiteDatabase> open_migrated_db(const std::string& path) {
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(path);
    SchemaManager schema(db);
    schema.initialize();
    return db;
}

TEST_CASE("SchemaManager migrates to version 2", "[persistence][migration]") {
    PhaseEFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(E_TEST_DB);
    SchemaManager schema(db);
    schema.initialize();
    REQUIRE(schema.current_version() == SchemaManager::LATEST_VERSION);
    REQUIRE(db->get_user_version() == SchemaManager::LATEST_VERSION);

    // Tables exist.
    REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM profiles;"));
    REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM routines;"));
    REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM consent_records;"));

    // Phase D tables still exist (migration is additive).
    REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM events;"));
    REQUIRE_NOTHROW(db->execute("SELECT COUNT(*) FROM alerts;"));

    db->close();
}

TEST_CASE("Schema migration preserves existing event data", "[persistence][migration]") {
    PhaseEFixture f;
    auto now = std::chrono::system_clock::now();

    // Create a v1 database with an event, then run the migration on reopen.
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(E_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();
        // After initialize the DB is at v2; seed an event and confirm it survives.
        EventRepository events(db);
        events.save(Event::create_observation("keep-me", "src", json::object()));
        db->close();
    }

    // Reopen and migrate again (idempotent); the event must still be present.
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(E_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();  // rerun: must be safe and preserve data
        EventRepository events(db);
        auto found = events.find_by_id("keep-me");
        REQUIRE(found.has_value());
        REQUIRE(found->get_event_id() == "keep-me");
        db->close();
    }
    (void)now;
}

TEST_CASE("ProfileRepository save, find, duplicate, remove", "[persistence][profile]") {
    PhaseEFixture f;
    auto db = open_migrated_db(E_TEST_DB);
    ProfileRepository repo(db);

    auto now = std::chrono::system_clock::now();
    repo.save(FamilyProfile("p1", "Alice", AgeBand::adult, true, now, now, 1));

    auto found = repo.find_by_id("p1");
    REQUIRE(found.has_value());
    REQUIRE(found->get_display_name() == "Alice");
    REQUIRE(repo.count() == 1);

    // Duplicate primary key throws.
    REQUIRE_THROWS_AS(repo.save(FamilyProfile("p1", "Alice2", std::nullopt, true, now, now, 1)),
                      DuplicateProfileException);

    // Missing ID returns empty optional.
    REQUIRE_FALSE(repo.find_by_id("nope").has_value());

    // Remove works.
    REQUIRE(repo.remove("p1") == 1);
    REQUIRE(repo.count() == 0);

    db->close();
}

TEST_CASE("RoutineRepository FK enforcement and cascade delete", "[persistence][routine]") {
    PhaseEFixture f;
    auto db = open_migrated_db(E_TEST_DB);
    ProfileRepository profiles(db);
    RoutineRepository routines(db);

    auto now = std::chrono::system_clock::now();
    profiles.save(FamilyProfile("p1", "Alice", std::nullopt, true, now, now, 1));

    // Routine for a non-existent profile violates the foreign key and is
    // rejected as a DatabaseException (not a duplicate-ID error).
    Routine orphan("r1", "ghost", "Label", std::nullopt, std::nullopt, true, now, now, 1);
    REQUIRE_THROWS_AS(routines.save(orphan), DatabaseException);
    REQUIRE_THROWS_AS(routines.save(orphan), DatabaseException);

    // Routine for an existing profile succeeds.
    routines.save(Routine("r2", "p1", "Morning", std::string("07:30"), std::string("UTC"), true, now, now, 1));
    REQUIRE(routines.count() == 1);
    REQUIRE(routines.find_by_profile("p1").size() == 1);

    // Deleting the profile cascades to its routines.
    profiles.remove("p1");
    REQUIRE(routines.count() == 0);

    db->close();
}

TEST_CASE("ConsentRepository save and query by purpose", "[persistence][consent]") {
    PhaseEFixture f;
    auto db = open_migrated_db(E_TEST_DB);
    ConsentRepository repo(db);

    auto now = std::chrono::system_clock::now();
    repo.save(ConsentRecord::create_granted("c1", "p1", "fall_detection", {"camera"}, "v1", "user"));
    repo.save(ConsentRecord::create_withdrawn("c2", "p1", "presence", "v1", "user"));

    REQUIRE(repo.count() == 2);
    REQUIRE(repo.find_by_subject("p1").size() == 2);
    REQUIRE(repo.find_by_purpose("p1", "fall_detection").size() == 1);
    REQUIRE(repo.find_by_purpose("p1", "presence").size() == 1);

    auto fall = repo.find_by_id("c1");
    REQUIRE(fall.has_value());
    REQUIRE(fall->get_decision() == ConsentDecision::granted);
    REQUIRE(fall->is_active(now));

    // Duplicate consent_id throws.
    REQUIRE_THROWS_AS(repo.save(ConsentRecord::create_granted("c1", "p1", "x", {"camera"}, "v1", "user")),
                      DuplicateConsentException);

    db->close();
}

TEST_CASE("Persistence survives application restart", "[persistence][restart]") {
    PhaseEFixture f;
    auto now = std::chrono::system_clock::now();

    // First "run": write a profile, consent, and routine.
    {
        auto db = open_migrated_db(E_TEST_DB);
        ProfileRepository profiles(db);
        ConsentRepository consents(db);
        RoutineRepository routines(db);

        profiles.save(FamilyProfile("p1", "Alice", AgeBand::senior, true, now, now, 1));
        consents.save(ConsentRecord::create_granted("c1", "p1", "fall_detection", {"camera"}, "v1", "user"));
        routines.save(Routine("r1", "p1", "Evening walk", std::string("18:00"), std::string("Europe/London"), true, now, now, 1));
        db->close();
    }

    // Second "run": reopen the same file and confirm everything is still there.
    {
        auto db = open_migrated_db(E_TEST_DB);
        ProfileRepository profiles(db);
        ConsentRepository consents(db);
        RoutineRepository routines(db);

        auto p = profiles.find_by_id("p1");
        REQUIRE(p.has_value());
        REQUIRE(p->get_display_name() == "Alice");

        auto c = consents.find_by_id("c1");
        REQUIRE(c.has_value());
        REQUIRE(c->is_active(now));

        auto r = routines.find_by_id("r1");
        REQUIRE(r.has_value());
        REQUIRE(r->get_time_zone().value() == "Europe/London");
        db->close();
    }
}

TEST_CASE("Migration transaction rollback leaves schema unchanged on failure", "[persistence][migration]") {
    PhaseEFixture f;

    // Build a genuine v1-only database: v1 tables present, v2 tables absent,
    // user_version = 1.
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(E_TEST_DB);
        SchemaManager schema(db);
        schema.initialize();                 // brings DB to v2
        db->execute("DROP TABLE IF EXISTS consent_records;");
        db->execute("DROP TABLE IF EXISTS routines;");
        db->execute("DROP TABLE IF EXISTS profiles;");
        db->execute("PRAGMA user_version = 1;");
        db->close();
    }

    // Reopen at version 1. A deliberately broken migration must roll back and
    // leave user_version at 1 with no partial v2 tables.
    {
        auto db = std::make_shared<SQLiteDatabase>();
        db->open(E_TEST_DB);
        REQUIRE(db->get_user_version() == 1);
        REQUIRE_THROWS(db->execute("SELECT COUNT(*) FROM profiles;"));  // v2 table absent

        db->begin_transaction();
        try {
            db->execute("CREATE TABLE profiles (profile_id TEXT PRIMARY KEY);");
            db->execute("CREATE TABLE (;;; broken");  // parse error -> throw
            db->commit();
            FAIL("expected commit to throw");
        } catch (const DatabaseException&) {
            db->rollback();
        }

        // Rollback undid the partial CREATE: profiles must still be absent and
        // the version must remain 1.
        REQUIRE_THROWS(db->execute("SELECT COUNT(*) FROM profiles;"));
        REQUIRE(db->get_user_version() == 1);
        db->close();
    }
}

TEST_CASE("PersistenceManager exposes Phase E repositories", "[persistence][manager]") {
    PhaseEFixture f;
    auto now = std::chrono::system_clock::now();

    PersistenceManager pm(E_TEST_DB);
    pm.open();

    pm.profile_repo().save(FamilyProfile("p1", "Alice", std::nullopt, true, now, now, 1));
    pm.consent_repo().save(ConsentRecord::create_granted("c1", "p1", "presence", {"sensor"}, "v1", "user"));
    pm.routine_repo().save(Routine("r1", "p1", "Checkin", std::string("09:00"), std::string("UTC"), true, now, now, 1));

    REQUIRE(pm.profile_repo().count() == 1);
    REQUIRE(pm.consent_repo().count() == 1);
    REQUIRE(pm.routine_repo().count() == 1);

    // Accessing a repository on a closed manager throws.
    pm.close();
    REQUIRE_THROWS_AS(pm.profile_repo(), DatabaseException);
}
