#include "core/ActivityEpisode.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace homeguardian {

namespace {

// The direction an observation reports, when it reports one at all. Detectors that do not model a
// direction (or do not publish it as a feature) return an empty string and never disqualify a run.
std::string direction_of(const Observation& o) {
    if (!o.features.is_object()) return {};
    const auto it = o.features.find("direction");
    if (it == o.features.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace

std::optional<ActivityEpisode> ActivityEpisodeSelector::best(
    const std::vector<Observation>& observations) const {
    if (observations.empty() || cfg_.min_span == 0) return std::nullopt;

    // Time order is what makes "consecutive" mean anything. Work on indices so the caller keeps its
    // own vector untouched, and cap the work done.
    std::vector<size_t> order(observations.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return observations[a].observation_timestamp < observations[b].observation_timestamp;
    });
    if (order.size() > cfg_.max_observations) order.resize(cfg_.max_observations);

    std::optional<ActivityEpisode> best;
    std::vector<size_t> run;
    // The established direction of the run being collected. A change of character - a sustained rise
    // followed by a sustained fall - ends the run and starts a new one, because they are two different
    // episodes, not one long one. Without this the recorded run's six rises and forty-one settling
    // falls merged into a single 47-long run whose direction was "mixed", and the whole run was then
    // refused: the improvement would have thrown away exactly the event it exists to represent.
    std::string run_direction;

    auto close_run = [&]() {
        if (run.size() < cfg_.min_span) {
            run.clear();
            return;
        }
        double sum = 0.0;
        double weakest = 1.0;
        size_t real_inputs = 0;
        size_t synthetic_inputs = 0;
        std::string direction;
        bool direction_unknown = false;
        for (size_t idx : run) {
            const Observation& o = observations[idx];
            const double c = o.confidence.value_or(0.0);
            sum += c;
            weakest = std::min(weakest, c);
            if (o.provenance == Provenance::real) {
                ++real_inputs;
            } else {
                ++synthetic_inputs;
            }
            const std::string d = direction_of(o);
            if (d.empty()) {
                direction_unknown = true;
                continue;
            }
            if (direction.empty()) {
                direction = d;
            } else if (direction != d) {
                direction = "mixed";
            }
        }
        if (direction.empty() && direction_unknown) direction = "unknown";

        size_t illumination_members = 0;
        size_t characterised_members = 0;
        for (size_t idx : run) {
            const auto& features = observations[idx].features;
            if (!features.is_object() || !features.contains("change_character")) continue;
            const std::string c = features.value("change_character", std::string());
            if (c.empty()) continue;
            ++characterised_members;
            if (c == "global_illumination" || c == "illumination_settling") ++illumination_members;
        }
        const bool character_ok = !cfg_.require_motion_character || characterised_members == 0 ||
                                  (illumination_members * 2 < characterised_members);

        const double mean = sum / static_cast<double>(run.size());
        const bool direction_ok =
            !cfg_.require_rising || (direction != "falling" && direction != "mixed");
        if (!direction_ok || !character_ok || mean < cfg_.min_mean_confidence) {
            run.clear();
            return;
        }

        ActivityEpisode episode;
        episode.valid = true;
        episode.start_index = run.front();
        episode.length = run.size();
        episode.mean_confidence = mean;
        episode.weakest_confidence = weakest;
        episode.provenance = derive_evidence_provenance(real_inputs, synthetic_inputs);
        episode.real_inputs = real_inputs;
        episode.synthetic_inputs = synthetic_inputs;
        episode.direction = direction;

        const bool better = !best.has_value() ||
                            mean > best->mean_confidence ||
                            (mean == best->mean_confidence && run.size() > best->length);
        if (better) best = episode;
        run.clear();
    };

    for (size_t idx : order) {
        const Observation& o = observations[idx];
        const bool confirmed_change = (o.status == DetectorStatus::detected) && o.confidence.has_value();
        if (!confirmed_change) {
            close_run();   // any non-confirmed observation (baseline, inconclusive, unavailable) ends it
            run_direction.clear();
            continue;
        }
        const std::string d = direction_of(o);
        if (!d.empty() && !run_direction.empty() && d != run_direction) {
            close_run();   // the change changed character: a new episode begins here
            run_direction.clear();
        }
        if (run_direction.empty() && !d.empty()) run_direction = d;
        run.push_back(idx);
    }
    close_run();

    if (best) {
        std::ostringstream os;
        if (cfg_.require_motion_character) {
            os << "motion-character gate on; ";
        }
        os << "activity episode: " << best->length << " consecutive confirmed "
           << (best->direction == "unknown" ? "change" : best->direction) << " observation(s), mean "
           << "confidence " << std::fixed << std::setprecision(3) << best->mean_confidence
           << ", weakest " << best->weakest_confidence << ", provenance "
           << evidence_provenance_to_string(best->provenance);
        best->reason = os.str();
    }
    return best;
}

}  // namespace homeguardian
