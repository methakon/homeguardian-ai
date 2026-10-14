#pragma once

// Environmental correlation adapter (precursor layer, Category B evidence).
//
// WHY: the precursor skill's most defensible environmental channels are
// barometric pressure, humidity and temperature (the ant/amphibian behaviour it
// discusses reads as pressure+humidity sensitivity). HomeGuardian must be able
// to ACCEPT an authorized environmental measurement from a real source (a Home
// Assistant entity, a node sensor) and associate it with a detected change in
// the same time window.
//
// EVIDENCE GRADE (from the skill's own correction ledger): this is Category B -
// "plausible but incompletely validated correlation". Therefore:
//   * a correlation is an OBSERVATION, never an alert and never a prediction;
//   * the result vocabulary is `environmental_correlation` or
//     `insufficient_evidence`, never "flood_will_occur";
//   * the correlation output records the LAG and the pairing it used, so a human
//     can audit whether the claim is meaningful;
//   * correlation is stated as correlation, not causation.
//
// The adapter validates aggressively: metric allowlist, absolute plausibility
// ranges (a humidity of 100000% is a broken sensor, not a finding), bounded
// strings, timestamps that are not far in the future, and an explicit
// authorization context. An unauthorized sample is NOT ingested - there is no
// path by which a measurement bypasses the consent model.

#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

struct EnvironmentalSample {
    std::string source;   // device / entity id (bounded)
    std::string metric;   // temperature | humidity | pressure | illuminance
    double value = 0.0;
    std::string unit;     // "C" | "%" | "hPa" | "lux" (documentation only)
    std::chrono::system_clock::time_point observed_at{};
    Provenance provenance = Provenance::synthetic;
    // Explicit authorization context, e.g.
    // "subject=node:hg-1;purpose=environment_monitor;decision=granted".
    std::string authorization;
    std::string note;     // bounded, non-sensitive
};

struct EnvironmentalBounds {
    static constexpr size_t kMaxMetricChars = 32;
    static constexpr size_t kMaxSourceChars = 96;
    static constexpr size_t kMaxUnitChars = 16;
    static constexpr size_t kMaxNoteChars = 160;
    static constexpr size_t kMaxAuthorizationChars = 160;

    // Allowlisted metrics only. An unknown metric is refused, not guessed.
    static bool metric_supported(const std::string& metric);

    // Absolute plausibility range for a metric. Values outside it are treated as
    // sensor faults and refused.
    static bool plausible(const std::string& metric, double value);
};

class EnvironmentalAdapter {
public:
    struct Config {
        std::chrono::seconds max_future_skew{120};  // ingestion earlier than observed -> refuse
        Config() = default;
    };

    static Config defaults() { return Config{}; }
    explicit EnvironmentalAdapter(Config cfg = defaults()) : cfg_(cfg) {}

    // Fail-closed validation. Returns false with a bounded reason.
    bool validate(const EnvironmentalSample& sample,
                  std::chrono::system_clock::time_point ingestion_now, std::string* reason) const;

    // Authorized + valid -> a bounded observation. Otherwise std::nullopt with
    // the reason; nothing is ingested.
    std::optional<Observation> to_observation(const EnvironmentalSample& sample,
                                              const std::string& observation_id,
                                              std::chrono::system_clock::time_point ingestion_now) const;

private:
    Config cfg_;
};

// Time-window association between detected changes and environmental samples.
class CorrelationWindow {
public:
    struct Config {
        std::chrono::seconds window{600};    // how far back evidence is considered
        std::chrono::seconds max_lag{300};   // maximum |change - sample| for a correlation
        size_t max_changes = 64;             // bounded
        size_t max_samples = 64;             // bounded
        Config() = default;
    };

    struct ChangeRecord {
        std::string source;
        std::string kind;  // e.g. "activity_change", "audio_deviation"
        std::chrono::system_clock::time_point at{};
    };

    struct Result {
        bool correlated = false;
        std::string status;  // "environmental_correlation" | "insufficient_evidence"
        size_t change_count = 0;
        size_t environmental_count = 0;
        long long lag_seconds = 0;   // signed: sample_time - change_time
        std::string metric;
        double metric_value = 0.0;
        std::string reason;
    };

    static Config defaults() { return Config{}; }
    explicit CorrelationWindow(Config cfg = defaults()) : cfg_(cfg) {}

    void add_change(const ChangeRecord& record);
    void add_sample(const EnvironmentalSample& sample);

    // Evaluate at `now`; entries older than window are ignored (and pruned).
    Result correlate(std::chrono::system_clock::time_point now);

    // Observation for the pipeline: `detector_id = "environmental-correlation"`,
    // status detected only when a correlation was found.
    Observation to_observation(const Result& result, const std::string& observation_id,
                               const std::string& source,
                               Provenance provenance = Provenance::synthetic,
                               std::chrono::system_clock::time_point now =
                                   std::chrono::system_clock::now()) const;

    size_t change_count() const { return changes_.size(); }
    size_t sample_count() const { return samples_.size(); }
    void clear();

private:
    Config cfg_;
    std::vector<ChangeRecord> changes_{};
    std::vector<EnvironmentalSample> samples_{};
};

}  // namespace homeguardian
