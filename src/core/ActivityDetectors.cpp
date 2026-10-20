#include "ActivityDetectors.h"

#include "core/BoundedFeatures.h"

#include <cmath>

namespace homeguardian {

// ---------------------------------------------------------------------------
// ActivityChangeDetector
// ---------------------------------------------------------------------------

std::chrono::system_clock::time_point ActivityChangeDetector::now_default() {
    return std::chrono::system_clock::now();
}

const char* ActivityChangeDetector::direction_to_string(Direction d) {
    switch (d) {
        case Direction::stable: return "stable";
        case Direction::rising: return "rising";
        case Direction::falling: return "falling";
        case Direction::unknown: return "unknown";
    }
    return "unknown";
}

void ActivityChangeDetector::reset() {
    history_.clear();
    baseline_ = 0.0;
    last_value_ = 0.0;
    samples_ = 0;
    last_direction_ = Direction::unknown;
    consecutive_ = 0;
    confirmed_ = false;
}

ActivityChangeDetector::Result ActivityChangeDetector::snapshot() const {
    // The snapshot is the last OBSERVED result, not a re-derivation. Reporting `ratio` as the
    // raw last value (as this did) let a caller print "sustained increase" beside a ratio
    // below 1 - a self-contradicting finding a reviewer would rightly distrust.
    if (have_result_) return last_result_;
    Result r;
    r.baseline_ready = samples_ >= cfg_.warmup;
    r.direction = last_direction_;
    r.confirmed = confirmed_;
    r.baseline_level = baseline_;
    r.recent_level = last_value_;
    r.ratio = 1.0;
    r.consecutive = consecutive_;
    r.samples = samples_;
    return r;
}

ActivityChangeDetector::Result ActivityChangeDetector::observe(
    double value, std::chrono::system_clock::time_point /*now*/) {
    Result r;
    r.samples = samples_;

    if (!std::isfinite(value)) {
        r.reason = "non-finite activity value refused";
        r.baseline_ready = samples_ >= cfg_.warmup;
        r.direction = last_direction_;
        r.confirmed = confirmed_;
        r.baseline_level = baseline_;
        r.recent_level = last_value_;
        r.ratio = 1.0;
        last_result_ = r;
        have_result_ = true;
        return r;
    }

    const double clamped = value < 0.0 ? 0.0 : value;

    // Bounded ring of recent values.
    history_.push_back(clamped);
    if (history_.size() > cfg_.history) history_.erase(history_.begin());
    ++samples_;
    last_value_ = clamped;

    // Baseline: EMA over the whole stream, seeded with the first sample.
    if (samples_ == 1) {
        baseline_ = clamped;
    } else {
        baseline_ = (1.0 - cfg_.slow_alpha) * baseline_ + cfg_.slow_alpha * clamped;
    }

    // Recent level: mean of the last min(recent_window, available) samples.
    const size_t window = std::min(cfg_.recent_window, history_.size());
    const size_t start = history_.size() - window;
    double sum = 0.0;
    for (size_t i = start; i < history_.size(); ++i) sum += history_[i];
    const double recent = window > 0 ? sum / static_cast<double>(window) : clamped;

    r.baseline_ready = samples_ >= cfg_.warmup;
    r.baseline_level = baseline_;
    r.recent_level = recent;
    r.samples = samples_;

    // Ratio against the baseline. An all-zero baseline with non-zero recent
    // activity is an unbounded ratio; report a large finite number rather than
    // infinity so the value stays serialisable.
    r.ratio = baseline_ > 1e-9 ? recent / baseline_ : (recent > 1e-9 ? 1e6 : 1.0);

    if (!r.baseline_ready) {
        r.direction = Direction::unknown;
        r.confirmed = false;
        r.reason = "warming up (" + std::to_string(samples_) + " of " +
                   std::to_string(cfg_.warmup) + " samples)";
        last_direction_ = r.direction;
        consecutive_ = 0;
        confirmed_ = false;
        last_result_ = r;
        have_result_ = true;
        return r;
    }

    Direction direction = Direction::stable;
    if (r.ratio >= cfg_.confirm_ratio) {
        direction = Direction::rising;
    } else if (r.ratio <= cfg_.fall_ratio) {
        direction = Direction::falling;
    }

    if (direction == last_direction_ && direction != Direction::stable) {
        if (consecutive_ < cfg_.history) ++consecutive_;
    } else {
        consecutive_ = (direction == Direction::stable) ? 0 : 1;
    }
    last_direction_ = direction;

    // A confirmed change needs the direction to persist; a single episode above
    // the ratio is "one door slamming" and stays unconfirmed.
    confirmed_ = (direction != Direction::stable) && consecutive_ >= cfg_.confirm_samples;

    r.direction = direction;
    r.confirmed = confirmed_;
    r.consecutive = consecutive_;
    if (direction == Direction::stable) {
        r.reason = "activity within the learned baseline band (ratio " +
                   std::to_string(r.ratio) + ")";
    } else if (confirmed_) {
        r.reason = std::string("activity ") + direction_to_string(direction) + " to " +
                   std::to_string(r.ratio) + "x the learned baseline over " +
                   std::to_string(consecutive_) + " samples";
    } else {
        r.reason = std::string("sustained ") + direction_to_string(direction) +
                   " activity not yet confirmed (" + std::to_string(consecutive_) + " of " +
                   std::to_string(cfg_.confirm_samples) + ")";
    }
    last_result_ = r;
    have_result_ = true;
    return r;
}

DetectorStatus ActivityChangeDetector::status_for(const Result& r) const {
    if (!r.baseline_ready) return DetectorStatus::inconclusive;
    if (r.confirmed) return DetectorStatus::detected;
    return DetectorStatus::not_detected;
}

Observation ActivityChangeDetector::to_observation(const Result& r,
                                                   const std::string& observation_id,
                                                   const std::string& source,
                                                   Provenance provenance) const {
    Observation o;
    o.observation_id = observation_id;
    o.source = source;
    o.provenance = provenance;
    o.detector_id = cfg_.detector_id;
    o.model_id = cfg_.model_id;
    o.observation_timestamp = ActivityChangeDetector::now_default();
    o.status = status_for(r);
    o.features = nlohmann::json::object();
    o.features[cfg_.feature] = r.recent_level;
    o.features["baseline_level"] = r.baseline_level;
    o.features["ratio"] = r.ratio;
    o.features["direction"] = direction_to_string(r.direction);
    o.features["consecutive"] = r.consecutive;
    o.features["samples"] = r.samples;
    o.features["baseline_ready"] = r.baseline_ready;
    // Confidence is the strength of the change, not a probability of any specific cause: it
    // saturates at 1.0 and stays 0 while unconfirmed.
    //
    // It must be clamped at BOTH ends. A confirmed FALLING change has ratio < 1, so
    // (ratio - 1) is negative and a one-sided min() would produce a negative confidence -
    // which the validated Event constructor rejects, turning a legitimate falling finding
    // into an exception. The magnitude of the deviation from 1.0 is the strength in both
    // directions.
    if (r.confirmed) {
        const double strength = std::abs(r.ratio - 1.0) / 2.0;
        o.confidence = std::max(0.0, std::min(1.0, strength));
    } else {
        o.confidence = 0.0;
    }
    o.note = r.reason;
    return o;
}

// ---------------------------------------------------------------------------
// VectorDriftDetector
// ---------------------------------------------------------------------------

std::chrono::system_clock::time_point VectorDriftDetector::now_default() {
    return std::chrono::system_clock::now();
}

const char* VectorDriftDetector::drift_state_to_string(Drift d) {
    switch (d) {
        case Drift::warming_up: return "warming_up";
        case Drift::normal: return "normal";
        case Drift::drifting: return "drifting";
        case Drift::confirmed_shift: return "confirmed_shift";
        case Drift::recovering: return "recovering";
    }
    return "unknown";
}

void VectorDriftDetector::reset() {
    centroid_.clear();
    ring_.clear();
    dim_ = 0;
    distance_sum_ = 0.0;
    distance_count_ = 0;
    samples_ = 0;
    consecutive_ = 0;
    state_ = Drift::warming_up;
    last_distance_ = 0.0;
}

VectorDriftDetector::Result VectorDriftDetector::snapshot() const {
    Result r;
    r.state = state_;
    r.baseline_ready = samples_ >= cfg_.warmup;
    r.distance = last_distance_;
    r.baseline_distance = distance_count_ > 0 ? distance_sum_ / static_cast<double>(distance_count_) : 0.0;
    r.consecutive = consecutive_;
    r.samples = samples_;
    r.dim = dim_;
    return r;
}

VectorDriftDetector::Result VectorDriftDetector::observe(
    const std::vector<double>& vector, std::chrono::system_clock::time_point /*timestamp*/) {
    Result r = snapshot();

    if (vector.empty()) {
        r.reason = "empty feature vector refused";
        return r;
    }
    if (vector.size() > FeatureBounds::kMaxVectorDim) {
        r.reason = "feature vector too large (max " +
                   std::to_string(FeatureBounds::kMaxVectorDim) + ")";
        return r;
    }
    if (!vector_is_finite(vector.data(), vector.size())) {
        r.reason = "non-finite feature vector refused";
        return r;
    }
    if (dim_ == 0) {
        dim_ = cfg_.dim > 0 ? cfg_.dim : vector.size();
        if (vector.size() != dim_) {
            r.reason = "feature vector dimension does not match the configured dimension";
            return r;
        }
        centroid_.assign(dim_, 0.0);
    } else if (vector.size() != dim_) {
        r.reason = "feature vector dimension changed (" + std::to_string(vector.size()) +
                   " != " + std::to_string(dim_) + "); refused";
        return r;
    }

    ++samples_;

    // Warm-up: build the centroid as the running mean of the quiet window.
    if (samples_ <= cfg_.warmup) {
        for (size_t i = 0; i < dim_; ++i) {
            centroid_[i] += (vector[i] - centroid_[i]) / static_cast<double>(samples_);
        }
        state_ = samples_ < cfg_.warmup ? Drift::warming_up : Drift::normal;
        last_distance_ = 0.0;
        r = snapshot();
        r.reason = samples_ < cfg_.warmup
                       ? "warming up (" + std::to_string(samples_) + " of " +
                             std::to_string(cfg_.warmup) + " baseline windows)"
                       : "baseline established over " + std::to_string(cfg_.warmup) + " windows";
        return r;
    }

    const double distance = cosine_distance(vector.data(), centroid_.data(), dim_);
    last_distance_ = distance;

    if (state_ == Drift::warming_up) state_ = Drift::normal;

    if (distance >= cfg_.drift_threshold) {
        ++consecutive_;
        // Sustained shift only: `confirm_windows` consecutive drifted windows.
        state_ = consecutive_ >= cfg_.confirm_windows ? Drift::confirmed_shift : Drift::drifting;
    } else {
        consecutive_ = 0;
        if (state_ == Drift::confirmed_shift) {
            state_ = distance <= cfg_.recovery_threshold ? Drift::normal : Drift::recovering;
        } else if (state_ == Drift::recovering) {
            if (distance <= cfg_.recovery_threshold) state_ = Drift::normal;
        } else {
            state_ = Drift::normal;
        }
    }

    // The centroid is maintained ONLY while the regime looks normal, so a
    // sustained shift cannot drag the baseline onto itself and hide the change.
    if (state_ == Drift::normal) {
        const double alpha = 1.0 / static_cast<double>(cfg_.warmup);
        for (size_t i = 0; i < dim_; ++i) {
            centroid_[i] = (1.0 - alpha) * centroid_[i] + alpha * vector[i];
        }
        ring_.push_back(distance);
        if (ring_.size() > cfg_.history) ring_.erase(ring_.begin());
        distance_sum_ += distance;
        ++distance_count_;
    }

    r = snapshot();
    switch (state_) {
        case Drift::confirmed_shift:
            r.reason = "sustained regime shift: cosine distance " + std::to_string(distance) +
                       " >= " + std::to_string(cfg_.drift_threshold) + " for " +
                       std::to_string(consecutive_) + " consecutive windows";
            break;
        case Drift::drifting:
            r.reason = "drift not yet confirmed (" + std::to_string(consecutive_) + " of " +
                       std::to_string(cfg_.confirm_windows) + " windows)";
            break;
        case Drift::recovering:
            r.reason = "shift decaying (distance " + std::to_string(distance) + ")";
            break;
        case Drift::normal:
            r.reason = "vector within the learned baseline (distance " + std::to_string(distance) + ")";
            break;
        case Drift::warming_up:
            r.reason = "warming up";
            break;
    }
    return r;
}

DetectorStatus VectorDriftDetector::status_for(const Result& r) const {
    if (!r.baseline_ready) return DetectorStatus::inconclusive;
    if (r.state == Drift::confirmed_shift) return DetectorStatus::detected;
    return DetectorStatus::not_detected;
}

Observation VectorDriftDetector::to_observation(const Result& r,
                                                const std::string& observation_id,
                                                const std::string& source,
                                                Provenance provenance) const {
    Observation o;
    o.observation_id = observation_id;
    o.source = source;
    o.provenance = provenance;
    o.detector_id = cfg_.detector_id;
    o.model_id = cfg_.model_id;
    o.observation_timestamp = VectorDriftDetector::now_default();
    o.status = status_for(r);
    o.features = nlohmann::json::object();
    o.features["vector_dim"] = r.dim;
    o.features["cosine_distance"] = r.distance;
    o.features["baseline_distance"] = r.baseline_distance;
    o.features["drift_threshold"] = cfg_.drift_threshold;
    o.features["consecutive_windows"] = r.consecutive;
    o.features["samples"] = r.samples;
    o.features["state"] = drift_state_to_string(r.state);
    o.confidence = r.state == Drift::confirmed_shift
                       ? std::min(1.0, r.distance / (2.0 * cfg_.drift_threshold))
                       : 0.0;
    o.note = r.reason;
    return o;
}

}  // namespace homeguardian
