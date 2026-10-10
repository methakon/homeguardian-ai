#include "catch2/catch.hpp"
#include "core/TimeWindowCorrelation.h"

using namespace homeguardian;

TEST_CASE("Time Window Correlation Rule", "[correlation]") {
    TimeWindowCorrelation rule("test_rule", {EventType::observation}, 2, std::chrono::milliseconds(5000), Severity::warning, "Alert msg");
    
    auto now = std::chrono::system_clock::now();
    
    Event e1 = Event::create_observation("id1", "cam", {}); // approx now
    Event e2("id2", 1, EventType::observation, "cam", now + std::chrono::milliseconds(1000), now, std::nullopt, std::nullopt, std::nullopt, {}, std::nullopt, std::nullopt, std::nullopt);
    Event e3("id3", 1, EventType::observation, "cam", now + std::chrono::milliseconds(6000), now, std::nullopt, std::nullopt, std::nullopt, {}, std::nullopt, std::nullopt, std::nullopt);
    
    SECTION("Fires when count met within window") {
        auto alert = rule.evaluate({e1, e2});
        REQUIRE(alert.has_value());
        REQUIRE(alert->severity == Severity::warning);
        REQUIRE(alert->contributing_event_ids.size() == 2);
    }
    
    SECTION("Fails when insufficient events") {
        auto alert = rule.evaluate({e1});
        REQUIRE_FALSE(alert.has_value());
    }
    
    SECTION("Fails when events outside window") {
        auto alert = rule.evaluate({e1, e3});
        REQUIRE_FALSE(alert.has_value());
    }
}
