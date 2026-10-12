#pragma once

// AnomalyDetector: a low-cost, EXPLAINABLE statistical anomaly detector for
// HomeGuardian observations. Pure C++ standard library — no ML framework, no
// model download, no new dependency. This directly implements the
// "explainable anomaly detection" priority (configurable thresholds,
// statistical baselines, change detection, missing-observation handling).
//
// DESIGN (deliberately simple and auditable):
//  - A detector tracks a numeric feature per (source, feature) key over a
//    rolling window and maintains a running mean + standard deviation.
//  - A new sample whose |z-score| exceeds a configurable threshold is flagged
//    as an anomaly, WITH a human-readable reason (which is what "explainable"
//    means here: every flag carries the baseline, the sample, the z-score, and
//    the threshold that produced it).
//  - "Missing observation" handling: if no sample arrives for a key within a
//    configurable max-gap, the detector reports a `missing` status — so a
//    sensor going quiet is surfaced, not silently ignored.
//  - A warm-up period: the first `min_samples` observations only build the
//    baseline and never flag (avoids false positives on cold start).
//  - Bounded memory: each key keeps at most `max_window` recent samples
//    (ring eviction); the number of tracked keys is bounded by `max_keys`
//    (oldest-evicted).
//
// WHAT IT IS NOT:
//  - NOT proof of a fall, illness, emergency, or dangerous behaviour. It
//    surfaces statistical deviations as CANDIDATES FOR REVIEW. The caller
//    decides what a deviation means; this class makes no medical, safety, or
//    behavioural claim.
//  - NOT a behavioural profile builder and NOT an authorization learner. It
//    stores only bounded numeric baselines, never identities or media.

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian {

enum class AnomalyKind {
    none,        // within baseline
    high,        // value significantly above baseline
    low,         // value significantly below baseline
    missing      // no observation within the expected gap
};

inline const char* anomaly_kind_to_string(AnomalyKind k) {
    switch (k) {
        case AnomalyKind::none: return "none";
        case AnomalyKind::high: return "high";
        case AnomalyKind::low: return "low";
        case AnomalyKind::missing: return "missing";
    }
    return "unknown";
}

// The result of evaluating one sample (or a missing-check). Always carries an
// explanation so a flag can be audited.
struct AnomalyResult {
    bool anomalous = false;
    AnomalyKind kind = AnomalyKind::none;
    double value = 0.0;         // the sample that was evaluated
    double mean = 0.0;          // baseline mean at evaluation time
    double stddev = 0.0;        // baseline stddev at evaluation time
    double zscore = 0.0;        // (value - mean) / stddev (0 if stddev==0)
    size_t sample_count = 0;    // observations seen for this key
    std::string reason;         // human-readable, bounded explanation
};

class AnomalyDetector {
public:
    struct Config {
        double zscore_threshold = 3.0;      // flag when |z| exceeds this
        size_t min_samples = 8;             // warm-up: build baseline, never flag
        size_t max_window = 64;             // per-key rolling window (bounded)
        size_t max_keys = 256;              // total tracked keys (bounded)
        std::chrono::seconds max_gap{3600}; // missing-observation threshold
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    // Note: the default argument calls defaults() rather than using Config{}
    // inline, because a nested type's default member initializers are not yet
    // usable in the enclosing class's own default argument.
    explicit AnomalyDetector(Config cfg = defaults()) : cfg_(cfg) {}

    // Record a sample for `key` and return whether it is anomalous (with an
    // explanation). `now` is injected for deterministic tests.
    AnomalyResult record(const std::string& key, double value,
                         std::chrono::system_clock::time_point now =
                             std::chrono::system_clock::now());

    // Check whether `key` has gone quiet (no sample within max_gap of `now`).
    // Returns a `missing` result if so; otherwise a non-anomalous result.
    // Only meaningful for keys already seen at least once.
    AnomalyResult check_missing(const std::string& key,
                                std::chrono::system_clock::time_point now =
                                    std::chrono::system_clock::now());

    // Introspection (no sensitive data).
    size_t tracked_keys() const;
    size_t sample_count(const std::string& key) const;

    void reset();

private:
    struct KeyState {
        std::deque<double> window;   // bounded recent samples
        double sum = 0.0;
        double sum_sq = 0.0;
        std::chrono::system_clock::time_point last_seen{};
        uint64_t order = 0;          // insertion order for LRU eviction
    };

    AnomalyResult evaluate(KeyState& st, double value) const;
    void evict_if_needed();

    Config cfg_;
    mutable std::mutex m_;
    std::unordered_map<std::string, KeyState> state_;
    std::vector<std::string> insertion_order_;  // for bounded eviction
    uint64_t next_order_ = 0;
};

}  // namespace homeguardian
