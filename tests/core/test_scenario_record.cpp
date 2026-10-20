// test_scenario_record: what makes a staged physical run usable as evidence, and what does not.
//
// SYNTHETIC fixtures only: no hardware, no capture, no microphone. Every case below is constructed in
// code from a fabricated CaptureFacts, so the suite is deterministic and runs anywhere.
//
// These tests exist because the failure mode they guard against is silent: a run whose scene nobody
// annotated looks exactly like a run whose scene was known empty, and the numbers from it would be read
// as a false-positive rate. Every case below is a way that could happen.

#include <catch2/catch.hpp>

#include "core/ScenarioRecord.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

auto t0() { return std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)); }

SceneAnnotation annotated(OccupancyState state) {
    SceneAnnotation a;
    a.occupancy = state;
    a.operator_confirmed = true;
    a.deliberate_change = "light switched on at t=8s, off at t=20s";
    return a;
}

CaptureFacts facts(size_t requested = 600, size_t captured = 600) {
    CaptureFacts f;
    f.frames_requested = requested;
    f.frames_captured = captured;
    f.driver_dropped = 0;
    f.run_finished = true;
    f.observations = 120;
    f.character_counts = {{"quiet", 93}, {"localized_motion", 19}, {"diffuse_motion", 8}};
    f.peak_motion = 0.98;
    f.motion_verdict = "a sustained increase WAS observed";
    f.runner_started = t0();
    f.runner_ended = t0() + std::chrono::seconds(45);
    f.pipeline_timestamp = t0() + std::chrono::seconds(45);
    return f;
}

}  // namespace

TEST_CASE("scenario: a complete, annotated run is attributable", "[scenario]") {
    const auto record = evaluate_scenario("light_on_off", ScenarioClass::lighting,
                                          annotated(OccupancyState::empty), facts());
    INFO(record.summary());
    REQUIRE(record.complete);
    REQUIRE(record.timestamps_sane);
    REQUIRE(record.attributable);
    REQUIRE(record.blockers.empty());
    REQUIRE(record.motion_labelled() == 27);
    REQUIRE(record.summary().find("lighting") == 0);
}

TEST_CASE("scenario: an unannotated run cannot support a rate, however clean it looks", "[scenario]") {
    // The exact failure this whole layer exists for: nothing recorded whether a person was in the scene.
    const auto record = evaluate_scenario("light_on_off", ScenarioClass::lighting, SceneAnnotation{}, facts());
    INFO(record.summary());
    REQUIRE_FALSE(record.attributable);
    REQUIRE_FALSE(record.blockers.empty());
    REQUIRE(record.blockers.front().find("no operator occupancy annotation") != std::string::npos);
    REQUIRE(record.blockers.front().find("gain artifacts") != std::string::npos);
    // it still reports what it measured
    REQUIRE(record.motion_labelled() == 27);
}

TEST_CASE("scenario: a confirmed-unknown scene is attributable and says so", "[scenario]") {
    const auto record = evaluate_scenario("ambient", ScenarioClass::ambient_none,
                                          annotated(OccupancyState::unknown), facts());
    REQUIRE(record.attributable);
    REQUIRE(record.blockers.size() == 1);
    REQUIRE(record.blockers.front().find("UNKNOWN") != std::string::npos);
}

TEST_CASE("scenario: interruption and incomplete captures are not results", "[scenario][refusal]") {
    SECTION("the run did not finish") {
        auto f = facts();
        f.run_finished = false;
        const auto record = evaluate_scenario("light_on_off", ScenarioClass::lighting,
                                              annotated(OccupancyState::empty), f);
        INFO(record.summary());
        REQUIRE_FALSE(record.complete);
        REQUIRE_FALSE(record.attributable);
        REQUIRE(record.blockers.front().find("did not finish") != std::string::npos);
    }
    SECTION("the capture is short") {
        const auto record = evaluate_scenario("light_on_off", ScenarioClass::lighting,
                                              annotated(OccupancyState::empty), facts(600, 540));
        REQUIRE_FALSE(record.complete);
        REQUIRE_FALSE(record.attributable);
        REQUIRE(record.blockers.front().find("incomplete capture: 540 of 600") != std::string::npos);
    }
    SECTION("frames were dropped by the driver") {
        auto f = facts();
        f.driver_dropped = 12;
        const auto record = evaluate_scenario("light_on_off", ScenarioClass::lighting,
                                              annotated(OccupancyState::empty), f);
        REQUIRE(record.complete);                       // the run itself completed
        REQUIRE(record.attributable);                   // and is usable, with the drop reported
        REQUIRE(record.blockers.size() == 1);
        REQUIRE(record.blockers.front().find("12 frame(s) were dropped") != std::string::npos);
    }
    SECTION("no requested frame count was recorded") {
        auto f = facts();
        f.frames_requested = 0;
        const auto record = evaluate_scenario("x", ScenarioClass::lighting, annotated(OccupancyState::empty), f);
        REQUIRE_FALSE(record.complete);
    }
}

TEST_CASE("scenario: clock problems are detected, not assumed away", "[scenario][refusal]") {
    SECTION("the harness clock went backwards") {
        auto f = facts();
        f.runner_ended = f.runner_started - std::chrono::seconds(2);
        const auto record = evaluate_scenario("x", ScenarioClass::lighting, annotated(OccupancyState::empty), f);
        REQUIRE_FALSE(record.timestamps_sane);
        REQUIRE_FALSE(record.attributable);
        REQUIRE(record.blockers.front().find("went backwards") != std::string::npos);
    }
    SECTION("the pipeline's stamp drifts beyond the allowance") {
        auto f = facts();
        f.pipeline_timestamp = f.runner_ended + std::chrono::minutes(3);
        const auto record = evaluate_scenario("x", ScenarioClass::lighting, annotated(OccupancyState::empty), f);
        REQUIRE_FALSE(record.timestamps_sane);
        REQUIRE(record.blockers.front().find("differs from the harness") != std::string::npos);
    }
    SECTION("a missing pipeline timestamp is a gap, not a pass") {
        auto f = facts();
        f.pipeline_timestamp = std::nullopt;
        const auto record = evaluate_scenario("x", ScenarioClass::lighting, annotated(OccupancyState::empty), f);
        REQUIRE_FALSE(record.timestamps_sane);
        REQUIRE(record.blockers.front().find("no pipeline timestamp") != std::string::npos);
    }
}

TEST_CASE("scenario: frame characters and fusion alerts are reported separately", "[scenario]") {
    // A scene can be full of movement labels and produce no alert. That is a result, not a contradiction,
    // and the two numbers must never be merged into one.
    auto f = facts();
    f.alert_raised = false;
    const auto no_alert = evaluate_scenario("object_cross", ScenarioClass::object_motion,
                                            annotated(OccupancyState::empty), f);
    REQUIRE(no_alert.motion_labelled() == 27);
    REQUIRE_FALSE(no_alert.facts.alert_raised);
    REQUIRE(no_alert.summary().find("no alert") != std::string::npos);

    auto f2 = facts();
    f2.alert_raised = true;
    f2.alert_rule = "sustained-motion-rise";
    f2.alert_confidence = 0.91;
    f2.character_counts = {{"quiet", 118}, {"global_illumination", 2}};
    const auto alerted = evaluate_scenario("light_on", ScenarioClass::lighting,
                                           annotated(OccupancyState::empty), f2);
    REQUIRE(alerted.motion_labelled() == 0);            // no movement labels at all
    REQUIRE(alerted.facts.alert_raised);                // and an alert anyway
    REQUIRE(alerted.summary().find("ALERT RAISED") != std::string::npos);
    REQUIRE(alerted.summary().find("sustained-motion-rise") != std::string::npos);
}

TEST_CASE("scenario: classes are never pooled, and rates are withheld from bad runs", "[scenario][report]") {
    std::vector<ScenarioRecord> records;
    records.push_back(evaluate_scenario("light_on_off", ScenarioClass::lighting,
                                       annotated(OccupancyState::empty), facts()));
    records.push_back(evaluate_scenario("hand_object", ScenarioClass::object_motion,
                                        annotated(OccupancyState::empty), facts()));
    records.push_back(evaluate_scenario("unannotated", ScenarioClass::object_motion,
                                        SceneAnnotation{}, facts()));

    const std::string report = scenario_report(records);
    INFO(report);
    REQUIRE(report.find("== lighting: 1 run(s), 1 attributable") != std::string::npos);
    REQUIRE(report.find("== object_motion: 2 run(s), 1 attributable") != std::string::npos);
    // The good class publishes a rate; the one with an unannotated run does not.
    REQUIRE(report.find("rate: WITHHELD - 1 run(s) in this class are not attributable") != std::string::npos);
    REQUIRE(report.find("== ambient_none") == std::string::npos);
}

TEST_CASE("scenario: class and occupancy names round-trip, and nothing lands in the wrong class",
          "[scenario]") {
    for (ScenarioClass c : {ScenarioClass::lighting, ScenarioClass::object_motion,
                            ScenarioClass::person_motion, ScenarioClass::camera_vibration,
                            ScenarioClass::occlusion, ScenarioClass::occupancy,
                            ScenarioClass::ambient_none}) {
        REQUIRE(scenario_class_from_string(scenario_class_to_string(c)) == c);
    }
    REQUIRE(scenario_class_from_string("light_on_off") == ScenarioClass::unknown);
    REQUIRE(occupancy_state_from_string(occupancy_state_to_string(OccupancyState::person_present)) ==
            OccupancyState::person_present);
    // "unknown" is the default, so an absent annotation can never read as "empty".
    REQUIRE(SceneAnnotation{}.occupancy == OccupancyState::unknown);
    REQUIRE_FALSE(SceneAnnotation{}.operator_confirmed);
}
