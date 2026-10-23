#include "core/SyntheticImpactRule.h"

#include "core/Observation.h"
#include "core/UUID.h"

#include <iomanip>
#include <sstream>

namespace homeguardian {

namespace {

// True when this observation came from the impact classifier and takes part in
// the pattern (either the pending transient or the completed confirmation).
bool is_pattern_observation(const Observation& o, const std::string& detector_id,
                            bool& confirmed) {
    if (o.detector_id != detector_id) return false;
    if (!o.features.is_object() || !o.features.contains("phase")) return false;
    const std::string phase = o.features["phase"].get<std::string>();
    if (phase == "confirmed") {
        confirmed = true;
        return true;
    }
    return phase == "transient";   // precursor support
}

}  // namespace

std::optional<Alert> SyntheticImpactRule::evaluate(const std::vector<Event>& window) {
    std::vector<std::string> supporting_ids;
    size_t confirmed_count = 0;
    double transient_value = 0.0;
    double stillness_value = 0.0;
    size_t samples_since = 0;
    std::string reason;

    for (const auto& ev : window) {
        const auto o = Observation::from_event(ev);
        if (!o) continue;
        bool confirmed = false;
        if (!is_pattern_observation(*o, cfg_.detector_id, confirmed)) continue;

        if (supporting_ids.size() < cfg_.max_contributing) {
            supporting_ids.push_back(ev.get_event_id());
        }
        if (confirmed) {
            ++confirmed_count;
            if (o->features.contains("transient_value"))
                transient_value = o->features["transient_value"].get<double>();
            if (o->features.contains("stillness_value"))
                stillness_value = o->features["stillness_value"].get<double>();
            if (o->features.contains("samples_since_transient"))
                samples_since = o->features["samples_since_transient"].get<size_t>();
            if (reason.empty() && !o->note.empty()) reason = o->note;
        }
    }

    // A confirmed pattern needs BOTH a completed confirmation and enough
    // supporting observations. A single ambiguous sample never gets here.
    if (confirmed_count == 0) return std::nullopt;
    if (supporting_ids.size() < cfg_.min_supporting) return std::nullopt;

    std::ostringstream msg;
    msg << "SIMULATED critical impact pattern detected (transient "
        << std::fixed << std::setprecision(2) << transient_value
        << " -> stillness " << stillness_value << " within " << samples_since
        << " sample(s), " << supporting_ids.size()
        << " supporting observation(s)). Demonstration only: this does not confirm "
           "a real fall and no emergency services are contacted.";

    nlohmann::json details = {
        {"impact_pattern", "transient_then_stillness"},
        {"transient_intensity", transient_value},
        {"stillness_intensity", stillness_value},
        {"samples_since_transient", samples_since},
        {"supporting_observations", supporting_ids.size()},
        {"confirmed_observations", confirmed_count},
        {"detector_id", cfg_.detector_id},
        {"provenance", "synthetic"},
        {"simulated", true},
        {"emergency_services_contacted", false},
        {"reason", reason},
    };

    // A confirmed synthetic pattern maps to critical. This is the only place in
    // the build that produces Severity::critical.
    const double confidence =
        std::min(1.0, 0.5 + 0.5 * static_cast<double>(supporting_ids.size()) / 4.0);

    return Alert::create("alert-" + UUID::generate(), Severity::critical, msg.str(),
                         supporting_ids, confidence, name_, details);
}

}  // namespace homeguardian
