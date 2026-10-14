#pragma once

// Activity-change and vector-drift detectors (precursor layer).
//
// Ported from `animal-calamity-precursor-detection` (SKILL.md "the pipeline" and
// references/cpp-pipeline.md section 3) with the model-dependent parts REMOVED
// rather than stubbed. What is left is pure, deterministic, bounded C++ that
// runs today and is unit-tested on the host.
//
// ADOPTED FROM THE SKILL
//   * "Smooth before you threshold. One anomalous frame is a door slamming.
//     Require k consecutive windows above threshold."  -> confirm_samples /
//     confirm_windows in both detectors.
//   * "Cosine over Euclidean for embeddings: you care about direction, not
//     magnitude."  -> VectorDriftDetector uses cosine_distance().
//   * "Keep a small ring buffer of past embeddings and flag a SUSTAINED regime
//     shift, not a spike."  -> bounded history ring + a baseline that is only
//     maintained while the state is normal.
//   * Vector-before-labels: the drift detector needs no annotated dataset, only
//     a quiet baseline window. That is why it is implementable now and a species
//     classifier is not.
//
// DEFERRED (NOT implemented here, with the reason recorded in
// docs/SKILLS_INTEGRATION_VALIDATION.md)
//   * YAMNet log-mel + 1024-d embeddings: needs ONNX Runtime plus AudioSet
//     weights (licensing/resource review not done; no ML runtime on the host).
//   * YOLOv8 / Farneback optical flow: needs OpenCV (not present).
//   * Any species/emotion/emergency claim: no defensible classifier or dataset.

#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace homeguardian {

// Compares recent activity against a learned slow baseline. This is the
// LEVEL/TREND view; statistical deviation (z-score) is deliberately NOT
// duplicated here - it stays in the existing AnomalyMonitor.
class ActivityChangeDetector {
public:
    struct Config {
        size_t warmup = 4;           // samples before a baseline exists
        size_t recent_window = 4;    // samples averaged as "recent"
        size_t history = 32;         // bounded ring size
        double slow_alpha = 0.05;    // baseline EMA weight
        double confirm_ratio = 1.6;  // recent/baseline at or above -> rising
        double fall_ratio = 0.6;     // recent/baseline at or below -> falling
        size_t confirm_samples = 3;  // consecutive samples to CONFIRM a direction
        std::string feature = "activity-level";
        std::string detector_id = "activity-change";
        std::string model_id = "ema-baseline-v1";
        Config() = default;
    };

    enum class Direction { unknown, stable, rising, falling };

    struct Result {
        bool baseline_ready = false;
        Direction direction = Direction::unknown;
        bool confirmed = false;
        double baseline_level = 0.0;
        double recent_level = 0.0;
        double ratio = 1.0;
        size_t consecutive = 0;
        size_t samples = 0;
        std::string reason;
    };

    static Config defaults() { return Config{}; }
    explicit ActivityChangeDetector(Config cfg = defaults()) : cfg_(std::move(cfg)) {}

    // Feed one AUTHORIZED, already-extracted activity level (e.g. motion
    // changed_fraction, or audio RMS). Non-finite input is refused, not
    // accumulated. `now` is injected so tests and scenarios are deterministic.
    Result observe(double value, std::chrono::system_clock::time_point now = now_default());

    Result snapshot() const;

    // warming up -> inconclusive; confirmed change -> detected; otherwise
    // not_detected. Never "detected" merely because a single sample moved.
    DetectorStatus status_for(const Result& r) const;

    Observation to_observation(const Result& r, const std::string& observation_id,
                               const std::string& source,
                               Provenance provenance = Provenance::synthetic) const;

    void reset();
    size_t sample_count() const { return samples_; }

    static std::chrono::system_clock::time_point now_default();

private:
    static const char* direction_to_string(Direction d);

    Config cfg_;
    std::vector<double> history_{};  // bounded ring, oldest first
    double baseline_ = 0.0;
    double last_value_ = 0.0;
    size_t samples_ = 0;
    Direction last_direction_ = Direction::unknown;
    size_t consecutive_ = 0;
    bool confirmed_ = false;
};

// Cosine drift of a bounded feature vector from a learned baseline centroid.
// Works on ANY bounded vector: today on hand-built feature vectors, and on an
// approved embedding model later without changing this code.
class VectorDriftDetector {
public:
    struct Config {
        size_t dim = 0;                     // 0 = adopt the first vector's dimension
        size_t warmup = 8;                  // vectors before the centroid is usable
        size_t history = 32;                // bounded ring of distances
        size_t confirm_windows = 3;         // consecutive drifted windows to confirm
        double drift_threshold = 0.15;      // acceptance target, not measured
        double recovery_threshold = 0.05;   // below this, a shift is considered over
        std::string detector_id = "vector-drift";
        std::string model_id = "unsupervised-centroid-v1";
        Config() = default;
    };

    enum class Drift { warming_up, normal, drifting, confirmed_shift, recovering };

    struct Result {
        Drift state = Drift::warming_up;
        bool baseline_ready = false;
        double distance = 0.0;
        double baseline_distance = 0.0;  // mean distance while normal
        size_t consecutive = 0;
        size_t samples = 0;
        size_t dim = 0;
        std::string reason;
    };

    static Config defaults() { return Config(); }
    explicit VectorDriftDetector(Config cfg = defaults()) : cfg_(std::move(cfg)) {}

    // Feed one bounded feature vector. Wrong-length or non-finite input is
    // refused (the previous state is preserved) rather than silently resizing.
    // `timestamp` is accepted for API symmetry and future windowing; the detector
    // itself is sample-ordered.
    Result observe(const std::vector<double>& vector,
                   std::chrono::system_clock::time_point timestamp = now_default());

    Result snapshot() const;

    // warming up -> inconclusive; confirmed shift -> detected; otherwise
    // not_detected.
    DetectorStatus status_for(const Result& r) const;

    Observation to_observation(const Result& r, const std::string& observation_id,
                               const std::string& source,
                               Provenance provenance = Provenance::synthetic) const;

    void reset();
    size_t sample_count() const { return samples_; }
    size_t dim() const { return dim_; }
    bool baseline_ready() const { return samples_ >= cfg_.warmup; }

    static std::chrono::system_clock::time_point now_default();
    static const char* drift_state_to_string(Drift d);

private:
    Config cfg_;
    size_t dim_ = 0;
    std::vector<double> centroid_{};
    std::vector<double> ring_{};     // bounded history of distances (oldest first)
    double distance_sum_ = 0.0;      // sum over ring_ entries recorded while normal
    size_t distance_count_ = 0;
    size_t samples_ = 0;
    size_t consecutive_ = 0;
    Drift state_ = Drift::warming_up;
    double last_distance_ = 0.0;
};

}  // namespace homeguardian
