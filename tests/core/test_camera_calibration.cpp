#include "catch2/catch.hpp"

#include "core/ActivityDetectors.h"
#include "core/CameraActivitySession.h"
#include "core/DetectionFusionRule.h"
#include "core/FusionEngine.h"
#include "core/Logger.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Controlled calibration analysis for the camera path.
//
// WHY THIS FILE EXISTS
//   Two measured problems stood between the camera path and an alert, and the standing rule is
//   that neither may be "fixed" by loosening a threshold:
//     1. under SUSTAINED motion the detector's confidence decays as the EMA baseline adapts, so
//        a real change can fall below the rule's 0.5 confidence gate while still being sustained;
//     2. the rule fuses over a 5-second window of observations, and low-confidence
//        (not_detected) votes dilute the fused mean.
//   This file MEASURES both instead of guessing at them, using the real detector and the real
//   FusionEngine - no re-implementation, no threshold edits. The numbers it prints are the
//   evidence for any decision that follows.
//
// WHAT IT DOES NOT DO
//   It changes nothing. No threshold, no baseline rate, no window length, no confidence formula.
//   There is no camera here either: the input is a deterministic synthetic step.
// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::system_clock;

constexpr size_t kW = 64, kH = 48;
constexpr size_t kPixels = kW * kH;

std::vector<uint8_t> plane(uint8_t v) { return std::vector<uint8_t>(kPixels, v); }

void move_block(std::vector<uint8_t>& p, int side, uint8_t delta) {
    const size_t cx = kW / 2, cy = kH / 2;
    for (size_t y = cy - side / 2; y < cy + side / 2; ++y) {
        for (size_t x = cx - side / 2; x < cx + side / 2; ++x) {
            p[y * kW + x] = static_cast<uint8_t>(p[y * kW + x] + delta);
        }
    }
}

// A step response: `quiet_windows` of small change, then `strong_windows` of large change.
// Returns, per window after the step, the observation the detector produced.
struct DecayRun {
    double peak_confidence = 0.0;
    size_t windows_above_gate = 0;   // confidence >= 0.5 (the rule's current gate)
    size_t windows_detected = 0;
    size_t first_below_gate = 0;     // 0 = never fell below after the step
};

DecayRun measure_step(double slow_alpha, size_t quiet_windows, size_t strong_windows,
                      uint8_t strong_delta) {
    CameraActivitySession::Config cfg;
    cfg.slow_alpha = slow_alpha;
    CameraActivitySession session(cfg);
    auto now = Clock::time_point(std::chrono::seconds(1000));

    auto frame = plane(100);
    uint8_t v = 0;
    for (size_t i = 0; i < quiet_windows; ++i) {
        move_block(frame, 2, static_cast<uint8_t>(1 + (v++ % 2)));
        (void)session.ingest(frame.data(), frame.size(), Provenance::synthetic, now);
        now += std::chrono::seconds(1);
    }

    DecayRun run;
    bool dropped = false;
    for (size_t i = 0; i < strong_windows; ++i) {
        move_block(frame, 40, strong_delta);
        (void)session.ingest(frame.data(), frame.size(), Provenance::synthetic, now);
        now += std::chrono::seconds(1);
        const Observation o = session.observation_for("cal-" + std::to_string(i), "analysis");
        const double c = o.confidence.value_or(0.0);
        if (o.status == DetectorStatus::detected) {
            ++run.windows_detected;
            if (c > run.peak_confidence) run.peak_confidence = c;
            if (c >= 0.5) {
                ++run.windows_above_gate;
                dropped = false;
            } else if (!dropped) {
                dropped = true;
                run.first_below_gate = i + 1;
            }
        }
    }
    return run;
}

// One observation, with the confidence the detector would attach to it.
Observation vote(DetectorStatus status, double confidence, size_t index) {
    Observation o;
    o.observation_id = "cal-vote-" + std::to_string(index);
    o.source = "analysis";
    o.provenance = Provenance::synthetic;
    o.detector_id = "camera-motion-activity";
    o.model_id = "ema-baseline-v1";
    o.status = status;
    o.confidence = confidence;
    o.observation_timestamp = Clock::now();
    o.features = nlohmann::json::object();
    return o;
}

std::string band(double v) {
    if (v >= 0.5) return ">=0.5 (would pass the current gate)";
    if (v >= 0.25) return "0.25-0.5 (below the gate)";
    return "<0.25 (far below)";
}

}  // namespace

TEST_CASE("Calibration: EMA baseline adaptation decays the confidence of a SUSTAINED change",
          "[camera][analysis][decay]") {
    Logger::initialize("error");

    struct Row { double alpha; size_t quiet; size_t strong; DecayRun run; };
    std::vector<Row> rows;
    for (double alpha : {0.05, 0.02, 0.01}) {
        for (size_t strong : {8u, 20u, 40u}) {
            rows.push_back(Row{alpha, 6, strong, measure_step(alpha, 6, strong, 60)});
        }
    }

    for (const auto& r : rows) {
        WARN("slow_alpha=" << r.alpha << " quiet=" << r.quiet << " strong=" << r.strong
                           << " -> peak_confidence=" << r.run.peak_confidence
                           << " windows_detected=" << r.run.windows_detected
                           << " windows>=0.5=" << r.run.windows_above_gate
                           << " first_below_gate_at=" << r.run.first_below_gate);
    }

    // What the measurement must show, whatever the exact numbers: a sustained change IS detected,
    // and its confidence is NOT monotonic - it decays as the baseline absorbs the new level. Those
    // two facts are the reason a naive "is the confidence above the gate?" test is the wrong
    // question for a sustained change.
    for (const auto& r : rows) {
        REQUIRE(r.run.windows_detected > 0);
        REQUIRE(r.run.peak_confidence > 0.0);
        REQUIRE(r.run.peak_confidence <= 1.0);
    }
    // A smaller alpha (slower baseline) keeps the confidence above the gate for longer: the
    // mechanism is the baseline rate, not the gate.
    const auto slow = measure_step(0.01, 6, 40, 60);
    const auto fast = measure_step(0.05, 6, 40, 60);
    WARN("above-gate windows: alpha=0.01 -> " << slow.windows_above_gate
                                              << ", alpha=0.05 -> " << fast.windows_above_gate);
    REQUIRE(slow.windows_above_gate >= fast.windows_above_gate);
}

TEST_CASE("Calibration: the 5-second fusion window dilutes the fused confidence",
          "[camera][analysis][window]") {
    Logger::initialize("error");
    FusionEngine engine;

    // The real engine, fed the real vote mix. D = detected votes at full confidence, Q = quiet
    // (not_detected, confidence 0.0) votes. This is exactly the composition question: a window
    // holding both a change and its surroundings.
    struct Row { size_t detected; size_t quiet; double fused; std::string verdict; };
    std::vector<Row> rows;
    for (size_t detected : {1u, 3u, 5u, 15u, 30u}) {
        for (size_t quiet : {0u, 4u, 10u, 30u, 60u}) {
            std::vector<Observation> obs;
            for (size_t i = 0; i < detected; ++i)
                obs.push_back(vote(DetectorStatus::detected, 1.0, i));
            for (size_t i = 0; i < quiet; ++i)
                obs.push_back(vote(DetectorStatus::not_detected, 0.0, detected + i));
            const FusionResult fr = engine.fuse(obs);
            rows.push_back(Row{detected, quiet, fr.confidence, fusion_verdict_to_string(fr.verdict)});
        }
    }
    for (const auto& r : rows) {
        WARN("detected=" << r.detected << " quiet=" << r.quiet << " -> fused=" << r.fused
                         << " (" << band(r.fused) << ") verdict=" << r.verdict);
    }

    // The dilution is monotone: adding quiet votes to the SAME evidence never raises confidence.
    for (size_t detected : {3u, 5u, 15u}) {
        double previous = 2.0;
        for (size_t quiet : {0u, 4u, 10u, 30u, 60u}) {
            std::vector<Observation> obs;
            for (size_t i = 0; i < detected; ++i)
                obs.push_back(vote(DetectorStatus::detected, 1.0, i));
            for (size_t i = 0; i < quiet; ++i)
                obs.push_back(vote(DetectorStatus::not_detected, 0.0, detected + i));
            const double fused = engine.fuse(obs).confidence;
            REQUIRE(fused <= previous + 1e-9);
            previous = fused;
        }
    }

    // The quantitative consequence worth stating: at the demo's own 5 s window with one
    // observation per frame, a one-second change sits in a window dominated by quiet votes.
    const size_t fps = 15;
    const size_t window_seconds = 5;
    const size_t per_frame_votes = fps * window_seconds;          // one observation per frame
    const size_t per_second_votes = window_seconds;               // one observation per second
    auto fused_for = [&](size_t change_seconds, size_t cadence_period) {
        const size_t detected = change_seconds * (fps / cadence_period);
        const size_t total = window_seconds * (fps / cadence_period);
        std::vector<Observation> obs;
        for (size_t i = 0; i < detected; ++i) obs.push_back(vote(DetectorStatus::detected, 1.0, i));
        for (size_t i = 0; i < total - detected; ++i)
            obs.push_back(vote(DetectorStatus::not_detected, 0.0, detected + i));
        return engine.fuse(obs).confidence;
    };
    WARN("a 1 s change in a 5 s window: per-frame observations -> fused=" << fused_for(1, 1)
         << " (" << per_frame_votes << " votes); 1/s observations -> fused=" << fused_for(1, 15)
         << " (" << per_second_votes << " votes)");
    WARN("a 5 s change in a 5 s window: per-frame observations -> fused=" << fused_for(5, 1)
         << "; 1/s observations -> fused=" << fused_for(5, 15));

    // The honest summary of the arithmetic: dilution is a function of the vote MIX, so both the
    // observation cadence and how long a change persists inside the window matter far more than
    // the gate's exact value.
    REQUIRE(fused_for(5, 1) > fused_for(1, 1));
    REQUIRE(fused_for(5, 15) > fused_for(1, 15));
}
