#pragma once

// SyntheticImpactClassifier: a small, DETERMINISTIC classifier for a SIMULATED
// impact-like signature.
//
// THIS IS NOT FALL DETECTION. Roadmap H-01 (real pose-based fall detection) is
// NOT implemented: there is no camera, no pose estimation, no model and no
// biometric data anywhere in this build. H-01s is a synthetic demonstration of
// severity prioritisation, and a positive result here means only "the synthetic
// input followed the pattern this classifier looks for".
//
// DOCUMENTED SYNTHETIC SEMANTICS — the pattern is a transient followed by
// stillness:
//   phase 1 (transient): an intensity sample >= transient_threshold (0.80)
//                        -> the sample is NOT a confirmation on its own
//                           (status inconclusive: "single transient, awaiting
//                            supporting evidence")
//   phase 2 (stillness): a following sample <= stillness_threshold (0.25)
//                        within `max_window` (4) samples of the transient
//                        -> status detected (the two-phase pattern is complete)
//   anything else        -> status not_detected
//   a transient that expires without phase 2 -> not_detected (reason given)
//
// Every sample yields an Observation (detector_id "impact-signal") carrying the
// bounded numeric evidence and a human-readable reason, so the resulting alert
// can explain exactly which synthetic evidence produced it.
//
// Bounded: two numbers and a small counter of samples; no history beyond the
// current pending transient.

#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace homeguardian {

// The classifier's per-sample verdict (bounded, non-sensitive).
struct ImpactResult {
    DetectorStatus status = DetectorStatus::not_detected;
    std::string phase;              // "idle" | "transient" | "confirmed" | "expired"
    double value = 0.0;             // the sample that was classified
    double transient_value = 0.0;   // the pending/confirming transient
    double stillness_value = 0.0;   // the sample that completed the pattern
    size_t samples_since_transient = 0;
    std::string reason;             // bounded explanation
};

class SyntheticImpactClassifier {
public:
    struct Config {
        double transient_threshold = 0.80;  // >= this starts phase 1
        double stillness_threshold = 0.25;  // <= this completes phase 2
        size_t max_window = 4;              // samples allowed between phases
        std::string detector_id = "impact-signal";
        std::string feature = "impact-intensity";
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    explicit SyntheticImpactClassifier(Config cfg = defaults()) : cfg_(std::move(cfg)) {}

    // Classify one bounded intensity sample in [0,1]. Values outside [0,1] are
    // clamped but reported as not_detected (a malformed input is not evidence).
    ImpactResult classify(double intensity);

    // Build the detection Observation for the pipeline. `id` must be unique.
    Observation to_observation(const ImpactResult& r, const std::string& id,
                               std::chrono::system_clock::time_point now) const;

    // Introspection (bounded counters, no sensitive data).
    size_t samples() const { return samples_; }
    size_t confirmations() const { return confirmations_; }
    bool pending_transient() const { return transient_pending_; }

    // Clear all state (called per scenario run for deterministic results).
    void reset();

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    bool transient_pending_ = false;
    double transient_value_ = 0.0;
    size_t samples_since_transient_ = 0;
    size_t samples_ = 0;
    size_t confirmations_ = 0;
};

}  // namespace homeguardian
