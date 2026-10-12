#include "FusionEngine.h"

#include <algorithm>
#include <sstream>

namespace homeguardian {

FusionResult FusionEngine::fuse(const std::vector<Observation>& obs) const {
    FusionResult r;

    if (obs.empty()) {
        r.verdict = FusionVerdict::unavailable;
        r.reason = "no observations to fuse";
        return r;
    }

    // Find the newest observation timestamp; only fuse inputs within `window`.
    auto newest = std::max_element(obs.begin(), obs.end(),
        [](const Observation& a, const Observation& b) {
            return a.observation_timestamp < b.observation_timestamp;
        })->observation_timestamp;

    // Partition inputs into usable evidence vs not.
    //  - detected / not_detected with a confidence are usable votes.
    //  - unavailable is never a vote for "present"; it only reduces confidence.
    //  - inconclusive is not a vote.
    std::vector<const Observation*> votes;      // detected or not_detected
    std::vector<const Observation*> unusable;   // unavailable / inconclusive / stale
    for (const auto& o : obs) {
        auto age = newest - o.observation_timestamp;
        bool stale = age > cfg_.window;
        bool has_conf = o.confidence.has_value();
        bool usable_status = (o.status == DetectorStatus::detected ||
                              o.status == DetectorStatus::not_detected);
        if (!stale && usable_status && has_conf) {
            votes.push_back(&o);
        } else {
            unusable.push_back(&o);
        }
    }

    // Record which detector ids contributed usable votes.
    for (const auto* v : votes) {
        r.sources.push_back(v->detector_id);
        r.contributing++;
    }

    if (r.contributing < cfg_.min_contributing) {
        r.verdict = FusionVerdict::unavailable;
        std::ostringstream os;
        os << "only " << r.contributing << " usable input(s); need >= "
           << cfg_.min_contributing;
        if (!unusable.empty()) os << " (" << unusable.size() << " unusable)";
        r.reason = os.str();
        return r;
    }

    // Tally votes. "detected" is a positive vote; "not_detected" is negative.
    size_t positive = 0, negative = 0;
    double conf_sum = 0.0;
    for (const auto* v : votes) {
        if (v->status == DetectorStatus::detected) positive++;
        else negative++;
        conf_sum += v->confidence.value();
    }
    const double total = static_cast<double>(r.contributing);
    const double agreement = static_cast<double>(std::max(positive, negative)) / total;
    const double mean_conf = conf_sum / total;

    // Confidence propagation: agreement raises confidence, disagreement and
    // unusable inputs lower it. Never exceeds the mean input confidence.
    double penalty = 1.0;
    if (!unusable.empty()) {
        // Each unusable input dilutes confidence modestly.
        penalty *= static_cast<double>(r.contributing) /
                   static_cast<double>(r.contributing + unusable.size());
    }
    r.confidence = std::min(mean_conf, mean_conf * agreement * penalty);
    if (r.confidence < 0.0) r.confidence = 0.0;
    if (r.confidence > 1.0) r.confidence = 1.0;

    const bool majority_detected = positive > negative;

    if (agreement < cfg_.agreement_threshold) {
        // Detectors conflict: do not pick a winner.
        r.verdict = FusionVerdict::inconclusive;
        std::ostringstream os;
        os << "detectors conflict: " << positive << " detected vs " << negative
           << " not_detected (agreement " << agreement << " < threshold "
           << cfg_.agreement_threshold << ")";
        r.reason = os.str();
        return r;
    }

    if (majority_detected) {
        r.verdict = FusionVerdict::detected;
    } else {
        r.verdict = FusionVerdict::not_detected;
    }
    std::ostringstream os;
    os << (majority_detected ? "detected" : "not_detected") << " by " << r.contributing
       << " input(s), agreement " << agreement << ", confidence " << r.confidence;
    r.reason = os.str();
    return r;
}

}  // namespace homeguardian
