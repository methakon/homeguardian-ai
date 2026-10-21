#pragma once

// AnomalyMonitor: integrates the statistical AnomalyDetector into the synthetic
// observation pipeline (roadmap L-12).
//
// WHY: AnomalyDetector learns a baseline for a bounded numeric feature and
// flags deviations. Alone it is a library; this monitor gives it a pipeline
// role: it is fed the feature of each AUTHORIZED synthetic observation, and each
// result is turned into a detection Observation that flows through the SAME
// event pipeline (validation -> history -> detection rule -> alert policy).
//
// WHAT IT IS NOT: a statistical deviation is NOT a fall, an emergency, a
// medical event, or a danger claim. The monitor reports "this value deviates
// from the learned baseline" and nothing more.
//
// EVIDENCE SEMANTICS (important): the anomaly detector's observations do NOT
// vote in the detection/fusion rule by default. DetectionFusionRule holds an
// explicit evidence allowlist whose default is the activity detector only, so
// anomaly evidence is ADVISORY: it is reported, audited and persisted, but it
// does not influence whether an alert is raised. Adding "activity-anomaly" to
// that allowlist is the explicit, tested way to make it influence alerts.
//
// The monitor never carries raw media, identities or profiles: only the bounded
// numeric feature, the learned mean/stddev, the z-score and a short reason.

#include "core/AnomalyDetector.h"
#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace homeguardian {

// Bounded, non-sensitive summary of the monitor (safe for the API/dashboard).
struct AnomalyStatus {
    bool ran = false;                 // the anomaly stage executed this run
    bool baseline_ready = false;      // enough samples to evaluate
    size_t sample_count = 0;          // samples in the current baseline window
    size_t min_samples = 0;           // warm-up requirement
    double baseline_mean = 0.0;
    double baseline_stddev = 0.0;
    std::string phase;                // "warm-up" | "ready"
    std::string status;               // warm-up|normal|high|low|missing
    bool deviation = false;           // an anomaly was flagged
    double last_value = 0.0;
    double last_zscore = 0.0;
    size_t anomalies = 0;             // anomalies flagged in this run
    std::string reason;               // bounded explanation from the detector
    bool missing_checked = false;     // a deliberate gap evaluation was run
    bool missing = false;             // the feature had gone quiet
    std::string missing_reason;       // bounded explanation for the gap
};

class AnomalyMonitor {
public:
    struct Config {
        double zscore_threshold = 2.0;   // flag when |z| exceeds this
        size_t min_samples = 4;          // warm-up (baseline built, never flags)
        size_t max_window = 32;          // bounded per-key rolling window
        std::chrono::seconds max_gap{3}; // missing-observation threshold
        std::string feature = "activity-energy";
        std::string detector_id = "activity-anomaly";
        std::string key = "activity-energy";
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    explicit AnomalyMonitor(Config cfg = defaults()) : detector_(make_detector_config(cfg)), cfg_(std::move(cfg)) {}

    // Feed the bounded numeric feature of one AUTHORIZED observation. `now` is
    // injected so tests and scenarios are deterministic.
    AnomalyResult observe(double value,
                          std::chrono::system_clock::time_point now =
                              std::chrono::system_clock::now());

    // Deliberate gap evaluation: reports `missing` when no sample arrived for
    // the feature within max_gap of `now`.
    AnomalyResult check_missing(std::chrono::system_clock::time_point now =
                                    std::chrono::system_clock::now());

    // Bounded, non-sensitive summary.
    AnomalyStatus snapshot() const;

    // Map a detector result to a detection Observation status, EXPLICITLY:
    //   warm-up (baseline not ready) -> inconclusive   (never a vote)
    //   missing (no sample)          -> inconclusive   (never a vote)
    //   within baseline              -> not_detected
    //   high / low deviation         -> detected       (the detector's target
    //                                    IS "an anomaly", so a flag is a
    //                                    positive result for THIS detector)
    static DetectorStatus status_for(const AnomalyResult& r, size_t min_samples);

    // Build the detection Observation for the pipeline (payload carries the
    // provenance, detector id, status, z-score/mean/stddev and the reason).
    Observation to_observation(const AnomalyResult& r, const std::string& observation_id,
                               std::chrono::system_clock::time_point now,
                               size_t min_samples) const;

    // Clear the learned baseline. Called at the start of every scenario run so
    // each run establishes its own baseline deterministically (documented
    // baseline-reset semantics; no cross-run drift).
    void reset();

    const Config& config() const { return cfg_; }
    size_t sample_count() const { return samples_; }

private:
    static AnomalyDetector::Config make_detector_config(const Config& c) {
        AnomalyDetector::Config d;
        d.zscore_threshold = c.zscore_threshold;
        d.min_samples = c.min_samples;
        d.max_window = c.max_window;
        d.max_gap = c.max_gap;
        return d;
    }

    AnomalyDetector detector_;
    Config cfg_;
    size_t samples_ = 0;
    size_t anomalies_ = 0;
    AnomalyResult last_{};          // last VALUE evaluation (drives the baseline)
    bool missing_checked_ = false;  // a deliberate gap evaluation was run
    AnomalyResult missing_{};       // its result (kept separate from last_)
};

}  // namespace homeguardian
