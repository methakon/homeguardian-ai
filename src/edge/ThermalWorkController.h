// ThermalWorkController — heat relief that cannot cost the event.
//
// THE ONE INVARIANT
// -----------------
// Thermal relief reduces SEMANTIC work only. It never reduces, gates, throttles or skips the Tier-0
// observation rate. Slowing the cheap reflex to save heat loses the event rather than merely
// delaying analysis, which is the opposite of the point: the whole architecture exists so that a
// fast subject is still seen while the expensive stage is cold.
//
// That invariant is enforced by construction rather than by discipline. There is no setter for a
// Tier-0 rate anywhere in this class, and `ThermalDecision::tier0_rate_multiplier` is a constant
// 1.0 that no input can move. A test sweeps the whole input space to prove it.
//
// WHERE THE LIMIT COMES FROM
// --------------------------
// The device's OWN trip points, read from its thermal zones. The binding constraint is the
// MINIMUM passive trip across all zones - on the target handset that is the skin/quiet zone, not
// the CPU zones, whose limits are far higher and would let the envelope be overrun by tens of
// degrees. Assuming a throttle point instead of reading it is exactly how a device gets cooked by
// a design that looked correct on paper.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hg::edge {

enum class ThermalState { normal, warm, hot, critical };

inline const char* thermal_state_to_string(ThermalState s) {
    switch (s) {
        case ThermalState::normal:   return "normal";
        case ThermalState::warm:     return "warm";
        case ThermalState::hot:      return "hot";
        case ThermalState::critical: return "critical";
    }
    return "unknown";
}

// One zone as the device reports it. `passive_trip_c` is the temperature at which the platform
// starts throttling that zone; `critical_trip_c` is where it shuts down.
struct ThermalZone {
    std::string name;              // "skin", "quiet", "cpu", ... - kept for the reason string
    double current_c = 0.0;
    double passive_trip_c = 0.0;
    double critical_trip_c = 0.0;
};

struct ThermalConfig {
    // Fractions of the envelope at which semantic work steps down.
    double warm_fraction = 0.70;
    double hot_fraction = 0.85;
    double critical_fraction = 0.95;
    // Hysteresis: to step BACK UP the reading must fall this far below the threshold being left,
    // or a device hovering at a boundary flaps between states every sample.
    double recovery_margin_fraction = 0.05;

    // The semantic ladder. Tier-0 does not appear here, and cannot.
    int semantic_threads_normal = 2;      // 1-2 threads, never all cores
    int semantic_threads_warm = 1;
    int semantic_threads_hot = 1;
    int semantic_threads_critical = 0;    // no semantic work at all
    std::size_t semantic_frames_normal = 8;   // the 3-8 frame band from the design
    std::size_t semantic_frames_warm = 5;
    std::size_t semantic_frames_hot = 3;
    std::size_t semantic_frames_critical = 0;
};

struct ThermalDecision {
    ThermalState state = ThermalState::normal;

    // The envelope actually used, and where it came from.
    bool envelope_known = false;
    double envelope_c = 0.0;
    std::string binding_zone;       // the zone that set the envelope
    double hottest_c = 0.0;
    std::string hottest_zone;
    double load = 0.0;              // hottest / envelope, 0 when the envelope is unknown

    // What semantic work is permitted right now.
    int semantic_threads = 0;
    std::size_t semantic_frames = 0;
    bool semantic_enabled = false;

    // THE INVARIANT. Always 1.0. Present so a caller, a test and a reviewer can see the claim
    // stated rather than inferred from the absence of a setter.
    double tier0_rate_multiplier = 1.0;

    std::string reason;
};

class ThermalWorkController {
public:
    explicit ThermalWorkController(ThermalConfig cfg = {}) : cfg_(cfg) {}

    // Feed the device's own zones. An empty list means the envelope is UNKNOWN - not that the
    // device is cool - and is reported as such rather than guessed at.
    ThermalDecision update(const std::vector<ThermalZone>& zones);

    ThermalState state() const { return state_; }
    const ThermalConfig& config() const { return cfg_; }
    std::uint64_t samples() const { return samples_; }
    std::uint64_t step_downs() const { return step_downs_; }
    std::uint64_t step_ups() const { return step_ups_; }
    std::uint64_t held_by_hysteresis() const { return held_; }

    // The semantic work for a given state, without needing a reading.
    ThermalDecision for_state(ThermalState s) const;

private:
    ThermalState level_for(double load) const;
    ThermalDecision build(ThermalState s) const;
    double threshold_of(ThermalState s) const;

    ThermalConfig cfg_;
    ThermalState state_ = ThermalState::normal;
    bool have_state_ = false;
    double last_envelope_ = 0.0;
    std::uint64_t samples_ = 0;
    std::uint64_t step_downs_ = 0;
    std::uint64_t step_ups_ = 0;
    std::uint64_t held_ = 0;
};

}  // namespace hg::edge
