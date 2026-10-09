#include <catch2/catch.hpp>
#include "core/Pipeline.h"
#include "core/TimeWindowCorrelation.h"
#include "core/EventValidator.h"

using namespace homeguardian;

TEST_CASE("Pipeline processing", "[pipeline]") {
    auto rule = std::make_shared<TimeWindowCorrelation>("rule", std::vector<EventType>{EventType::observation}, 2, std::chrono::milliseconds(5000), Severity::info, "msg");
    Pipeline pipeline({rule});
    
    SECTION("Valid event returns nullopt if no alert") {
        Event e1 = Event::create_observation("id1", "src", {});
        auto alert = pipeline.process(e1);
        REQUIRE_FALSE(alert.has_value());
    }
    
    SECTION("Generates alert when rule fires") {
        Event e1 = Event::create_observation("id1", "src", {});
        Event e2 = Event::create_observation("id2", "src", {});
        pipeline.process(e1);
        auto alert = pipeline.process(e2);
        REQUIRE(alert.has_value());
    }
    
    SECTION("Max history size bounds memory") {
        pipeline.set_max_history_size(1);
        Event e1 = Event::create_observation("id1", "src", {});
        Event e2 = Event::create_observation("id2", "src", {});
        pipeline.process(e1);
        pipeline.process(e2); 
        auto alert = pipeline.process(Event::create_observation("id3", "src", {})); 
        REQUIRE_FALSE(alert.has_value());
        REQUIRE(pipeline.get_recent_events(std::chrono::hours(1)).size() == 1);
    }
}
