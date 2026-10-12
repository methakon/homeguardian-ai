#pragma once

// SyntheticActivityDetector: a small, DETERMINISTIC, SYNTHETIC detector that
// produces Observations using the framework in Observation.h. It exists to
// (a) prove the observation model + pipeline integration works end-to-end with
// real (synthetic) inference, and (b) feed the anomaly/fusion layers in tests
// and the web demo.
//
// It is NOT a real-world detector. It computes a bounded "activity energy"
// score from a numeric input (which a real backend would derive from pixel or
// audio energy) and classifies it against configurable thresholds into a
// DetectorStatus. Everything it emits is Provenance::synthetic.
//
// It does NOT read a camera, microphone, or any file. Its input is a plain
// number supplied by the caller (in tests/demo) or derived from existing
// synthetic simulator state.

#include "core/Observation.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace homeguardian {

class SyntheticActivityDetector {
public:
    struct Config {
        std::string detector_id = "activity-energy";
        std::string model_id = "synthetic-v1";
        std::string source = "sim_sensor";
        double detect_threshold = 0.5;     // >= this => detected
        double min_quality = 0.3;          // below this => inconclusive
        Config() = default;                // explicit: usable in a default arg
    };

    static Config defaults() { return Config{}; }

    explicit SyntheticActivityDetector(Config cfg = defaults()) : cfg_(cfg) {}

    // Produce an Observation from a bounded input energy in [0,1]. If the input
    // is out of range or quality is too low, the result is inconclusive — never
    // a fabricated detection. `id` is a caller-supplied unique id.
    Observation detect(const std::string& id, double energy,
                       std::chrono::system_clock::time_point now =
                           std::chrono::system_clock::now()) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace homeguardian
