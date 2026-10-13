#include "core/AnomalyMonitor.h"

#include <algorithm>
#include <cmath>

namespace homeguardian {

AnomalyResult AnomalyMonitor::observe(double value,
                                      std::chrono::system_clock::time_point now) {
    AnomalyResult r = detector_.record(cfg_.key, value, now);
    ++samples_;
    if (r.anomalous) ++anomalies_;
    last_ = r;
    return r;
}

AnomalyResult AnomalyMonitor::check_missing(std::chrono::system_clock::time_point now) {
    AnomalyResult r = detector_.check_missing(cfg_.key, now);
    if (r.anomalous) ++anomalies_;
    // Kept separate from `last_` so the gap probe does not erase the value
    // baseline the dashboard reports.
    missing_checked_ = true;
    missing_ = r;
    return r;
}

DetectorStatus AnomalyMonitor::status_for(const AnomalyResult& r, size_t min_samples) {
    // No sample was evaluated (warm-up, or a deliberate missing-check): this is
    // never a detection and never a vote.
    if (r.kind == AnomalyKind::missing) return DetectorStatus::inconclusive;
    if (r.sample_count < min_samples) return DetectorStatus::inconclusive;
    if (!r.anomalous) return DetectorStatus::not_detected;
    return DetectorStatus::detected;   // an anomaly WAS detected (not a danger claim)
}

Observation AnomalyMonitor::to_observation(const AnomalyResult& r,
                                           const std::string& observation_id,
                                           std::chrono::system_clock::time_point now,
                                           size_t min_samples) const {
    Observation o;
    o.observation_id = observation_id;
    o.source = "sim_sensor";
    o.provenance = Provenance::synthetic;
    o.detector_id = cfg_.detector_id;
    o.model_id = "statistical-baseline-v1";
    o.schema_version = 1;
    o.observation_timestamp = now;
    o.status = status_for(r, min_samples);

    // Confidence is derived ONLY from how far the sample is from the baseline
    // relative to the threshold; it is not a safety confidence.
    if (r.sample_count >= min_samples && cfg_.zscore_threshold > 0.0) {
        const double ratio = std::fabs(r.zscore) / (2.0 * cfg_.zscore_threshold);
        o.confidence = std::min(1.0, std::max(0.0, ratio));
    } else {
        o.confidence = 0.0;
    }
    o.quality = r.anomalous ? 1.0 : (r.sample_count >= min_samples ? 0.8 : 0.0);

    o.features = {
        {"feature", cfg_.feature},
        {"value", r.value},
        {"baseline_mean", r.mean},
        {"baseline_stddev", r.stddev},
        {"zscore", r.zscore},
        {"samples", static_cast<uint64_t>(r.sample_count)},
        {"zscore_threshold", cfg_.zscore_threshold},
    };

    // Bounded, non-sensitive explanation. States plainly what it is (and is not).
    std::string note;
    if (r.kind == AnomalyKind::missing) {
        note = "missing observation: " + r.reason;
    } else if (r.sample_count < min_samples) {
        note = "statistical baseline warming up: " + r.reason;
    } else if (r.anomalous) {
        note = "statistical deviation (" + std::string(anomaly_kind_to_string(r.kind)) +
               ") relative to the learned baseline; not a safety verdict: " + r.reason;
    } else {
        note = "within the learned statistical baseline: " + r.reason;
    }
    if (note.size() > Observation::kMaxNoteChars) note.resize(Observation::kMaxNoteChars);
    o.note = note;

    o.validate();
    return o;
}

AnomalyStatus AnomalyMonitor::snapshot() const {
    AnomalyStatus s;
    s.ran = true;
    s.min_samples = cfg_.min_samples;
    s.sample_count = last_.sample_count;
    s.baseline_mean = last_.mean;
    s.baseline_stddev = last_.stddev;
    s.baseline_ready = last_.sample_count >= cfg_.min_samples;
    s.phase = s.baseline_ready ? "ready" : "warm-up";
    s.last_value = last_.value;
    s.last_zscore = last_.zscore;
    s.anomalies = anomalies_;
    s.reason = last_.reason;

    s.missing_checked = missing_checked_;
    s.missing = missing_checked_ && missing_.anomalous;
    s.missing_reason = missing_checked_ ? missing_.reason : std::string();
    if (s.missing) {
        s.status = "missing";
        s.deviation = true;
    } else if (!s.baseline_ready) {
        s.status = "warm-up";
        s.deviation = false;
    } else if (last_.anomalous) {
        s.status = anomaly_kind_to_string(last_.kind);   // "high" | "low"
        s.deviation = true;
    } else {
        s.status = "normal";
        s.deviation = false;
    }
    return s;
}

void AnomalyMonitor::reset() {
    detector_.reset();
    samples_ = 0;
    anomalies_ = 0;
    last_ = AnomalyResult{};
}

}  // namespace homeguardian
