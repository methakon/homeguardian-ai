#include <catch2/catch.hpp>
#include "core/Event.h"
#include <chrono>

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("Event construction and validation", "[event]") {
    auto now = std::chrono::system_clock::now();
    
    SECTION("Valid event constructs successfully") {
        REQUIRE_NOTHROW(Event("id1", 1, EventType::observation, "cam1", now, now, std::nullopt, 0.8, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt));
    }
    
    SECTION("Empty event_id throws") {
        REQUIRE_THROWS_AS(Event("", 1, EventType::observation, "cam1", now, now, std::nullopt, 0.8, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt), std::invalid_argument);
    }
    
    SECTION("Schema version < 1 throws") {
        REQUIRE_THROWS_AS(Event("id1", 0, EventType::observation, "cam1", now, now, std::nullopt, 0.8, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt), std::invalid_argument);
    }
    
    SECTION("Empty source throws") {
        REQUIRE_THROWS_AS(Event("id1", 1, EventType::observation, "", now, now, std::nullopt, 0.8, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt), std::invalid_argument);
    }
    
    SECTION("Invalid confidence throws") {
        REQUIRE_THROWS_AS(Event("id1", 1, EventType::observation, "cam1", now, now, std::nullopt, 1.5, std::nullopt, json::object(), std::nullopt, std::nullopt, std::nullopt), std::invalid_argument);
    }
}

TEST_CASE("Event to_json and from_json round trip", "[event]") {
    Event orig = Event::create_inference("id2", "modelX", {{"key", "val"}}, 0.95);
    json j = orig.to_json();
    Event restored = Event::from_json(j);
    
    REQUIRE(restored.get_event_id() == orig.get_event_id());
    REQUIRE(restored.get_type() == EventType::inference);
    REQUIRE(restored.get_source() == "modelX");
    REQUIRE(restored.get_confidence() == 0.95);
    REQUIRE(restored.get_payload() == orig.get_payload());
}
