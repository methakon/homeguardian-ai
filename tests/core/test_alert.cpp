#include "catch2/catch.hpp"
#include "core/Alert.h"

using namespace homeguardian;
using json = nlohmann::json;

TEST_CASE("Alert creation and serialization", "[alert]") {
    Alert a = Alert::create("a1", Severity::critical, "msg", {"id1"}, 0.99, "rule1", json::object());
    
    REQUIRE(a.alert_id == "a1");
    REQUIRE(a.severity == Severity::critical);
    
    json j = a.to_json();
    Alert restored = Alert::from_json(j);
    
    REQUIRE(restored.alert_id == a.alert_id);
    REQUIRE(restored.rule_name == a.rule_name);
}
