#include "catch2/catch.hpp"
#include "persistence/SQLiteDatabase.h"
#include "persistence/EventRepository.h"
#include "persistence/AlertRepository.h"
#include "persistence/SchemaManager.h"
#include "persistence/PersistenceManager.h"
#include "core/Event.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include <filesystem>
#include <fstream>

using namespace homeguardian;
using json = nlohmann::json;

static const std::string TEST_DB = "test_persistence.db";

struct PersistenceFixture {
    PersistenceFixture() {
        Logger::initialize("debug");
        std::filesystem::remove(TEST_DB);
        std::filesystem::remove(TEST_DB + "-wal");
        std::filesystem::remove(TEST_DB + "-shm");
    }
    ~PersistenceFixture() {
        std::filesystem::remove(TEST_DB);
        std::filesystem::remove(TEST_DB + "-wal");
        std::filesystem::remove(TEST_DB + "-shm");
    }
};

TEST_CASE("SQLiteDatabase open and close", "[persistence][database]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    REQUIRE_FALSE(db->is_open());
    db->open(TEST_DB);
    REQUIRE(db->is_open());
    db->close();
    REQUIRE_FALSE(db->is_open());
}

TEST_CASE("SQLiteDatabase double open throws", "[persistence][database]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);
    REQUIRE_THROWS_AS(db->open(TEST_DB), DatabaseException);
    db->close();
}

TEST_CASE("SQLiteDatabase transactions", "[persistence][database]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    db->execute("CREATE TABLE test (id INTEGER PRIMARY KEY, val TEXT);");
    db->begin_transaction();
    db->execute("INSERT INTO test (val) VALUES ('hello');");
    db->commit();

    db->begin_transaction();
    db->execute("INSERT INTO test (val) VALUES ('world');");
    db->rollback();

    db->close();
}

TEST_CASE("SQLiteDatabase user_version", "[persistence][database]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    REQUIRE(db->get_user_version() == 0);
    db->set_user_version(42);
    REQUIRE(db->get_user_version() == 42);

    db->close();
}

TEST_CASE("EventRepository save and find", "[persistence][event]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    Event e = Event::create_observation("evt-1", "sensor.motion", {{"x", 1}});
    repo.save(e);

    auto found = repo.find_by_id("evt-1");
    REQUIRE(found.has_value());
    REQUIRE(found->get_event_id() == "evt-1");
    REQUIRE(found->get_source() == "sensor.motion");
    REQUIRE(found->get_type() == EventType::observation);

    db->close();
}

TEST_CASE("EventRepository duplicate ID throws", "[persistence][event]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    Event e1 = Event::create_observation("dup-id", "src", json::object());
    repo.save(e1);

    Event e2 = Event::create_observation("dup-id", "other", json::object());
    REQUIRE_THROWS_AS(repo.save(e2), DuplicateEventException);

    db->close();
}

TEST_CASE("EventRepository find_recent", "[persistence][event]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    for (int i = 0; i < 5; i++) {
        repo.save(Event::create_observation("evt-" + std::to_string(i), "src", json::object()));
    }

    auto recent = repo.find_recent(3);
    REQUIRE(recent.size() == 3);

    db->close();
}

TEST_CASE("EventRepository find_by_correlation_id", "[persistence][event]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    auto now = std::chrono::system_clock::now();
    Event e1("corr-1", 1, EventType::observation, "src", now, now,
             std::nullopt, std::nullopt, std::nullopt, json::object(),
             std::nullopt, "group-a", std::nullopt);
    Event e2("corr-2", 1, EventType::observation, "src", now, now,
             std::nullopt, std::nullopt, std::nullopt, json::object(),
             std::nullopt, "group-a", std::nullopt);
    Event e3("corr-3", 1, EventType::observation, "src", now, now,
             std::nullopt, std::nullopt, std::nullopt, json::object(),
             std::nullopt, "group-b", std::nullopt);

    repo.save(e1);
    repo.save(e2);
    repo.save(e3);

    auto group_a = repo.find_by_correlation_id("group-a");
    REQUIRE(group_a.size() == 2);

    auto group_b = repo.find_by_correlation_id("group-b");
    REQUIRE(group_b.size() == 1);

    db->close();
}

TEST_CASE("EventRepository delete_older_than", "[persistence][event]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    auto now = std::chrono::system_clock::now();
    auto old = now - std::chrono::hours(48);

    Event e1("old-1", 1, EventType::observation, "src", old, old,
             std::nullopt, std::nullopt, std::nullopt, json::object(),
             std::nullopt, std::nullopt, std::nullopt);
    Event e2("new-1", 1, EventType::observation, "src", now, now,
             std::nullopt, std::nullopt, std::nullopt, json::object(),
             std::nullopt, std::nullopt, std::nullopt);

    repo.save(e1);
    repo.save(e2);
    REQUIRE(repo.count() == 2);

    auto cutoff = now - std::chrono::hours(24);
    size_t deleted = repo.delete_older_than(cutoff);
    REQUIRE(deleted == 1);
    REQUIRE(repo.count() == 1);

    db->close();
}

TEST_CASE("AlertRepository save and find", "[persistence][alert]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    AlertRepository repo(db);

    Alert a = Alert::create("alert-1", Severity::critical, "Test alert",
                            {"evt-1", "evt-2"}, 0.95, "test_rule", json::object());
    repo.save(a);

    auto found = repo.find_by_id("alert-1");
    REQUIRE(found.has_value());
    REQUIRE(found->alert_id == "alert-1");
    REQUIRE(found->severity == Severity::critical);
    REQUIRE(found->confidence == 0.95);
    REQUIRE(found->contributing_event_ids.size() == 2);

    db->close();
}

TEST_CASE("AlertRepository duplicate ID throws", "[persistence][alert]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    AlertRepository repo(db);

    Alert a1 = Alert::create("dup-alert", Severity::info, "msg", {}, 0.5, "rule", json::object());
    repo.save(a1);

    Alert a2 = Alert::create("dup-alert", Severity::warning, "other", {}, 0.7, "rule2", json::object());
    REQUIRE_THROWS_AS(repo.save(a2), DuplicateAlertException);

    db->close();
}

TEST_CASE("AlertRepository find_by_severity", "[persistence][alert]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    AlertRepository repo(db);

    repo.save(Alert::create("a1", Severity::info, "info msg", {}, 0.5, "r", json::object()));
    repo.save(Alert::create("a2", Severity::warning, "warn msg", {}, 0.6, "r", json::object()));
    repo.save(Alert::create("a3", Severity::critical, "crit msg", {}, 0.9, "r", json::object()));
    repo.save(Alert::create("a4", Severity::critical, "crit2 msg", {}, 0.8, "r", json::object()));

    auto critical = repo.find_by_severity(Severity::critical);
    REQUIRE(critical.size() == 2);

    auto info = repo.find_by_severity(Severity::info);
    REQUIRE(info.size() == 1);

    db->close();
}

TEST_CASE("SchemaManager initialize creates tables", "[persistence][schema]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    REQUIRE(schema.current_version() == SchemaManager::LATEST_VERSION);

    EventRepository event_repo(db);
    AlertRepository alert_repo(db);

    event_repo.save(Event::create_observation("schema-test", "src", json::object()));
    alert_repo.save(Alert::create("schema-alert", Severity::info, "msg", {}, 0.5, "r", json::object()));

    REQUIRE(event_repo.count() == 1);
    REQUIRE(alert_repo.count() == 1);

    db->close();
}

TEST_CASE("SchemaManager idempotent initialize", "[persistence][schema]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();
    schema.initialize();

    REQUIRE(schema.current_version() == SchemaManager::LATEST_VERSION);

    db->close();
}

TEST_CASE("PersistenceManager restart recovery", "[persistence][manager]") {
    PersistenceFixture f;

    {
        PersistenceManager pm(TEST_DB);
        pm.open();
        pm.save_event(Event::create_observation("restart-1", "src", {{"k", "v"}}));
        pm.save_alert(Alert::create("restart-alert", Severity::warning, "msg", {"restart-1"}, 0.7, "rule", json::object()));
        pm.close();
    }

    {
        PersistenceManager pm(TEST_DB);
        pm.open();

        REQUIRE(pm.event_count() == 1);
        REQUIRE(pm.alert_count() == 1);

        auto evt = pm.find_event("restart-1");
        REQUIRE(evt.has_value());
        REQUIRE(evt->get_event_id() == "restart-1");

        auto alert = pm.find_alert("restart-alert");
        REQUIRE(alert.has_value());
        REQUIRE(alert->severity == Severity::warning);

        pm.close();
    }
}

TEST_CASE("PersistenceManager retention enforcement", "[persistence][manager]") {
    PersistenceFixture f;

    PersistenceManager pm(TEST_DB, std::chrono::hours(1), std::chrono::hours(1));
    pm.open();

    auto now = std::chrono::system_clock::now();

    // One event observed 2 hours ago (older than the 1 hour retention window).
    Event old_evt("old-evt", 1, EventType::observation, "src", now - std::chrono::hours(2), now,
                  std::nullopt, std::nullopt, std::nullopt, json::object(),
                  std::nullopt, std::nullopt, std::nullopt);
    pm.event_repo().save(old_evt);
    pm.event_repo().save(Event::create_observation("new-evt", "src", json::object()));

    REQUIRE(pm.event_count() == 2);

    size_t deleted = pm.enforce_retention();
    REQUIRE(deleted == 1);
    REQUIRE(pm.event_count() == 1);

    pm.close();
}

TEST_CASE("PersistenceManager not open throws", "[persistence][manager]") {
    PersistenceFixture f;
    PersistenceManager pm(TEST_DB);
    REQUIRE_THROWS_AS(pm.save_event(Event::create_observation("x", "y", json::object())), DatabaseException);
    REQUIRE_THROWS_AS(pm.event_count(), DatabaseException);
}

TEST_CASE("PersistenceManager transaction rollback on error", "[persistence][manager]") {
    PersistenceFixture f;
    auto db = std::make_shared<SQLiteDatabase>();
    db->open(TEST_DB);

    SchemaManager schema(db);
    schema.initialize();

    EventRepository repo(db);

    db->begin_transaction();
    repo.save(Event::create_observation("tx-1", "src", json::object()));
    db->rollback();

    REQUIRE(repo.count() == 0);

    db->close();
}
