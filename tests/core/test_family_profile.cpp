#include "catch2/catch.hpp"
#include "core/FamilyProfile.h"
#include <chrono>

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("FamilyProfile construction and validation", "[profile]") {
    auto now = std::chrono::system_clock::now();

    SECTION("Valid profile constructs") {
        REQUIRE_NOTHROW(FamilyProfile("p1", "Alice", AgeBand::adult, true, now, now, 1));
    }

    SECTION("Optional age_band may be absent") {
        REQUIRE_NOTHROW(FamilyProfile("p1", "Alice", std::nullopt, true, now, now, 1));
    }

    SECTION("Empty profile_id throws") {
        REQUIRE_THROWS_AS(FamilyProfile("", "Alice", std::nullopt, true, now, now, 1), std::invalid_argument);
    }

    SECTION("Empty display_name throws") {
        REQUIRE_THROWS_AS(FamilyProfile("p1", "", std::nullopt, true, now, now, 1), std::invalid_argument);
    }

    SECTION("Over-long display_name throws") {
        std::string long_name(200, 'x');
        REQUIRE_THROWS_AS(FamilyProfile("p1", long_name, std::nullopt, true, now, now, 1), std::invalid_argument);
    }

    SECTION("Schema version < 1 throws") {
        REQUIRE_THROWS_AS(FamilyProfile("p1", "Alice", std::nullopt, true, now, now, 0), std::invalid_argument);
    }
}

TEST_CASE("FamilyProfile to_json and from_json round trip", "[profile]") {
    auto now = std::chrono::system_clock::now();
    FamilyProfile orig("p1", "Alice", AgeBand::teen, false, now, now, 1);

    json j = orig.to_json();
    FamilyProfile restored = FamilyProfile::from_json(j);

    REQUIRE(restored.get_profile_id() == "p1");
    REQUIRE(restored.get_display_name() == "Alice");
    REQUIRE(restored.get_age_band().has_value());
    REQUIRE(restored.get_age_band().value() == AgeBand::teen);
    REQUIRE(restored.is_enabled() == false);
    REQUIRE(restored.get_schema_version() == 1);

    SECTION("Profile without age_band round trips as absent") {
        FamilyProfile no_band("p2", "Bob", std::nullopt, true, now, now, 1);
        FamilyProfile r2 = FamilyProfile::from_json(no_band.to_json());
        REQUIRE_FALSE(r2.get_age_band().has_value());
    }
}

TEST_CASE("FamilyProfile stores no sensitive attributes by default", "[profile][privacy]") {
    auto now = std::chrono::system_clock::now();
    FamilyProfile p("p1", "Alice", std::nullopt, true, now, now, 1);
    json j = p.to_json();

    // The serialized form must not contain any of the prohibited fields.
    for (const char* forbidden : {"birth_date", "date_of_birth", "face_embedding",
                                   "voiceprint", "document", "id_number", "ssn"}) {
        REQUIRE_FALSE(j.contains(forbidden));
    }
}
