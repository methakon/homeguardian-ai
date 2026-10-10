#include "catch2/catch.hpp"
#include "core/Routine.h"
#include <chrono>

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("Routine construction and validation", "[routine]") {
    SECTION("Minimal valid routine constructs") {
        REQUIRE_NOTHROW(Routine::create("r1", "p1", "Morning walk"));
    }

    SECTION("Empty routine_id throws") {
        REQUIRE_THROWS_AS(Routine("", "p1", "Label", std::nullopt, std::nullopt, true,
                                  std::chrono::system_clock::now(), std::chrono::system_clock::now(), 1),
                          std::invalid_argument);
    }

    SECTION("Empty profile_id throws") {
        REQUIRE_THROWS_AS(Routine("r1", "", "Label", std::nullopt, std::nullopt, true,
                                  std::chrono::system_clock::now(), std::chrono::system_clock::now(), 1),
                          std::invalid_argument);
    }

    SECTION("Empty label throws") {
        REQUIRE_THROWS_AS(Routine("r1", "p1", "", std::nullopt, std::nullopt, true,
                                  std::chrono::system_clock::now(), std::chrono::system_clock::now(), 1),
                          std::invalid_argument);
    }

    SECTION("Over-long label throws") {
        std::string long_label(200, 'x');
        REQUIRE_THROWS_AS(Routine("r1", "p1", long_label, std::nullopt, std::nullopt, true,
                                  std::chrono::system_clock::now(), std::chrono::system_clock::now(), 1),
                          std::invalid_argument);
    }

    SECTION("Schema version < 1 throws") {
        REQUIRE_THROWS_AS(Routine("r1", "p1", "Label", std::nullopt, std::nullopt, true,
                                  std::chrono::system_clock::now(), std::chrono::system_clock::now(), 0),
                          std::invalid_argument);
    }
}

TEST_CASE("Routine schedule validation", "[routine]") {
    auto now = std::chrono::system_clock::now();

    SECTION("Accepted schedules construct") {
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("daily"), std::nullopt, true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("weekly"), std::nullopt, true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("weekdays"), std::nullopt, true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::nullopt, true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("23:59"), std::nullopt, true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("00:00"), std::nullopt, true, now, now, 1));
    }

    SECTION("Invalid schedules throw") {
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("hourly"), std::nullopt, true, now, now, 1), std::invalid_argument);
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("24:00"), std::nullopt, true, now, now, 1), std::invalid_argument);
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("12:60"), std::nullopt, true, now, now, 1), std::invalid_argument);
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("7:30"), std::nullopt, true, now, now, 1), std::invalid_argument);
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("* * * * *"), std::nullopt, true, now, now, 1), std::invalid_argument);
    }
}

TEST_CASE("Routine time zone validation", "[routine]") {
    auto now = std::chrono::system_clock::now();

    SECTION("Accepted time zones construct") {
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::string("UTC"), true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::string("Asia/Kolkata"), true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::string("America/New_York"), true, now, now, 1));
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::string("Europe/London"), true, now, now, 1));
    }

    SECTION("Unknown time zone throws") {
        REQUIRE_THROWS_AS(Routine("r1", "p1", "L", std::string("07:30"), std::string("Mars/Olympus"), true, now, now, 1), std::invalid_argument);
    }

    SECTION("Schedule without time_zone is allowed (defaults to UTC)") {
        REQUIRE_NOTHROW(Routine("r1", "p1", "L", std::string("07:30"), std::nullopt, true, now, now, 1));
    }
}

TEST_CASE("Routine to_json and from_json round trip", "[routine]") {
    auto now = std::chrono::system_clock::now();
    Routine orig("r1", "p1", "Morning meds", std::string("07:30"), std::string("Asia/Kolkata"), true, now, now, 1);

    json j = orig.to_json();
    Routine restored = Routine::from_json(j);

    REQUIRE(restored.get_routine_id() == "r1");
    REQUIRE(restored.get_profile_id() == "p1");
    REQUIRE(restored.get_label() == "Morning meds");
    REQUIRE(restored.get_schedule().has_value());
    REQUIRE(restored.get_schedule().value() == "07:30");
    REQUIRE(restored.get_time_zone().has_value());
    REQUIRE(restored.get_time_zone().value() == "Asia/Kolkata");
    REQUIRE(restored.is_enabled());
}

TEST_CASE("Routine is configuration only, not confirmation", "[routine][privacy]") {
    Routine r = Routine::create("r1", "p1", "Take medication", std::string("08:00"), std::string("UTC"));
    json j = r.to_json();

    // The serialized routine must not carry any completion/confirmation state.
    for (const char* forbidden : {"completed", "confirmed", "done", "status", "last_completed"}) {
        REQUIRE_FALSE(j.contains(forbidden));
    }
}
