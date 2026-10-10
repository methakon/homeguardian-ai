#include "catch2/catch.hpp"
#include "core/SimulatedSensorSource.h"
#include "core/Event.h"
#include <chrono>

using namespace homeguardian;

TEST_CASE("Simulated sensor source: deterministic emit", "[simulated_sensor]") {
    SimulatedSensorSource src;
    REQUIRE(src.name() == "simulated_sensor");

    SimulatedReading r1;
    r1.event_id = "evt_001";
    r1.source = "sensor.motion";
    r1.kind = SimulatedSensorKind::motion;

    SimulatedReading r2;
    r2.event_id = "evt_002";
    r2.source = "sensor.presence";
    r2.kind = SimulatedSensorKind::presence;

    SimulatedReading r3;
    r3.event_id = "evt_003";
    r3.source = "sensor.custom";
    r3.kind = SimulatedSensorKind::custom;

    src.enqueue(r1);
    src.enqueue(r2);
    src.enqueue(r3);

    auto e1 = src.next();
    REQUIRE(e1.has_value());
    REQUIRE(e1->get_event_id() == "evt_001");

    auto e2 = src.next();
    REQUIRE(e2.has_value());
    REQUIRE(e2->get_event_id() == "evt_002");

    auto e3 = src.next();
    REQUIRE(e3.has_value());
    REQUIRE(e3->get_event_id() == "evt_003");
}

TEST_CASE("Simulated sensor source: synthetic marking", "[simulated_sensor]") {
    SimulatedSensorSource src;

    SimulatedReading r_motion;
    r_motion.event_id = "syn_motion";
    r_motion.source = "sensor.simulated.motion";
    r_motion.kind = SimulatedSensorKind::motion;

    SimulatedReading r_presence;
    r_presence.event_id = "syn_presence";
    r_presence.source = "sensor.simulated.presence";
    r_presence.kind = SimulatedSensorKind::presence;

    SimulatedReading r_custom;
    r_custom.event_id = "syn_custom";
    r_custom.source = "sensor.simulated.custom";
    r_custom.kind = SimulatedSensorKind::custom;
    r_custom.payload = nlohmann::json{{"key", "value"}};

    src.enqueue(r_motion);
    src.enqueue(r_presence);
    src.enqueue(r_custom);

    auto e_motion = src.next();
    REQUIRE(e_motion.has_value());
    const auto& p_motion = e_motion->get_payload();
    REQUIRE(p_motion["synthetic"] == true);
    REQUIRE(p_motion["sensor_kind"] == "motion");

    auto e_presence = src.next();
    REQUIRE(e_presence.has_value());
    const auto& p_presence = e_presence->get_payload();
    REQUIRE(p_presence["synthetic"] == true);
    REQUIRE(p_presence["sensor_kind"] == "presence");

    auto e_custom = src.next();
    REQUIRE(e_custom.has_value());
    const auto& p_custom = e_custom->get_payload();
    REQUIRE(p_custom["synthetic"] == true);
    REQUIRE(p_custom["sensor_kind"] == "custom");
}

TEST_CASE("Simulated sensor source: configurable timestamps", "[simulated_sensor]") {
    SimulatedSensorSource src;

    auto past_ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(1577836800000LL));

    SimulatedReading r;
    r.event_id = "ts_001";
    r.source = "sensor.simulated.motion";
    r.kind = SimulatedSensorKind::motion;
    r.observation_timestamp = past_ts;

    src.enqueue(r);

    auto emitted = src.next();
    REQUIRE(emitted.has_value());

    auto expected_ms = std::chrono::duration_cast<std::chrono::milliseconds>(past_ts.time_since_epoch()).count();
    auto actual_ms = std::chrono::duration_cast<std::chrono::milliseconds>(emitted->get_observation_timestamp().time_since_epoch()).count();
    REQUIRE(actual_ms == expected_ms);
}

TEST_CASE("Simulated sensor source: malformed input", "[simulated_sensor]") {
    SimulatedSensorSource src;
    src.enqueue_malformed("bad1", "src");
    REQUIRE_THROWS_AS(src.next(), std::invalid_argument);
}

TEST_CASE("Simulated sensor source: disconnect and reconnect", "[simulated_sensor]") {
    SimulatedSensorSource src;

    SimulatedReading r1;
    r1.event_id = "disc_001";
    r1.source = "sensor.simulated.motion";
    r1.kind = SimulatedSensorKind::motion;

    SimulatedReading r2;
    r2.event_id = "disc_002";
    r2.source = "sensor.simulated.presence";
    r2.kind = SimulatedSensorKind::presence;

    src.enqueue(r1);
    src.enqueue(r2);

    size_t count_before = src.emitted_count();
    src.disconnect();

    auto disconnected_result = src.next();
    REQUIRE_FALSE(disconnected_result.has_value());
    REQUIRE(src.emitted_count() == count_before);

    src.reconnect();

    auto reconnected_result = src.next();
    REQUIRE(reconnected_result.has_value());
    REQUIRE(reconnected_result->get_event_id() == "disc_001");
    REQUIRE(src.emitted_count() == count_before + 1);
}

TEST_CASE("Simulated sensor source: exhaustion", "[simulated_sensor]") {
    SECTION("Empty source has zero pending and returns nullopt") {
        SimulatedSensorSource src;
        REQUIRE(src.pending() == 0);
        auto res = src.next();
        REQUIRE_FALSE(res.has_value());
        REQUIRE(src.pending() == 0);
    }

    SECTION("Source returns nullopt after consuming all enqueued readings") {
        SimulatedSensorSource src;

        SimulatedReading r;
        r.event_id = "ex_001";
        r.source = "sensor.simulated.motion";
        r.kind = SimulatedSensorKind::motion;
        src.enqueue(r);

        auto e1 = src.next();
        REQUIRE(e1.has_value());

        auto e2 = src.next();
        REQUIRE_FALSE(e2.has_value());
    }
}
