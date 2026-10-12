#include "SyntheticActivityDetector.h"

#include <sstream>

namespace homeguardian {

Observation SyntheticActivityDetector::detect(const std::string& id, double energy,
                                              std::chrono::system_clock::time_point now) const {
    Observation o;
    o.observation_id = id;
    o.source = cfg_.source;
    o.provenance = Provenance::synthetic;
    o.detector_id = cfg_.detector_id;
    o.model_id = cfg_.model_id;
    o.schema_version = 1;
    o.observation_timestamp = now;

    // Clamp the input into [0,1]; an out-of-range input is a detector-quality
    // problem, not a detection.
    bool out_of_range = energy < 0.0 || energy > 1.0;
    double clamped = energy < 0.0 ? 0.0 : (energy > 1.0 ? 1.0 : energy);

    o.features = {
        {"energy", clamped},
        {"detect_threshold", cfg_.detect_threshold}
    };

    if (out_of_range) {
        o.status = DetectorStatus::inconclusive;
        o.confidence = 0.0;
        o.quality = 0.0;
        std::ostringstream os;
        os << "input energy " << energy << " out of [0,1]; clamped, inconclusive";
        o.note = os.str();
        return o;
    }

    // Quality heuristic: inputs very near the threshold are low-confidence.
    double dist = clamped - cfg_.detect_threshold;
    if (dist < 0.0) dist = -dist;
    double quality = dist >= 0.3 ? 1.0 : (dist / 0.3);
    o.quality = quality;

    if (quality < cfg_.min_quality) {
        o.status = DetectorStatus::inconclusive;
        o.confidence = quality;
        o.note = "input near threshold; low quality, inconclusive";
        return o;
    }

    if (clamped >= cfg_.detect_threshold) {
        o.status = DetectorStatus::detected;
        o.confidence = quality;
        o.note = "energy at/above threshold; activity detected (synthetic)";
    } else {
        o.status = DetectorStatus::not_detected;
        o.confidence = quality;
        o.note = "energy below threshold; no activity (synthetic)";
    }
    return o;
}

}  // namespace homeguardian
