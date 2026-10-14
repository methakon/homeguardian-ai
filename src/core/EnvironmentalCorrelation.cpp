#include "EnvironmentalCorrelation.h"

#include <cmath>

namespace homeguardian {

bool EnvironmentalBounds::metric_supported(const std::string& metric) {
    return metric == "temperature" || metric == "humidity" || metric == "pressure" ||
           metric == "illuminance";
}

bool EnvironmentalBounds::plausible(const std::string& metric, double value) {
    if (!std::isfinite(value)) return false;
    if (metric == "temperature") return value >= -90.0 && value <= 70.0;   // deg C: Earth surface range
    if (metric == "humidity") return value >= 0.0 && value <= 100.0;       // relative %
    if (metric == "pressure") return value >= 850.0 && value <= 1100.0;   // hPa at/near sea level
    if (metric == "illuminance") return value >= 0.0 && value <= 200000.0;  // lux (direct sun ~100k)
    return false;
}

bool EnvironmentalAdapter::validate(const EnvironmentalSample& sample,
                                    std::chrono::system_clock::time_point ingestion_now,
                                    std::string* reason) const {
    auto fail = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };

    if (sample.source.empty()) return fail("source is empty");
    if (sample.source.size() > EnvironmentalBounds::kMaxSourceChars) return fail("source too long");
    if (sample.metric.empty()) return fail("metric is empty");
    if (sample.metric.size() > EnvironmentalBounds::kMaxMetricChars) return fail("metric too long");
    if (!EnvironmentalBounds::metric_supported(sample.metric)) {
        return fail("unsupported metric '" + sample.metric + "'");
    }
    if (!EnvironmentalBounds::plausible(sample.metric, sample.value)) {
        return fail("value implausible or non-finite for metric '" + sample.metric + "'");
    }
    if (sample.unit.size() > EnvironmentalBounds::kMaxUnitChars) return fail("unit too long");
    if (sample.note.size() > EnvironmentalBounds::kMaxNoteChars) return fail("note too long");
    if (sample.authorization.empty()) return fail("missing authorization context");
    if (sample.authorization.size() > EnvironmentalBounds::kMaxAuthorizationChars) {
        return fail("authorization context too long");
    }
    if (sample.authorization.find("granted") == std::string::npos) {
        return fail("authorization context does not record a grant");
    }
    if (sample.observed_at.time_since_epoch().count() == 0) return fail("missing observation timestamp");
    if (sample.observed_at > ingestion_now + cfg_.max_future_skew) {
        return fail("observation timestamp is in the future");
    }
    if (reason != nullptr) reason->clear();
    return true;
}

std::optional<Observation> EnvironmentalAdapter::to_observation(
    const EnvironmentalSample& sample, const std::string& observation_id,
    std::chrono::system_clock::time_point ingestion_now) const {
    std::string why;
    if (!validate(sample, ingestion_now, &why)) return std::nullopt;

    Observation o;
    o.observation_id = observation_id;
    o.source = sample.source;
    o.provenance = sample.provenance;
    o.detector_id = "environmental-" + sample.metric;
    o.model_id = "sensor-report-v1";
    o.observation_timestamp = sample.observed_at;
    // A measurement is a REPORT, not a detection: the measurement arriving is the
    // observed fact. Whether it MATTERS is decided downstream (threshold crossing,
    // correlation), never here.
    o.status = DetectorStatus::detected;
    o.features = nlohmann::json::object();
    o.features["metric"] = sample.metric;
    o.features["value"] = sample.value;
    if (!sample.unit.empty()) o.features["unit"] = sample.unit;
    o.features["authorization"] = sample.authorization;
    o.quality = 1.0;  // an authorized, plausible measurement
    if (!sample.note.empty()) o.note = sample.note;
    return o;
}

void CorrelationWindow::add_change(const ChangeRecord& record) {
    if (record.kind.empty()) return;
    changes_.push_back(record);
    if (changes_.size() > cfg_.max_changes) {
        changes_.erase(changes_.begin(), changes_.begin() + (changes_.size() - cfg_.max_changes));
    }
}

void CorrelationWindow::add_sample(const EnvironmentalSample& sample) {
    if (!EnvironmentalBounds::metric_supported(sample.metric)) return;
    if (!EnvironmentalBounds::plausible(sample.metric, sample.value)) return;
    samples_.push_back(sample);
    if (samples_.size() > cfg_.max_samples) {
        samples_.erase(samples_.begin(), samples_.begin() + (samples_.size() - cfg_.max_samples));
    }
}

void CorrelationWindow::clear() {
    changes_.clear();
    samples_.clear();
}

CorrelationWindow::Result CorrelationWindow::correlate(std::chrono::system_clock::time_point now) {
    Result r;

    // Prune outside the window: stale evidence must not produce a fresh finding.
    const auto cutoff = now - cfg_.window;
    changes_.erase(std::remove_if(changes_.begin(), changes_.end(),
                                  [cutoff](const ChangeRecord& c) { return c.at < cutoff; }),
                   changes_.end());
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [cutoff](const EnvironmentalSample& s) { return s.observed_at < cutoff; }),
                   samples_.end());

    r.change_count = changes_.size();
    r.environmental_count = samples_.size();

    if (changes_.empty() && samples_.empty()) {
        r.status = "insufficient_evidence";
        r.reason = "no detected change and no environmental sample in the window";
        return r;
    }
    if (changes_.empty()) {
        r.status = "insufficient_evidence";
        r.reason = "environmental samples present but no detected change to correlate with";
        return r;
    }
    if (samples_.empty()) {
        r.status = "insufficient_evidence";
        r.reason = "detected change present but no authorized environmental sample to correlate with";
        return r;
    }

    // Closest pairing by |delta|; ties resolve to the earliest sample.
    const ChangeRecord* best_change = nullptr;
    const EnvironmentalSample* best_sample = nullptr;
    std::chrono::seconds best_delta = cfg_.max_lag;
    bool found = false;
    for (const ChangeRecord& c : changes_) {
        for (const EnvironmentalSample& s : samples_) {
            const auto delta = std::chrono::duration_cast<std::chrono::seconds>(s.observed_at - c.at);
            const long long magnitude = delta.count() < 0 ? -delta.count() : delta.count();
            if (!found || magnitude < best_delta.count()) {
                best_delta = std::chrono::seconds(magnitude);
                best_change = &c;
                best_sample = &s;
                found = true;
            }
        }
    }

    if (!found || !best_change || !best_sample || best_delta > cfg_.max_lag) {
        r.status = "insufficient_evidence";
        r.reason = "no environmental sample within " + std::to_string(cfg_.max_lag.count()) +
                   "s of a detected change";
        return r;
    }

    const auto signed_lag =
        std::chrono::duration_cast<std::chrono::seconds>(best_sample->observed_at - best_change->at);
    r.correlated = true;
    r.status = "environmental_correlation";
    r.lag_seconds = static_cast<long long>(signed_lag.count());
    r.metric = best_sample->metric;
    r.metric_value = best_sample->value;
    r.reason = "change from '" + best_change->source + "' (" + best_change->kind +
               ") coincides with a " + best_sample->metric + " reading within " +
               std::to_string(r.lag_seconds) + "s. Correlation only: this is not causation and not "
               "a prediction of any environmental event";
    return r;
}

Observation CorrelationWindow::to_observation(const Result& result, const std::string& observation_id,
                                             const std::string& source, Provenance provenance,
                                             std::chrono::system_clock::time_point now) const {
    Observation o;
    o.observation_id = observation_id;
    o.source = source;
    o.provenance = provenance;
    o.detector_id = "environmental-correlation";
    o.model_id = "window-correlation-v1";
    o.observation_timestamp = now;
    o.status = result.correlated ? DetectorStatus::detected : DetectorStatus::inconclusive;
    o.features = nlohmann::json::object();
    o.features["result"] = result.status;
    o.features["change_count"] = result.change_count;
    o.features["environmental_count"] = result.environmental_count;
    o.features["lag_seconds"] = result.lag_seconds;
    if (!result.metric.empty()) {
        o.features["metric"] = result.metric;
        o.features["metric_value"] = result.metric_value;
    }
    // Low confidence by construction: Category B evidence is weak evidence.
    o.confidence = result.correlated ? 0.25 : 0.0;
    o.note = result.reason;
    return o;
}

}  // namespace homeguardian
