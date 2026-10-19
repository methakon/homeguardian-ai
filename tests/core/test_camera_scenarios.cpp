// test_camera_scenarios: the environmental false-positive and false-negative evaluation.
//
// Every stream below is a SYNTHETIC motion-level sequence - a hypothesis about what a phenomenon does
// to the changed-pixel fraction - fed through the REAL ActivityChangeDetector at a stated cadence, then
// through the REAL ActivityEpisodeSelector and the REAL DetectionFusionRule.
//
// Disciplines this file keeps:
//   * provenance is Provenance::synthetic throughout: a constructed stream is not a measurement;
//   * no threshold is modified anywhere - the detector's defaults, the rule's 0.5 gate, the engine's
//     0.6 agreement threshold;
//   * the expected column is the MEASURED behaviour, not the hoped-for behaviour. Where the pixel proxy
//     misfires (a lighting transient alerts; smooth ~2x motion is missed) the table records that, so
//     the limitation is pinned by the suite and a future fix is a visible change rather than a
//     discovery. Each row carries its verdict: TP, TN, FP or FN.
//
// The real-camera counterpart of this matrix, with its own measured numbers, is in
// docs/PHYSICAL_CAMERA_SCENARIOS.md.

#include <catch2/catch.hpp>

#include "core/ActivityEpisode.h"
#include "core/ActivityDetectors.h"
#include "core/DetectionFusionRule.h"
#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

constexpr const char* kDetectorId = "camera-motion-activity";
constexpr const char* kModelId = "ema-baseline-v1";
constexpr const char* kSource = "/dev/video0";
// The consented capture's cadence: 600 frames in 42.604 s, one observation every 5 frames.
constexpr double kCaptureCadence = 0.356;

struct Scenario {
    std::string name;
    std::string story;              // the physical phenomenon this stream stands for
    std::string verdict;            // TP | TN | FP | FN, against the human reading of the story
    bool expect_detected;           // the detector reports at least one confirmed change
    bool expect_classic_alert;      // the default (whole-window fusion) path alerts
    bool expect_episode_alert;      // the opt-in episode path alerts
    std::vector<double> levels;
    double cadence = kCaptureCadence;   // seconds per observation
};

// --- level-shape helpers --------------------------------------------------------------------------

std::vector<double> quiet(int n, double level) { return std::vector<double>(size_t(n), level); }

void append(std::vector<double>& to, int n, double level) { to.insert(to.end(), size_t(n), level); }

std::vector<double> ramp(int n, double from, double to) {
    std::vector<double> v;
    for (int i = 0; i < n; ++i) v.push_back(from + (to - from) * (double(i) / double(n - 1)));
    return v;
}

std::vector<double> alternating(int n, double lo, double hi, int period) {
    std::vector<double> v;
    for (int i = 0; i < n; ++i) v.push_back(((i / period) % 2 == 0) ? lo : hi);
    return v;
}

std::vector<double> with_event(int pre, double pre_level, int len, double event_level, int post) {
    std::vector<double> v = quiet(pre, pre_level);
    append(v, len, event_level);
    append(v, post, pre_level);
    return v;
}

// --- measurement -----------------------------------------------------------------------------------

struct Measured {
    size_t observations = 0;
    size_t first_detected = 0;      // index of the first confirmed change (0 = none)
    std::string first_direction;
    size_t confirmed_runs = 0;      // how many separate runs of confirmed change
    size_t longest_run = 0;
    std::string classic_verdict;
    double classic_confidence = 0.0;
    bool classic_alert = false;
    double short_window_confidence = 0.0;
    bool short_window_alert = false;
    bool episode_found = false;
    size_t episode_length = 0;
    size_t episode_start = 0;
    std::string episode_direction;
    double episode_mean_confidence = 0.0;
    double episode_weakest_confidence = 0.0;
    bool episode_alert = false;
    double episode_alert_confidence = 0.0;
};

ActivityChangeDetector::Config camera_detector_config() {
    ActivityChangeDetector::Config cfg;
    cfg.detector_id = kDetectorId;
    cfg.feature = "motion-changed-fraction";
    cfg.model_id = kModelId;   // every other value stays at the detector's own default
    return cfg;
}

std::vector<Observation> observe_stream_with(const Scenario& s,
                                             const ActivityChangeDetector::Config& cfg) {
    ActivityChangeDetector detector(cfg);

    std::vector<Observation> obs;
    obs.reserve(s.levels.size());
    const auto span = std::chrono::milliseconds(int64_t(s.levels.size() * s.cadence * 1000));
    const auto t0 = std::chrono::system_clock::now() - span;
    for (size_t i = 0; i < s.levels.size(); ++i) {
        const auto at = t0 + std::chrono::milliseconds(int64_t(i * s.cadence * 1000));
        const auto result = detector.observe(s.levels[i], at);
        obs.push_back(detector.to_observation(result, "sc-" + std::to_string(i), kSource,
                                              Provenance::synthetic));
    }
    return obs;
}

std::vector<Observation> observe_stream(const Scenario& s) {
    return observe_stream_with(s, camera_detector_config());
}

std::vector<Event> as_events(const std::vector<Observation>& obs) {
    std::vector<Event> events;
    events.reserve(obs.size());
    for (const auto& o : obs) events.push_back(o.to_event());
    return events;
}

DetectionFusionRule rule(bool episode_mode) {
    DetectionFusionRule::Config cfg;
    cfg.evidence_detector_ids = {kDetectorId};
    cfg.use_episode_evidence = episode_mode;
    return DetectionFusionRule("detection-fusion", cfg);
}

Measured measure(const Scenario& scenario) {
    Measured m;
    const auto obs = observe_stream(scenario);
    m.observations = obs.size();

    size_t run = 0;
    for (size_t i = 0; i < obs.size(); ++i) {
        const bool confirmed = obs[i].status == DetectorStatus::detected && obs[i].confidence.has_value();
        if (confirmed) {
            if (m.first_detected == 0) {
                m.first_detected = i + 1;
                m.first_direction = obs[i].features.value("direction", std::string());
            }
            ++run;
            m.longest_run = std::max(m.longest_run, run);
        } else {
            if (run > 0) ++m.confirmed_runs;
            run = 0;
        }
    }
    if (run > 0) ++m.confirmed_runs;

    const auto events = as_events(obs);
    const FusionEngine engine;
    const auto fused = engine.fuse(obs);
    m.classic_verdict = fusion_verdict_to_string(fused.verdict);
    m.classic_confidence = fused.confidence;
    m.classic_alert = rule(false).evaluate(events).has_value();

    // The same evidence, judged on the most recent five seconds only: 5 / cadence observations.
    const size_t tail = std::min<size_t>(obs.size(), size_t(5.0 / scenario.cadence));
    std::vector<Observation> recent(obs.end() - long(tail), obs.end());
    const auto recent_fused = engine.fuse(recent);
    m.short_window_confidence = recent_fused.confidence;
    m.short_window_alert = rule(false).evaluate(as_events(recent)).has_value();

    if (const auto episode = ActivityEpisodeSelector().best(obs)) {
        m.episode_found = true;
        m.episode_length = episode->length;
        m.episode_start = episode->start_index;
        m.episode_direction = episode->direction;
        m.episode_mean_confidence = episode->mean_confidence;
        m.episode_weakest_confidence = episode->weakest_confidence;
    }
    if (const auto alert = rule(true).evaluate(events)) {
        m.episode_alert = true;
        m.episode_alert_confidence = alert->confidence;
    }
    return m;
}

void report(const Scenario& s, const Measured& m) {
    // Printed, not INFO(): the table is the deliverable and Catch2 only shows INFO on failure.
    std::printf("SCENARIO %-42s %-3s cad=%.3fs %s\n", s.name.c_str(), s.verdict.c_str(), s.cadence,
                s.story.c_str());
    std::printf("  detected=%s first=%zu dir='%s' runs=%zu longest=%zu\n",
                m.first_detected ? "yes" : "no", m.first_detected, m.first_direction.c_str(),
                m.confirmed_runs, m.longest_run);
    std::printf("  classic=%-13s conf=%.4f alert=%-3s | last5s conf=%.4f alert=%-3s\n",
                m.classic_verdict.c_str(), m.classic_confidence, m.classic_alert ? "YES" : "no",
                m.short_window_confidence, m.short_window_alert ? "YES" : "no");
    std::printf("  episode found=%-3s len=%-3zu dir='%s' mean=%.4f weakest=%.4f alert=%-3s conf=%.4f\n",
                m.episode_found ? "yes" : "no", m.episode_length, m.episode_direction.c_str(),
                m.episode_mean_confidence, m.episode_weakest_confidence,
                m.episode_alert ? "YES" : "no", m.episode_alert_confidence);
    std::fflush(stdout);
}

std::vector<Scenario> scenario_matrix() {
    std::vector<Scenario> all;

    all.push_back({"still_scene", "nothing moves: the quiet reference", "TN", false, false, false,
                   quiet(60, 0.010)});

    {   // a light switches on: the frame jumps for two observations, then re-baselines
        std::vector<double> v = quiet(30, 0.010);
        append(v, 2, 0.100);
        append(v, 28, 0.011);
        all.push_back({"sudden_lighting_transient", "light switched on: a two-observation jump",
                       "FP", true, false, true, v});
    }
    {   // the illumination change persists: not activity, but a sustained change in the pixel field
        all.push_back({"sustained_lighting_shift", "illumination changes and stays changed",
                       "FP", true, false, true, with_event(30, 0.010, 30, 0.100, 20)});
    }

    for (int span : {30, 60, 120}) {
        all.push_back({"gradual_day_night_" + std::to_string(span),
                       "slow illumination drift the baseline tracks", "TN", false, false, false,
                       ramp(span, 0.010, 0.030)});
    }

    all.push_back({"camera_vibration", "the mount shakes faster than the recent window", "TN",
                   false, false, false, alternating(60, 0.012, 0.020, 2)});
    all.push_back({"reflection_flicker", "a reflection flickers every observation", "TN",
                   true, false, false, alternating(60, 0.005, 0.030, 1)});

    {   // a shadow crosses the field, swelling to 3.5x and receding
        std::vector<double> v = quiet(20, 0.010);
        const auto up = ramp(12, 0.010, 0.035);
        v.insert(v.end(), up.begin(), up.end());
        const auto down = ramp(12, 0.035, 0.010);
        v.insert(v.end(), down.begin(), down.end());
        append(v, 20, 0.010);
        all.push_back({"shadow_sweep", "a shadow crosses the field and recedes", "TN",
                       true, false, false, v});
    }

    {   // a curtain moves in a draught: irregular real motion
        std::vector<double> v = quiet(20, 0.010);
        append(v, 3, 0.030);
        append(v, 2, 0.012);
        append(v, 4, 0.035);
        append(v, 2, 0.013);
        append(v, 3, 0.028);
        append(v, 20, 0.010);
        all.push_back({"curtain_movement", "a curtain moves in a draught: real motion", "FN",
                       true, false, false, v});
    }

    {   // the lens is covered and then uncovered
        std::vector<double> v = quiet(20, 0.010);
        append(v, 20, 0.001);
        append(v, 20, 0.010);
        all.push_back({"temporary_occlusion", "the view is blocked, then restored", "TN",
                       true, false, false, v});
    }

    {   // a fan blade or pendulum: three moving observations in every six
        std::vector<double> v = quiet(20, 0.010);
        for (int k = 0; k < 8; ++k) {
            append(v, 3, 0.045);
            append(v, 3, 0.010);
        }
        append(v, 10, 0.010);
        all.push_back({"repetitive_movement", "a repeating source: three moving observations in six",
                       "FN", true, false, false, v});
    }

    all.push_back({"short_animal_sized_motion",
                   "hand-sized body crosses for four observations at 1.9x the baseline", "FN",
                   false, false, false, with_event(25, 0.010, 4, 0.019, 30)});
    all.push_back({"sustained_animal_sized_motion",
                   "hand-sized body moves for twenty observations at 2.1x the baseline", "FN",
                   true, false, false, with_event(25, 0.010, 20, 0.021, 20)});
    // MEASURED LIMITATION: a four-observation event at 3.5x reaches only TWO consecutive confirmed
    // samples, because the four-sample recent window dilutes the first two and the 0.05-alpha baseline
    // climbs to meet the rest. With confirm_samples = 3 the detector therefore cannot confirm it, and
    // neither evidence path can report it. Verdict FN, and no threshold here was touched to change it.
    all.push_back({"short_high_contrast_motion",
                   "four observations at 3.5x the baseline: below the confirmation capability", "FN",
                   true, false, false, with_event(25, 0.010, 4, 0.035, 30)});
    all.push_back({"longer_high_contrast_motion",
                   "eight observations at 3.5x the baseline", "TP",
                   true, false, true, with_event(25, 0.010, 8, 0.035, 30)});


    return all;
}

}  // namespace

TEST_CASE("scenario matrix: the measured table, pinned as expectations", "[camera][scenarios]") {
    size_t tp = 0, tn = 0, fp = 0, fn = 0;
    for (const auto& s : scenario_matrix()) {
        const Measured m = measure(s);
        report(s, m);
        INFO("scenario " << s.name << " (" << s.verdict << ")");
        REQUIRE(m.observations == s.levels.size());
        REQUIRE((m.first_detected > 0) == s.expect_detected);
        REQUIRE(m.classic_alert == s.expect_classic_alert);
        REQUIRE(m.episode_alert == s.expect_episode_alert);
        if (s.verdict == "TP") ++tp;
        if (s.verdict == "TN") ++tn;
        if (s.verdict == "FP") ++fp;
        if (s.verdict == "FN") ++fn;
    }
    std::printf("MATRIX TOTALS tp=%zu tn=%zu fp=%zu fn=%zu (of %zu scenarios)\n", tp, tn, fp, fn,
                tp + tn + fp + fn);
    std::fflush(stdout);
    REQUIRE(tp + tn + fp + fn == scenario_matrix().size());
}

TEST_CASE("scenario matrix: the detector's blind spot is a smooth ~2x step, not a weak event",
          "[camera][scenarios][false-negative]") {
    // The measured surprise of this evaluation. A CLEAN step to 2.1x the baseline is not confirmed:
    // the four-sample recent window dilutes the first samples (0.0128 -> ratio 1.28, 0.0155 -> 1.55)
    // while the 0.05-alpha baseline is already climbing to meet it, so the ratio only reaches ~1.6 as
    // the baseline catches up - and the confirmation counter never sees three consecutive samples
    // above it. The recorded physical rise was confirmed six times because it was noisier (ratios
    // 1.85-2.70 in a fluctuating shape), not because it was larger.
    const auto all = scenario_matrix();
    const auto smooth = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
        return s.name == "sustained_animal_sized_motion";
    });
    REQUIRE(smooth != all.end());
    const Measured m = measure(*smooth);
    report(*smooth, m);
    REQUIRE(m.first_detected > 0);      // it is seen at least once...
    REQUIRE(m.longest_run == 1);        // ...but never confirmed for three consecutive samples
    REQUIRE_FALSE(m.episode_alert);     // so no alert, in either mode

    // The boundary is SHAPE, not merely amplitude. Twice the duration of the four-observation event -
    // eight observations at the same 3.5x - reaches five consecutive confirmed samples and does alert
    // through the episode path. So what the detector needs is a ratio that stays above the confirm
    // threshold long enough, which a rectangular four-observation event never provides.
    const auto strong = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
        return s.name == "longer_high_contrast_motion";
    });
    REQUIRE(strong != all.end());
    const Measured s2 = measure(*strong);
    report(*strong, s2);
    REQUIRE(s2.longest_run >= 3);
    REQUIRE(s2.episode_alert);
    REQUIRE_FALSE(s2.classic_alert);   // the classic path still refuses it: 5 of 65 observations

    // And the four-observation form is refused by BOTH paths, which is the limitation this row pins.
    const auto brief = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
        return s.name == "short_high_contrast_motion";
    });
    REQUIRE(brief != all.end());
    const Measured s3 = measure(*brief);
    report(*brief, s3);
    REQUIRE(s3.longest_run < 3);
    REQUIRE_FALSE(s3.episode_alert);
}

TEST_CASE("scenario matrix: the window decides the classic verdict - the measured dilution rule",
          "[camera][scenarios][window]") {
    // The recorded rise's own confidences (the six confirmed wins of the consented capture), replayed
    // and then diluted one quiet observation at a time. This is exact arithmetic on measured inputs,
    // not a hypothesis: the classic path alerts while at most ONE quiet observation shares the window
    // and refuses from two onwards, because FusionEngine computes confidence = mean * agreement.
    //
    // Generalized for this run's evidence confidence (mean 0.697): an alert needs
    // 0.697 * f^2 >= 0.5, i.e. f >= 0.85 - roughly SIX SEVEN of the window must be confirmed-change
    // evidence. A 60-second window at 2.81 observations/second can therefore never report a
    // two-second event, however clearly the detector saw it; the episode path is what recovers it.
    const double recorded[6] = {0.700, 0.795, 0.849, 0.750, 0.660, 0.425};
    const FusionEngine engine;
    DetectionFusionRule::Config cfg;
    cfg.evidence_detector_ids = {kDetectorId};

    auto window_with = [&](int quiet_count) {
        std::vector<Observation> obs;
        const auto base = std::chrono::system_clock::now() - std::chrono::seconds(10);
        int i = 0;
        for (double c : recorded) {
            Observation o;
            o.observation_id = "win-" + std::to_string(i);
            o.source = kSource;
            o.provenance = Provenance::synthetic;
            o.detector_id = kDetectorId;
            o.model_id = kModelId;
            o.status = DetectorStatus::detected;
            o.confidence = c;
            o.observation_timestamp = base + std::chrono::milliseconds(10 * i);
            o.features = nlohmann::json::object();
            o.features["direction"] = "rising";
            obs.push_back(o);
            ++i;
        }
        for (int k = 0; k < quiet_count; ++k) {
            Observation o;
            o.observation_id = "win-q" + std::to_string(k);
            o.source = kSource;
            o.provenance = Provenance::synthetic;
            o.detector_id = kDetectorId;
            o.model_id = kModelId;
            o.status = DetectorStatus::not_detected;
            o.confidence = 0.0;
            o.observation_timestamp = base + std::chrono::milliseconds(10 * i);
            o.features = nlohmann::json::object();
            o.features["direction"] = "rising";
            obs.push_back(o);
            ++i;
        }
        return obs;
    };

    for (int quiet = 0; quiet <= 3; ++quiet) {
        const auto obs = window_with(quiet);
        const auto events = as_events(obs);
        const auto fused = engine.fuse(obs);
        DetectionFusionRule classic("detection-fusion", cfg);
        const bool alerts = classic.evaluate(events).has_value();
        std::printf("WINDOW evidence=6 quiet=%d total=%zu agreement=%.4f conf=%.4f alert=%s\n", quiet,
                    obs.size(), double(6) / double(6 + quiet), fused.confidence,
                    alerts ? "YES" : "no");
        std::fflush(stdout);
        REQUIRE(alerts == (quiet <= 1));
    }
}

TEST_CASE("scenario matrix: the two lighting false positives, and why they must stay opt-in",
          "[camera][scenarios][false-positive][known-limitation]") {
    // A pixel-change proxy cannot separate "the light changed" from "something moved": both raise the
    // changed-pixel fraction. Two forms are measured, and both reach an alert.
    const auto all = scenario_matrix();
    for (const char* name : {"sudden_lighting_transient", "sustained_lighting_shift"}) {
        const auto it = std::find_if(all.begin(), all.end(), [name](const Scenario& s) {
            return s.name == name;
        });
        REQUIRE(it != all.end());
        const Measured m = measure(*it);
        report(*it, m);
        INFO("scenario " << name);
        REQUIRE(m.first_detected > 0);
        REQUIRE(m.episode_alert);
        // The classic path refuses both, so these false positives are attributable to the opt-in
        // episode path - one more reason it must stay off by default.
        REQUIRE_FALSE(m.classic_alert);
    }

    // The distinguishing signal a future mitigation would use: a lighting step is one long contiguous
    // run, whereas motion (the recorded capture, repetitive_movement) produces repeated runs.
    const auto shift = std::find_if(all.begin(), all.end(), [](const Scenario& s) {
        return s.name == "sustained_lighting_shift";
    });
    const Measured m = measure(*shift);
    REQUIRE(m.longest_run >= 10);
}

TEST_CASE("scenario matrix: the three RECORDED ambient runs, replayed exactly",
          "[camera][scenarios][recorded]") {
    // Three real captures, 600 frames and 120 observations each, ~43 s / ~25 s / ~25 s, 0 driver
    // drops, nobody deliberately doing anything, frames never stored
    // (docs/evidence/PHYSICAL_CAMERA_AMBIENT_RUN{,_2,_3}.log). The confirmed observations and their
    // confidences are the ones those logs printed; the run STRUCTURE (where the rising runs sit) is
    // taken from the sequence numbers in each log, and the settling falls carry a modelled confidence
    // because the logs record only their count.
    //
    // Replayed scalars, labelled synthetic: this shows what the pipeline WOULD do, not that a physical
    // alert was delivered. What each run's own tool output printed for the classic path is kept beside
    // the replay for comparison.
    enum Kind { kQuiet, kRise, kFall, kUnusable };
    struct Segment { Kind kind; int count; double confidence; };
    struct Run {
        std::string name;
        std::vector<Segment> segments;
        bool expect_episode;
        double expect_confidence;
        std::string tool_classic;   // what the capture tool printed for this run's own window
    };

    const std::vector<Run> runs = {
        {"run 1 (43 s): 3 confirmed rises, contiguous",
         {{kQuiet, 60, 0.0}, {kRise, 1, 0.710}, {kRise, 1, 0.710}, {kRise, 1, 0.400},
          {kQuiet, 52, 0.0}, {kUnusable, 5, 0.0}},
         true, 0.6067, "not_detected 0.015 -> no alert"},
        {"run 2 (25 s): rises at obs 9-10 and 17-19, then 41 settling falls",
         {{kQuiet, 9, 0.0}, {kRise, 1, 0.625}, {kRise, 1, 0.520}, {kQuiet, 6, 0.0},
          {kRise, 1, 0.715}, {kRise, 1, 0.630}, {kRise, 1, 0.435}, {kFall, 41, 0.10},
          {kQuiet, 54, 0.0}, {kUnusable, 5, 0.0}},
         true, 0.5933, "inconclusive 0.080 -> no alert"},
        {"run 3 (25 s): 3 confirmed rises that are individually weak",
         {{kQuiet, 31, 0.0}, {kRise, 1, 0.545}, {kRise, 1, 0.545}, {kRise, 1, 0.345},
          {kQuiet, 25, 0.0}, {kFall, 8, 0.10}, {kQuiet, 48, 0.0}, {kUnusable, 5, 0.0}},
         false, 0.0, "not_detected 0.025 -> no alert"},
    };

    for (const auto& run : runs) {
        std::vector<Observation> obs;
        const auto base = std::chrono::system_clock::now() - std::chrono::seconds(50);
        int i = 0;
        for (const auto& seg : run.segments) {
            for (int k = 0; k < seg.count; ++k) {
                Observation o;
                o.observation_id = "amb-" + std::to_string(i);
                o.source = kSource;
                o.provenance = Provenance::synthetic;   // replay
                o.detector_id = kDetectorId;
                o.model_id = kModelId;
                o.status = seg.kind == kUnusable ? DetectorStatus::inconclusive
                          : seg.kind == kQuiet    ? DetectorStatus::not_detected
                                                  : DetectorStatus::detected;
                o.confidence = seg.kind == kUnusable ? 0.0 : seg.confidence;
                o.observation_timestamp = base + std::chrono::milliseconds(205 * i);
                o.features = nlohmann::json::object();
                o.features["direction"] = seg.kind == kFall ? "falling" : "rising";
                obs.push_back(o);
                ++i;
            }
        }
        INFO("run " << run.name);
        REQUIRE(obs.size() == 120);

        const auto events = as_events(obs);
        const auto alert = rule(true).evaluate(events);
        const auto episode = ActivityEpisodeSelector().best(obs);
        std::printf("RECORDED %-58s episode=%s len=%zu conf=%.4f | tool classic: %s\n",
                    run.name.c_str(), alert.has_value() ? "ALERT" : "no alert",
                    episode ? episode->length : 0, alert ? alert->confidence : 0.0,
                    run.tool_classic.c_str());
        std::fflush(stdout);

        // The classic path refuses all three, which every run's own tool output confirms (printed
        // above): a home's ordinary ambience dilutes three to eight evidence votes into 115.
        REQUIRE_FALSE(rule(false).evaluate(events).has_value());

        if (run.expect_episode) {
            REQUIRE(alert.has_value());
            REQUIRE(alert->confidence == Approx(run.expect_confidence).margin(0.002));
            REQUIRE(alert->details["evidence_mode"] == "episode");
            REQUIRE(alert->details["provenance"] == "synthetic");   // the replay's label, honestly
        } else {
            // Run 3 is the useful negative: three contiguous confirmed rises, and the episode path
            // still refuses them because their mean confidence (0.478) is below the unchanged gate.
            REQUIRE_FALSE(alert.has_value());
            REQUIRE_FALSE(episode.has_value());
        }
    }
}
