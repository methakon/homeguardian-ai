#pragma once

// ActivityEpisodeSelector: represents a SHORT sustained activity change inside a long window.
//
// WHY THIS EXISTS
// ---------------
// FusionEngine fuses every usable observation in its window as an independent vote, and a vote is
// either "detected" or "not_detected". A brief real change therefore competes with the quiet baseline
// that surrounds it: in the recorded consented capture, six consecutive confirmed rises sat beside 68
// baseline observations and 41 settling observations, agreement came out at 0.5913 against a 0.6
// threshold, and the fusion reported `inconclusive` - so a change that the detector confirmed six
// times over could not be reported at all, because the window was long and the event was short.
//
// This component answers a narrower question instead: *is there a sustained episode of change here?*
// It selects the strongest run of consecutive confirmed changes and reports it with its own support,
// so a short event is judged on its own evidence rather than on the size of the window it arrived in.
//
// WHAT IT REFUSES TO DO
// ---------------------
//  - It does not lower any threshold. The confidence gate (0.5 by default) and the existing
//    FusionEngine behaviour are untouched; the episode is an ADDITIONAL, opt-in evidence path.
//  - It does not manufacture change. A quiet scene contains no confirmed changes at all, so it has no
//    episode: `best()` returns nullopt, and the caller must then behave exactly as before.
//  - It does not call a single confirmed change a change of activity: a run shorter than `min_span`
//    (3 by default) is refused - "one anomalous frame is a door slamming".
//  - It does not turn the end of activity into the start of one: with `require_rising` set (the
//    default), a run of confirmed *falls* - which is what a scene produces when the subject walks
//    away - is not an activity episode. Without that rule the recorded still-scene run, whose only
//    confirmed changes are falls, would have produced an alert.
//
// PROVENANCE
// ----------
// An episode carries the provenance of its own members, derived by the same rule the fusion engine
// uses, so a replay can never be reported as a measurement.

#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

struct ActivityEpisode {
    bool valid = false;
    size_t start_index = 0;              // index into the caller's (unordered) observation vector
    size_t length = 0;                   // consecutive confirmed changes
    double mean_confidence = 0.0;        // mean of the members' confidences
    double weakest_confidence = 0.0;     // the least confident member: the conservative bound
    EvidenceProvenance provenance = EvidenceProvenance::uncertain;
    size_t real_inputs = 0;
    size_t synthetic_inputs = 0;
    std::string direction;               // "rising" | "falling" | "unknown" | "mixed"
    std::string reason;
};

class ActivityEpisodeSelector {
public:
    struct Config {
        // Consecutive confirmed changes required before this is an episode at all. Three is the
        // detector's own confirmation habit (see ActivityChangeDetector::confirm_windows) and is
        // deliberately not a new sensitivity knob: a run of one or two confirmed changes is a spike.
        size_t min_span = 3;
        // The same gate the alert rule already uses. Not lowered here, and not tunable per-call by
        // a caller that wants an alert.
        double min_mean_confidence = 0.5;
        // An activity claim must come from activity starting, not from it ceasing.
        bool require_rising = true;
        // Bound on the work done per call.
        size_t max_observations = 4096;
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    explicit ActivityEpisodeSelector(Config cfg = defaults()) : cfg_(cfg) {}

    // The strongest eligible episode, or nullopt when the window contains none.
    std::optional<ActivityEpisode> best(const std::vector<Observation>& observations) const;

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

}  // namespace homeguardian
