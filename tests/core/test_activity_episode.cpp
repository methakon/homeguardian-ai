// test_activity_episode: can a short sustained change be represented without letting the still scene in?
//
// Every input below is a REPLAY of recorded scalars from the consented physical capture and from the
// durable still-scene run (docs/evidence/), labelled Provenance::synthetic because a replay is not a
// measurement. These tests establish what the pipeline WOULD do; they do not establish a physical
// delivered alert, and no test here claims one.
//
// The comparison that matters is before/after:
//   before - FusionEngine alone: the recorded run's vote mix misses the 0.6 agreement threshold, so a
//            change the detector confirmed six times over is reported as `inconclusive`;
//   after  - ActivityEpisodeSelector (opt-in): the same change is judged on its own six observations,
//            while quiet scenes, one-off spikes and runs of confirmed FALLS are all still refused.

#include <catch2/catch.hpp>

#include "core/ActivityEpisode.h"
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

// Recorded: the six consecutive confirmed sustained_increase observations of the consented capture
// (frames 255-281). Confidence = |ratio - 1| / 2, the detector's own formula.
const double kRecordedRiseConfidence[6] = {0.700, 0.795, 0.849, 0.750, 0.660, 0.425};
// Recorded: the five confirmed sustained_decrease observations of the still-scene run
// (ratios 0.39, 0.30, 0.26, 0.35, 0.41 -> confidence 0.305, 0.350, 0.370, 0.325, 0.295).
const double kRecordedFallConfidence[5] = {0.305, 0.350, 0.370, 0.325, 0.295};
// Recorded: the compositions reported by each run's "windows by kind" line.
constexpr int kRunIncreases = 6;
constexpr int kRunDecreases = 41;
constexpr int kRunQuiet = 68;
constexpr int kRunWarming = 3;
constexpr int kStillDecreases = 5;
constexpr int kStillQuiet = 8;

std::chrono::system_clock::time_point base_time() {
    return std::chrono::system_clock::now() - std::chrono::seconds(30);
}

Observation obs_of(int index, double confidence, DetectorStatus status, const char* direction,
                   std::chrono::system_clock::time_point now) {
    Observation o;
    o.observation_id = "ep-" + std::to_string(index);
    o.source = "/dev/video0";
    o.provenance = Provenance::synthetic;   // replay
    o.detector_id = kDetectorId;
    o.model_id = kModelId;
    o.status = status;
    o.confidence = confidence;
    o.observation_timestamp = now;
    o.features = nlohmann::json::object();
    o.features["direction"] = direction;
    return o;
}

// A window in recorded order: the rises happen first, then the settling falls, then the baseline.
std::vector<Observation> recorded_run(const double* rises, size_t rise_count, int falls,
                                      int quiet, int warming) {
    std::vector<Observation> obs;
    const auto base = base_time();
    int i = 0;
    for (size_t k = 0; k < rise_count; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(obs_of(i++, rises[k], DetectorStatus::detected, "rising", at));
    }
    for (int k = 0; k < falls; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(obs_of(i++, kRecordedFallConfidence[k % 5], DetectorStatus::detected,
                             "falling", at));
    }
    for (int k = 0; k < quiet; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(obs_of(i++, 0.0, DetectorStatus::not_detected, "rising", at));
    }
    for (int k = 0; k < warming; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        obs.push_back(obs_of(i++, 0.0, DetectorStatus::inconclusive, "rising", at));
    }
    return obs;
}

std::vector<Event> as_events(const std::vector<Observation>& obs) {
    std::vector<Event> events;
    for (const auto& o : obs) events.push_back(o.to_event());
    return events;
}

DetectionFusionRule rule(bool episode_mode) {
    DetectionFusionRule::Config cfg;
    cfg.evidence_detector_ids = {kDetectorId};
    cfg.use_episode_evidence = episode_mode;
    DetectionFusionRule r("detection-fusion", cfg);
    return r;
}

}  // namespace

TEST_CASE("episode: a run of confirmed rises is an episode; a bare window is not",
          "[episode][camera]") {
    const ActivityEpisodeSelector selector;

    // The six recorded rises.
    const auto rises = recorded_run(kRecordedRiseConfidence, 6, 0, 0, 0);
    const auto episode = selector.best(rises);
    REQUIRE(episode.has_value());
    REQUIRE(episode->length == 6);
    REQUIRE(episode->direction == "rising");
    REQUIRE(episode->mean_confidence == Approx(0.6965).epsilon(0.001));
    REQUIRE(episode->weakest_confidence == Approx(0.425));
    REQUIRE(episode->provenance == EvidenceProvenance::synthetic);   // a replay stays synthetic
    INFO(episode->reason);

    // Quiet baseline only: no confirmed change exists, so there is nothing to call an episode.
    const auto quiet = recorded_run(kRecordedRiseConfidence, 0, 0, kStillQuiet, 0);
    REQUIRE_FALSE(selector.best(quiet).has_value());
}

TEST_CASE("episode: the recorded full run selects the RISES, not the longer settling run",
          "[episode][camera]") {
    const ActivityEpisodeSelector selector;
    const auto obs = recorded_run(kRecordedRiseConfidence, kRunIncreases, kRunDecreases, kRunQuiet,
                                  kRunWarming);
    const auto episode = selector.best(obs);
    size_t detected = 0, falling = 0, rising = 0, quiet_count = 0;
    for (const auto& o : obs) {
        if (o.status != DetectorStatus::detected) { ++quiet_count; continue; }
        ++detected;
        const std::string d = o.features.value("direction", std::string());
        if (d == "falling") ++falling;
        if (d == "rising") ++rising;
    }
    INFO("window: total=" << obs.size() << " detected=" << detected << " rising=" << rising
                          << " falling=" << falling << " not_detected_or_other=" << quiet_count);
    REQUIRE(episode.has_value());
    REQUIRE(episode->length == 6);                       // 6 rises, not the 41 falls
    REQUIRE(episode->direction == "rising");
    REQUIRE(episode->mean_confidence == Approx(0.6965).epsilon(0.001));
}

TEST_CASE("episode: a brief isolated spike is refused - one anomalous frame is a door slamming",
          "[episode][camera][false-positive]") {
    const ActivityEpisodeSelector selector;
    for (int spike_length : {1, 2}) {
        std::vector<Observation> obs;
        const auto base = base_time();
        int i = 0;
        for (int k = 0; k < spike_length; ++k) {
            const auto at = base + std::chrono::milliseconds(10 * i);
            obs.push_back(obs_of(i++, 0.95, DetectorStatus::detected, "rising", at));
        }
        for (int k = 0; k < 6; ++k) {
            const auto at = base + std::chrono::milliseconds(10 * i);
            obs.push_back(obs_of(i++, 0.0, DetectorStatus::not_detected, "rising", at));
        }
        INFO("spike length " << spike_length);
        REQUIRE_FALSE(selector.best(obs).has_value());
        REQUIRE_FALSE(rule(true).evaluate(as_events(obs)).has_value());
    }

    // At three consecutive confirmed changes it becomes an episode - the boundary, asserted.
    std::vector<Observation> three;
    const auto base = base_time();
    for (int k = 0; k < 3; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * k);
        three.push_back(obs_of(k, 0.8, DetectorStatus::detected, "rising", at));
    }
    REQUIRE(selector.best(three).has_value());
}

TEST_CASE("episode: a run of confirmed FALLS is not an activity episode", "[episode][camera][false-positive]") {
    // The recorded still-scene run's only confirmed changes are falls - the subject walking away.
    // Reading that as "activity" would be a false positive invented by the improvement itself.
    std::vector<Observation> still;
    const auto base = base_time();
    int i = 0;
    for (double confidence : kRecordedFallConfidence) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        still.push_back(obs_of(i++, confidence, DetectorStatus::detected, "falling", at));
    }
    for (int k = 0; k < kStillQuiet; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        still.push_back(obs_of(i++, 0.0, DetectorStatus::not_detected, "rising", at));
    }

    const ActivityEpisodeSelector default_selector;
    REQUIRE_FALSE(default_selector.best(still).has_value());
    // The recorded falls are weak: mean 0.329 against the unchanged 0.5 gate. So the still scene is
    // refused here even by a selector that ignores direction - loosening the direction rule does not
    // open the door, it just moves which guard does the refusing.
    ActivityEpisodeSelector::Config relaxed;
    relaxed.require_rising = false;
    REQUIRE_FALSE(ActivityEpisodeSelector(relaxed).best(still).has_value());

    // The direction rule is a separate guard, and here is what it alone prevents: a run of
    // HIGH-confidence falls - which would clear the confidence gate - is still not an activity
    // episode, because activity is not "activity stopped".
    std::vector<Observation> strong_falls;
    for (int k = 0; k < 3; ++k) {
        const auto at = base_time() + std::chrono::milliseconds(10 * k);
        strong_falls.push_back(obs_of(k, 0.8, DetectorStatus::detected, "falling", at));
    }
    REQUIRE_FALSE(default_selector.best(strong_falls).has_value());
    const auto without_direction_rule = ActivityEpisodeSelector(relaxed).best(strong_falls);
    REQUIRE(without_direction_rule.has_value());
    REQUIRE(without_direction_rule->length == 3);
    REQUIRE(without_direction_rule->mean_confidence == Approx(0.8));

    REQUIRE_FALSE(rule(true).evaluate(as_events(still)).has_value());
}

TEST_CASE("episode before/after: the recorded run's alert reachability changes, the still scene's does not",
          "[episode][camera][before-after]") {
    const auto run = recorded_run(kRecordedRiseConfidence, kRunIncreases, kRunDecreases, kRunQuiet,
                                  kRunWarming);
    const auto events = as_events(run);

    // BEFORE (default configuration): the classic fusion, unchanged.
    const FusionEngine engine;
    const FusionResult fused = engine.fuse(run);
    INFO("classic: verdict=" << fusion_verdict_to_string(fused.verdict)
                             << " confidence=" << fused.confidence
                             << " contributing=" << fused.contributing);
    REQUIRE(fused.verdict == FusionVerdict::inconclusive);
    // With the recorded settling confidences the classic result is 0.0884; with the more forgiving
    // 0.10 assumption used in test_camera_replay.cpp it is 0.0415. Both are far below the 0.5 gate and
    // both are inconclusive, so the conclusion does not depend on that assumption - which is the point
    // of asserting the range rather than a single number.
    REQUIRE(fused.confidence == Approx(0.0884150332).epsilon(0.0005));
    REQUIRE(fused.confidence > 0.04);
    REQUIRE(fused.confidence < 0.10);
    REQUIRE_FALSE(rule(false).evaluate(events).has_value());

    // AFTER (opt-in episode evidence): the same window, judged on the change instead of the window size.
    const auto alert = rule(true).evaluate(events);
    REQUIRE(alert.has_value());
    REQUIRE(alert->details["evidence_mode"] == "episode");
    REQUIRE(alert->details["episode"]["length"] == 6);
    REQUIRE(alert->details["episode"]["direction"] == "rising");
    REQUIRE(alert->confidence == Approx(0.6965).epsilon(0.001));
    REQUIRE(alert->details["provenance"] == "synthetic");   // the replay label survives
    REQUIRE(alert->severity == Severity::info);             // 0.6965 stays below the warning band
    // The classic result is still reported alongside, so the alert explains why the window as a whole
    // was inconclusive rather than pretending it was not.
    REQUIRE(alert->details["fusion_verdict"] == "inconclusive");

    // And the still scene is unaffected in BOTH modes.
    std::vector<Observation> still;
    const auto base = base_time();
    int i = 0;
    for (double confidence : kRecordedFallConfidence) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        still.push_back(obs_of(i++, confidence, DetectorStatus::detected, "falling", at));
    }
    for (int k = 0; k < kStillQuiet; ++k) {
        const auto at = base + std::chrono::milliseconds(10 * i);
        still.push_back(obs_of(i++, 0.0, DetectorStatus::not_detected, "rising", at));
    }
    REQUIRE_FALSE(rule(false).evaluate(as_events(still)).has_value());
    REQUIRE_FALSE(rule(true).evaluate(as_events(still)).has_value());
}

TEST_CASE("episode: dilution no longer decides the outcome, and the confidence gate is untouched",
          "[episode][camera][before-after]") {
    // Six recorded rises with 0, 1 and 2 quiet observations beside them. The classic path degrades
    // with every added baseline vote; the episode path does not, because the baseline is not part of
    // the episode. No threshold differs between the two - only which observations are evidence.
    const ActivityEpisodeSelector selector;
    const FusionEngine engine;
    const double expected[3] = {0.6965, 0.5117, 0.3918};

    for (int quiet = 0; quiet <= 2; ++quiet) {
        const auto obs = recorded_run(kRecordedRiseConfidence, 6, 0, quiet, 0);
        const FusionResult fused = engine.fuse(obs);
        const auto episode = selector.best(obs);

        INFO("quiet beside the rises: " << quiet << " classic=" << fused.confidence
                                        << " episode=" << (episode ? episode->mean_confidence : 0.0));
        REQUIRE(fused.confidence == Approx(expected[quiet]).epsilon(0.001));
        REQUIRE(episode.has_value());
        REQUIRE(episode->length == 6);
        REQUIRE(episode->mean_confidence == Approx(0.6965).epsilon(0.001));

        // 0 and 1 quiet votes still clear the gate classically; 2 do not. The episode path is stable
        // at all three, which is the point of the change.
        const bool classic_alerts = fused.confidence >= DetectionFusionRule::Config{}.min_confidence;
        REQUIRE(classic_alerts == (quiet < 2));
        REQUIRE(rule(true).evaluate(as_events(obs)).has_value());
    }

    // The gate itself is unchanged, in both rule configurations.
    REQUIRE(DetectionFusionRule::Config{}.min_confidence == 0.5);
    REQUIRE(ActivityEpisodeSelector::Config{}.min_mean_confidence == 0.5);
    REQUIRE(FusionEngine::Config{}.agreement_threshold == 0.6);
}
