#include "catch2/catch.hpp"

#include "core/DetectionFusionRule.h"
#include "core/Observation.h"
#include "core/Pipeline.h"
#include "core/SyntheticActivityDetector.h"

#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// L-11: the detection -> alert causal path. These tests prove that an alert is
// caused by qualifying DETECTION/FUSION evidence (not by raw observation counts
// and not by a hardcoded message), and that inconclusive / unavailable /
// not-detected evidence can never produce one.
// ---------------------------------------------------------------------------

namespace {

SyntheticActivityDetector::Config det_cfg() {
    return SyntheticActivityDetector::defaults();
}

// Build detection Observation events from synthetic energies.
std::vector<Event> inference_events(const std::vector<double>& energies) {
    SyntheticActivityDetector det(det_cfg());
    std::vector<Event> out;
    for (size_t i = 0; i < energies.size(); ++i) {
        Observation o = det.detect("o-" + std::to_string(i), energies[i]);
        o.validate();
        out.push_back(o.to_event());
    }
    return out;
}

std::shared_ptr<DetectionFusionRule> make_rule() {
    return std::make_shared<DetectionFusionRule>("detection-fusion");
}

}  // namespace

// ===========================================================================
// Observation <-> Event round trip
// ===========================================================================

TEST_CASE("Observation::from_event round-trips to_event()", "[detection][l11]") {
    SyntheticActivityDetector det(det_cfg());
    Observation o = det.detect("obs-1", 0.9);   // detected, high quality
    o.validate();

    auto back = Observation::from_event(o.to_event());
    REQUIRE(back.has_value());
    REQUIRE(back->status == DetectorStatus::detected);
    REQUIRE(back->provenance == Provenance::synthetic);
    REQUIRE(back->detector_id == o.detector_id);
    REQUIRE(back->model_id == o.model_id);
    REQUIRE(back->confidence.has_value());
    REQUIRE(back->confidence.value() == Approx(o.confidence.value()));

    // A raw observation event is not a detection observation.
    Event raw = Event::create_observation("raw-1", "src", nlohmann::json::object());
    REQUIRE_FALSE(Observation::from_event(raw).has_value());

    // An inference event without the detector payload is rejected.
    Event bare = Event::create_inference("inf-1", "src", nlohmann::json::object(), 0.5);
    REQUIRE_FALSE(Observation::from_event(bare).has_value());

    // An unknown status is rejected rather than coerced.
    Event weird = Event::create_inference(
        "inf-2", "src",
        nlohmann::json{{"detector_id", "d"}, {"status", "banana"}}, 0.5);
    REQUIRE_FALSE(Observation::from_event(weird).has_value());
}

// ===========================================================================
// The rule only fires on qualifying detection evidence
// ===========================================================================

TEST_CASE("L-11: raw observations alone never raise a detection-driven alert",
          "[detection][l11]") {
    auto rule = make_rule();
    std::vector<Event> window = {
        Event::create_observation("r1", "src", nlohmann::json::object()),
        Event::create_observation("r2", "src", nlohmann::json::object()),
        Event::create_observation("r3", "src", nlohmann::json::object()),
    };
    REQUIRE_FALSE(rule->evaluate(window).has_value());
}

TEST_CASE("L-11: inconclusive evidence never raises an alert",
          "[detection][l11]") {
    auto rule = make_rule();
    // Energies within the inconclusive band (near the 0.5 threshold).
    auto window = inference_events({0.52, 0.49, 0.51});
    for (const auto& e : window) {
        REQUIRE(Observation::from_event(e)->status == DetectorStatus::inconclusive);
    }
    REQUIRE_FALSE(rule->evaluate(window).has_value());
}

TEST_CASE("L-11: not_detected evidence never raises an alert",
          "[detection][l11]") {
    auto rule = make_rule();
    auto window = inference_events({0.2, 0.25, 0.3});
    for (const auto& e : window) {
        REQUIRE(Observation::from_event(e)->status == DetectorStatus::not_detected);
    }
    REQUIRE_FALSE(rule->evaluate(window).has_value());
}

TEST_CASE("L-11: a single detected observation is insufficient",
          "[detection][l11]") {
    auto rule = make_rule();
    auto window = inference_events({0.9});
    REQUIRE_FALSE(rule->evaluate(window).has_value());
}

TEST_CASE("L-11: conflicting evidence is not resolved into an alert",
          "[detection][l11]") {
    auto rule = make_rule();
    // 1 detected vs 1 not_detected -> agreement 0.5 < 0.6 -> inconclusive.
    auto window = inference_events({0.9, 0.1});
    REQUIRE_FALSE(rule->evaluate(window).has_value());
}

TEST_CASE("L-11: agreeing detections raise an alert whose severity follows the "
          "evidence",
          "[detection][l11]") {
    auto rule = make_rule();

    // High-confidence agreement -> warning.
    auto strong = inference_events({0.95, 0.92});
    auto a = rule->evaluate(strong);
    REQUIRE(a.has_value());
    REQUIRE(a->rule_name == "detection-fusion");
    REQUIRE(a->severity == Severity::warning);
    REQUIRE(a->confidence >= 0.8);
    REQUIRE(a->contributing_event_ids.size() >= 2);
    REQUIRE(a->details["fusion_verdict"] == "detected");
    REQUIRE(a->details["provenance"] == "synthetic");
    REQUIRE(a->details.contains("reason"));

    // Weaker (but still qualifying) agreement -> info, never warning/critical.
    auto weak = inference_events({0.68, 0.72});
    auto b = rule->evaluate(weak);
    REQUIRE(b.has_value());
    REQUIRE(b->severity == Severity::info);
    REQUIRE(b->confidence < 0.8);
}

TEST_CASE("L-11: the alert is caused by the pipeline carrying the detection "
          "evidence",
          "[detection][l11]") {
    // End-to-end through the EXISTING pipeline: the first detection observation
    // cannot alert; the second agreeing one does.
    std::vector<std::shared_ptr<ICorrelationRule>> rules{make_rule()};
    Pipeline pipeline(rules);

    auto events = inference_events({0.95, 0.93});
    REQUIRE(events.size() == 2);

    REQUIRE_FALSE(pipeline.process(events[0]).has_value());   // 1 input -> no alert
    auto alert = pipeline.process(events[1]);                 // 2 agreeing -> alert
    REQUIRE(alert.has_value());
    REQUIRE(alert->rule_name == "detection-fusion");
    REQUIRE(alert->severity == Severity::warning);
}

TEST_CASE("L-11: contributing ids are bounded", "[detection][l11]") {
    DetectionFusionRule::Config cfg = DetectionFusionRule::defaults();
    cfg.max_contributing = 2;
    DetectionFusionRule rule("detection-fusion", cfg);

    std::vector<double> energies;
    for (int i = 0; i < 8; ++i) energies.push_back(0.9);   // all detected
    auto window = inference_events(energies);
    auto a = rule.evaluate(window);
    REQUIRE(a.has_value());
    REQUIRE(a->contributing_event_ids.size() <= 2);
    REQUIRE(a->details["detector_ids"].size() <= 2);
}
