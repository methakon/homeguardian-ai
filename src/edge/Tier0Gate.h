// Tier0Gate — the event trigger. A pre-filter in front of the analysis process.
//
// This is the cheap, always-on stage that decides WHEN the analyser runs at all. It consumes the
// host's existing per-frame finding and emits a trigger; it performs no image analysis of its own,
// decodes nothing, and owns no camera, credential or transport.
//
// WHY IT IS A PRE-FILTER AND NOT A SCHEDULE
// -----------------------------------------
// A timer with a motion check on top still misses the fast subject: the sampling instant is what
// fails, not the check. This gate samples continuously and cheaply, so the event is detected
// whenever it happens rather than when the timer next fires.
//
// WHAT IT CONSUMES
// ----------------
// The host's real `homeguardian::CameraFinding`, produced by CameraActivitySession after the
// existing extractor and the existing ChangeCharacter classification. It is consumed directly:
// a parallel input struct would be a second source of truth and would drift from the host's
// fields. The gate also uses the host's own `is_localized_or_diffuse_motion`, so "motion, not
// merely change" means exactly what it means elsewhere in the analyzer.
//
// WHAT IT DELIBERATELY DOES NOT DO
// --------------------------------
//   * It never reduces or skips a sample. Degrading the gate to save heat loses the event
//     instead of merely delaying analysis; thermal relief is applied to the semantic stage only.
//   * It never treats a stalled window as a quiet scene. Frames the device failed to deliver are
//     a finding about our equipment, and they neither fire an event nor silently reset state.
//   * It never invents the anchor. `first_weak_hit` is recorded on the FIRST weak observation,
//     not at confirmation, because the pre-roll has to reach back to where the movement began.
#pragma once

#include "core/CameraActivitySession.h"
#include "core/ChangeCharacter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace hg::edge {

using homeguardian::CameraFinding;
using homeguardian::CameraFindingKind;
using homeguardian::ChangeCharacter;

enum class GateState { quiet, candidate, active, cooldown };

inline const char* gate_state_to_string(GateState s) {
    switch (s) {
        case GateState::quiet:     return "quiet";
        case GateState::candidate: return "candidate";
        case GateState::active:    return "active";
        case GateState::cooldown:  return "cooldown";
    }
    return "unknown";
}

struct GateConfig {
    // Thresholds are MULTIPLES of the session's own measured idle band, never absolute constants.
    // The sensor's idle floor is a property of this camera, its exposure and its lighting: on the
    // workstation camera it measured ~0.0098, so a constant would either call that noise motion or
    // hide a small body. Absolute values are used only until the band is calibrated.
    double strong_multiplier = 6.0;
    double weak_multiplier = 2.0;
    double uncalibrated_strong = 0.02;
    double uncalibrated_weak = 0.004;
    // "K of the last N OBSERVATIONS". Every observation takes a slot; a quiet one is a miss in the
    // window rather than a reason to clear it. Clearing on a non-motion sample would turn this into
    // "K consecutive", which is the rule this design exists to replace.
    std::size_t weak_window = 3;
    std::size_t weak_hits_required = 2;
    std::chrono::milliseconds latch{1000};             // 0.8-1.5 s past the last valid hit
    std::chrono::milliseconds candidate_window{5000};  // how long a lone weak hit stays live
    std::chrono::milliseconds cooldown{2000};
    // diffuse_motion covers rain and foliage as well as large movement, so it must clear a higher
    // bar than a compact crossing.
    bool diffuse_requires_travel = true;
    std::size_t diffuse_min_travel_consistent = 2;
};

// What the trigger emits. `fire` begins an event; `extend` continues one already open.
struct TriggerEvent {
    bool fire = false;
    bool extend = false;
    std::uint64_t event_id = 0;                       // stable for one continuous occurrence
    std::chrono::system_clock::time_point anchor{};   // the FIRST WEAK HIT
    std::chrono::system_clock::time_point at{};
    GateState state = GateState::quiet;
    double motion_level = 0.0;
    double idle_band = 0.0;
    double region_travel = 0.0;
    ChangeCharacter character = ChangeCharacter::unknown;
    bool was_strong = false;                          // fired on one frame rather than K-of-N
    std::string reason;                               // why it did or did not fire
};

struct GateStats {
    std::uint64_t observations = 0;
    std::uint64_t fired = 0;
    std::uint64_t extended = 0;
    std::uint64_t rejected_illumination = 0;
    std::uint64_t rejected_unipolar = 0;
    std::uint64_t rejected_weak = 0;
    std::uint64_t stalled_windows = 0;
    std::uint64_t invalid_findings = 0;
};

// Parse the host's closed vocabulary. Returns ChangeCharacter::unknown for anything else, so an
// unrecognised string can never be treated as motion.
ChangeCharacter change_character_from_string(const std::string& s);

class Tier0Gate {
public:
    explicit Tier0Gate(GateConfig cfg = {}) : cfg_(cfg) {}

    // Consume one real finding, plus the time it was observed. The timestamp is a parameter and not
    // a member of CameraFinding: the host's finding carries no clock, so taking it from the caller
    // is the only honest option. A gate that guessed its own time could not anchor a pre-roll.
    TriggerEvent update(const CameraFinding& f, std::chrono::system_clock::time_point now);

    GateState state() const { return state_; }
    const GateStats& stats() const { return stats_; }
    const GateConfig& config() const { return cfg_; }
    std::chrono::system_clock::time_point first_hit() const { return first_hit_; }

    // Close the open event without new input. Used when authorization is withdrawn or the source
    // changes, so an event cannot outlive its authority.
    void reset();

private:
    void push_window(bool weak_hit);
    std::size_t weak_count() const;
    void expire_if_stale(std::chrono::system_clock::time_point now);

    GateConfig cfg_;
    GateState state_ = GateState::quiet;
    std::uint64_t next_event_id_ = 1;
    std::chrono::system_clock::time_point first_hit_{};
    std::chrono::system_clock::time_point latch_until_{};
    std::chrono::system_clock::time_point candidate_until_{};
    std::chrono::system_clock::time_point cooldown_until_{};
    std::deque<bool> window_;   // last N observations: true = a weak motion hit
    GateStats stats_{};
};

}  // namespace hg::edge
