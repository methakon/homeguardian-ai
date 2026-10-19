// test_camera_replay: does the RECORDED physical-camera evidence reach an alert?
//
// WHAT THIS IS
// ------------
// A replay. The six confirmed sustained-increase observations from the consented physical capture are
// typed in below exactly as the capture log printed them (docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md
// section 3.1, itself a transcription of that run's stdout), and they are pushed through the REAL
// DetectionFusionRule and the REAL FusionEngine. Nothing is stubbed and no threshold is changed: the
// engine's own confidence propagation and agreement rule decide the outcome.
//
// WHAT THIS IS NOT
// ----------------
// It is NOT a physical end-to-end result, and it must never be reported as one. The frames are not
// re-captured, the observations are rebuilt from recorded scalars, and every Observation below is
// therefore labelled Provenance::synthetic - a replay may not wear the label of a measurement. The
// numbers it produces bound what the pipeline WOULD do with that evidence; whether a delivered alert
// follows from a real run is still unproven, and this test says so in its assertions.
//
// The question it answers: with the recorded evidence, is the alert path reachable at all, and what
// exactly stops it when it is not?

#include <catch2/catch.hpp>

#include "core/DetectionFusionRule.h"
#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

constexpr const char* kDetectorId = "camera-motion-activity";
constexpr const char* kModelId = "ema-baseline-v1";
constexpr const char* kSource = "/dev/video0";

// The recorded ratios of the six consecutive confirmed sustained_increase observations
// (frames 255-281 of the consented capture), and the confidences the capture log reports for them.
// Confidence = |ratio - 1| / 2, clamped to [0, 1] - the detector's own formula
// (ActivityDetectors.cpp, ActivityChangeDetector::to_observation).
const double kRecordedIncreaseConfidence[6] = {0.700, 0.795, 0.849, 0.750, 0.660, 0.425};

// The recorded composition of that 600-frame run, from the capture log's "windows by kind" line.
constexpr int kRecordedIncreases = 6;
constexpr int kRecordedDecreases = 41;   // the operator settling afterwards - confirmed, so "detected"
constexpr int kRecordedQuiet = 68;       // motion within the established baseline - not_detected
constexpr int kRecordedWarming = 3;      // no baseline yet - inconclusive, and therefore unusable

// The recorded still-scene composition (docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log):
// sustained_increase=0. This is the run that must never alert.
constexpr int kStillQuiet = 8;
constexpr int kStillDecreases = 5;

std::chrono::system_clock::time_point base_time() {
    return std::chrono::system_clock::now() - std::chrono::seconds(30);
}

Observation make_observation(int index, DetectorStatus status, double confidence,
                             std::chrono::system_clock::time_point now) {
    Observation o;
    o.observation_id = "replay-" + std::to_string(index);
    o.source = kSource;
    o.provenance = Provenance::synthetic;   // a replay is not a measurement; the label says so
    o.detector_id = kDetectorId;
    o.model_id = kModelId;
    o.status = status;
    o.confidence = confidence;
    o.observation_timestamp = now;
    o.features = nlohmann::json::object();
    o.features["replay"] = true;
    o.features["direction"] = "rising";   // recorded rises; the replay keeps the direction it saw
    return o;
}

// A window with `increases` confirmed rises (using the recorded confidences in order), `decreases`
// confirmed falls, `quiet` baseline observations and `warming` unusable ones.
std::vector<Observation> window_of(int increases, int decreases, int quiet, int warming,
                                   double decrease_confidence = 0.10) {
    std::vector<Observation> obs;
    const auto base = base_time();
    int i = 0;
    for (int k = 0; k < decreases; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(make_observation(i++, DetectorStatus::detected, decrease_confidence, at));
    }
    for (int k = 0; k < increases && k < 6; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(make_observation(i++, DetectorStatus::detected,
                                       kRecordedIncreaseConfidence[k], at));
    }
    for (int k = 0; k < quiet; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(make_observation(i++, DetectorStatus::not_detected, 0.0, at));
    }
    for (int k = 0; k < warming; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(make_observation(i++, DetectorStatus::inconclusive, 0.0, at));
    }
    return obs;
}

DetectionFusionRule camera_rule() {
    DetectionFusionRule rule("detection-fusion");
    rule.set_evidence_detector_ids({kDetectorId});
    return rule;
}

std::vector<Event> as_events(const std::vector<Observation>& obs) {
    std::vector<Event> events;
    events.reserve(obs.size());
    for (const auto& o : obs) events.push_back(o.to_event());
    return events;
}

}  // namespace

TEST_CASE("camera replay: the six recorded confirmed rises, alone in a window, do reach the alert "
          "path", "[camera][replay]") {
    // Reachability, not a measurement: this is what the pipeline does with that evidence when nothing
    // else shares the window.
    const FusionEngine engine;
    const auto obs = window_of(kRecordedIncreases, 0, 0, 0);
    const FusionResult fused = engine.fuse(obs);

    INFO("fused verdict=" << fusion_verdict_to_string(fused.verdict)
                          << " confidence=" << fused.confidence
                          << " contributing=" << fused.contributing);
    REQUIRE(fused.verdict == FusionVerdict::detected);
    REQUIRE(fused.contributing == 6);

    const auto alert = camera_rule().evaluate(as_events(obs));
    REQUIRE(alert.has_value());
    INFO("alert severity confidence=" << alert->confidence);
    REQUIRE(alert->confidence == Approx(fused.confidence));
    // Recorded peak confidence 0.849 is below the rule's warning band, so the alert is informational.
    REQUIRE(alert->severity == Severity::info);
}

TEST_CASE("camera replay: the recorded evidence survives exactly one quiet observation beside it",
          "[camera][replay]") {
    // The dilution boundary, measured rather than guessed. FusionEngine propagates
    // confidence = mean(confidences) * agreement * penalty, so a single zero-confidence vote both
    // lowers the mean and lowers agreement.
    const FusionEngine engine;
    const auto one_quiet = window_of(kRecordedIncreases, 0, 1, 0);
    const FusionResult with_one = engine.fuse(one_quiet);
    INFO("one quiet vote: verdict=" << fusion_verdict_to_string(with_one.verdict)
                                    << " confidence=" << with_one.confidence
                                    << " agreement-path=" << with_one.reason);
    REQUIRE(with_one.verdict == FusionVerdict::detected);
    REQUIRE(camera_rule().evaluate(as_events(one_quiet)).has_value());

    const auto two_quiet = window_of(kRecordedIncreases, 0, 2, 0);
    const FusionResult with_two = engine.fuse(two_quiet);
    INFO("two quiet votes: verdict=" << fusion_verdict_to_string(with_two.verdict)
                                     << " confidence=" << with_two.confidence);
    REQUIRE(with_two.confidence < 0.5);
    REQUIRE_FALSE(camera_rule().evaluate(as_events(two_quiet)).has_value());
}

TEST_CASE("camera replay: the recorded 600-frame run as a whole does NOT alert", "[camera][replay]") {
    // The headline result of this investigation. Replayed with the run's own recorded composition,
    // the fusion is inconclusive: the 41 settling decreases are confirmed changes too, so they vote
    // "detected" alongside the 6 rises (47 positive) while the 68 quiet observations vote
    // "not_detected", leaving agreement at 0.591 - just under the engine's 0.6 threshold. Neither the
    // rise nor the confidence gate is the blocker; the vote mix is.
    const FusionEngine engine;
    const auto obs = window_of(kRecordedIncreases, kRecordedDecreases, kRecordedQuiet,
                               kRecordedWarming);
    const FusionResult fused = engine.fuse(obs);

    INFO("recorded run: verdict=" << fusion_verdict_to_string(fused.verdict)
                                  << " confidence=" << fused.confidence
                                  << " contributing=" << fused.contributing
                                  << " reason=" << fused.reason);
    REQUIRE(fused.contributing == kRecordedIncreases + kRecordedDecreases + kRecordedQuiet);
    REQUIRE(fused.verdict == FusionVerdict::inconclusive);
    REQUIRE(fused.confidence < 0.5);
    // 68 quiet votes beat 47 confirmed-change votes: agreement 68/115 = 0.5913 < 0.6.
    REQUIRE(fused.confidence < 0.06);

    const auto alert = camera_rule().evaluate(as_events(obs));
    REQUIRE_FALSE(alert.has_value());   // no alert from the recorded evidence as it stands
}

TEST_CASE("camera replay: the recorded still-scene run can never alert", "[camera][replay]") {
    // sustained_increase=0 in the durable log. Nothing here is close to the gate, in either window
    // shape, and no threshold change could make it alert.
    const FusionEngine engine;
    for (int increases : {0}) {
        const auto obs = window_of(increases, kStillDecreases, kStillQuiet, 0);
        const FusionResult fused = engine.fuse(obs);
        INFO("still scene: verdict=" << fusion_verdict_to_string(fused.verdict)
                                     << " confidence=" << fused.confidence);
        REQUIRE_FALSE(camera_rule().evaluate(as_events(obs)).has_value());
        REQUIRE(fused.confidence < 0.5);
    }
}

TEST_CASE("camera replay: every replayed observation is labelled synthetic, never real",
          "[camera][replay]") {
    // The guard that keeps this honest: a replay may not borrow the measurement's provenance.
    const auto obs = window_of(kRecordedIncreases, kRecordedDecreases, kRecordedQuiet,
                               kRecordedWarming);
    REQUIRE_FALSE(obs.empty());
    for (const auto& o : obs) {
        REQUIRE(o.provenance == Provenance::synthetic);
    }
    const auto events = as_events(obs);
    for (const auto& ev : events) {
        REQUIRE(ev.get_source() == kSource);   // the recorded source is carried unchanged
    }
}
