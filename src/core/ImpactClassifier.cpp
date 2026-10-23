#include "core/ImpactClassifier.h"

#include <cmath>
#include <sstream>

namespace homeguardian {

ImpactResult SyntheticImpactClassifier::classify(double intensity) {
    ImpactResult r;
    r.value = intensity;
    ++samples_;

    if (!std::isfinite(intensity) || intensity < 0.0 || intensity > 1.0) {
        // Out-of-range input is a malformed sample, not evidence.
        r.status = DetectorStatus::not_detected;
        r.phase = "idle";
        std::ostringstream os;
        os << "input " << intensity << " outside [0,1]; not evaluated as impact evidence";
        r.reason = os.str();
        return r;
    }

    // Count how long the pending transient has been waiting (before this sample
    // is interpreted), so the window is measured in samples.
    if (transient_pending_) ++samples_since_transient_;

    // Phase 2: stillness while a transient is pending -> the pattern completes.
    if (transient_pending_ && intensity <= cfg_.stillness_threshold &&
        samples_since_transient_ <= cfg_.max_window) {
        r.status = DetectorStatus::detected;
        r.phase = "confirmed";
        r.transient_value = transient_value_;
        r.stillness_value = intensity;
        r.samples_since_transient = samples_since_transient_;
        ++confirmations_;
        std::ostringstream os;
        os << "simulated impact signature: transient " << transient_value_
           << " followed by stillness " << intensity << " within "
           << samples_since_transient_ << " sample(s) (synthetic pattern; not a fall)";
        r.reason = os.str();
        transient_pending_ = false;
        transient_value_ = 0.0;
        samples_since_transient_ = 0;
        return r;
    }

    // Phase 1: a new transient (restarts the pending pattern if one was open).
    if (intensity >= cfg_.transient_threshold) {
        transient_pending_ = true;
        transient_value_ = intensity;
        samples_since_transient_ = 0;
        r.status = DetectorStatus::inconclusive;   // NOT a confirmation on its own
        r.phase = "transient";
        r.transient_value = intensity;
        std::ostringstream os;
        os << "single high-intensity sample " << intensity << " (>= "
           << cfg_.transient_threshold
           << "); awaiting supporting stillness within " << cfg_.max_window
           << " sample(s) — not confirmed";
        r.reason = os.str();
        return r;
    }

    // A pending transient that has now expired.
    if (transient_pending_ && samples_since_transient_ > cfg_.max_window) {
        transient_pending_ = false;
        r.status = DetectorStatus::not_detected;
        r.phase = "expired";
        std::ostringstream os;
        os << "transient " << transient_value_ << " not followed by stillness within "
           << cfg_.max_window << " sample(s); not confirmed";
        r.reason = os.str();
        transient_value_ = 0.0;
        samples_since_transient_ = 0;
        return r;
    }

    r.status = DetectorStatus::not_detected;
    r.phase = transient_pending_ ? "transient" : "idle";
    r.reason = transient_pending_
                   ? "within the pending transient window; no confirming stillness yet"
                   : "no impact transient";
    return r;
}

Observation SyntheticImpactClassifier::to_observation(
    const ImpactResult& r, const std::string& id,
    std::chrono::system_clock::time_point now) const {
    Observation o;
    o.observation_id = id;
    o.source = "sim_sensor";
    o.provenance = Provenance::synthetic;
    o.detector_id = cfg_.detector_id;
    o.model_id = "synthetic-impact-pattern-v1";
    o.schema_version = 1;
    o.observation_timestamp = now;
    o.status = r.status;

    // Confidence is derived only from how far the transient exceeded the
    // threshold and how quickly stillness followed. It is not a safety score.
    if (r.status == DetectorStatus::detected && cfg_.transient_threshold < 1.0) {
        const double over = (r.transient_value - cfg_.transient_threshold) /
                            (1.0 - cfg_.transient_threshold);
        const double speed = 1.0 - (static_cast<double>(r.samples_since_transient) /
                                    static_cast<double>(cfg_.max_window + 1));
        o.confidence = std::min(1.0, std::max(0.0, 0.5 * std::max(0.0, over) + 0.5 * speed));
    } else {
        o.confidence = 0.0;
    }
    o.quality = r.status == DetectorStatus::detected ? 1.0 : 0.0;

    o.features = {
        {"feature", cfg_.feature},
        {"value", r.value},
        {"phase", r.phase},
        {"transient_value", r.transient_value},
        {"stillness_value", r.stillness_value},
        {"samples_since_transient", static_cast<uint64_t>(r.samples_since_transient)},
        {"transient_threshold", cfg_.transient_threshold},
        {"stillness_threshold", cfg_.stillness_threshold},
    };

    std::string note = "synthetic impact classifier (" + r.phase + "): " + r.reason +
                       " | synthetic demonstration only; this is not fall detection and "
                       "no emergency service is involved";
    if (note.size() > Observation::kMaxNoteChars) note.resize(Observation::kMaxNoteChars);
    o.note = note;

    o.validate();
    return o;
}

void SyntheticImpactClassifier::reset() {
    transient_pending_ = false;
    transient_value_ = 0.0;
    samples_since_transient_ = 0;
    samples_ = 0;
    confirmations_ = 0;
}

}  // namespace homeguardian
