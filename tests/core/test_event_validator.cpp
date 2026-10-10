#include "catch2/catch.hpp"
#include "core/EventValidator.h"

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("Event validation logic", "[event_validator]") {
    auto now = std::chrono::system_clock::now();
    
    SECTION("Valid event returns nullopt") {
        Event e = Event::create_observation("id", "src", json::object());
        REQUIRE_FALSE(EventValidator::validate(e).has_value());
    }
    
    SECTION("Future timestamp fails") {
        auto future = now + std::chrono::hours(1);
        Event e("id", 1, EventType::observation, "src", future, future, std::nullopt, std::nullopt, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt);
        auto res = EventValidator::validate(e);
        REQUIRE(res.has_value());
        REQUIRE(res.value() == "observation_timestamp is in the future");
    }
}

TEST_CASE("Event normalization", "[event_validator]") {
    auto now = std::chrono::system_clock::now();
    Event e("id", 1, EventType::observation, "  src  ", now, std::chrono::system_clock::time_point(), std::nullopt, 0.12345, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt);
    
    Event norm = EventValidator::normalize(e);
    
    REQUIRE(norm.get_source() == "src");
    REQUIRE(norm.get_confidence().value() == Catch::Detail::Approx(0.1235));
    REQUIRE(norm.get_ingestion_timestamp().time_since_epoch().count() > 0);
    REQUIRE(norm.get_observation_timestamp() == e.get_observation_timestamp());
}
