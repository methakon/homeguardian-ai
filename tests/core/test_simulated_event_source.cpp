#include "catch2/catch.hpp"
#include "core/SimulatedEventSource.h"

using namespace homeguardian;

TEST_CASE("Simulated event source", "[event_source]") {
    std::vector<Event> events;
    events.push_back(Event::create_observation("id1", "src", {}));
    events.push_back(Event::create_observation("id2", "src", {}));
    
    SimulatedEventSource src(events);
    
    REQUIRE(src.name() == "simulated");
    
    auto e1 = src.next();
    REQUIRE(e1.has_value());
    REQUIRE(e1->get_event_id() == "id1");
    
    auto e2 = src.next();
    REQUIRE(e2.has_value());
    REQUIRE(e2->get_event_id() == "id2");
    
    REQUIRE_FALSE(src.next().has_value());
}
