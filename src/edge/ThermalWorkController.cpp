#include "edge/ThermalWorkController.h"

#include <algorithm>
#include <cmath>

namespace hg::edge {

double ThermalWorkController::threshold_of(ThermalState s) const {
    switch (s) {
        case ThermalState::normal:   return 0.0;
        case ThermalState::warm:     return cfg_.warm_fraction;
        case ThermalState::hot:      return cfg_.hot_fraction;
        case ThermalState::critical: return cfg_.critical_fraction;
    }
    return 1.0;
}

ThermalState ThermalWorkController::level_for(double load) const {
    if (load >= cfg_.critical_fraction) return ThermalState::critical;
    if (load >= cfg_.hot_fraction)      return ThermalState::hot;
    if (load >= cfg_.warm_fraction)     return ThermalState::warm;
    return ThermalState::normal;
}

ThermalDecision ThermalWorkController::build(ThermalState s) const {
    ThermalDecision d;
    d.state = s;
    switch (s) {
        case ThermalState::normal:
            d.semantic_threads = cfg_.semantic_threads_normal;
            d.semantic_frames  = cfg_.semantic_frames_normal;
            d.semantic_enabled = cfg_.semantic_frames_normal > 0 && cfg_.semantic_threads_normal > 0;
            break;
        case ThermalState::warm:
            d.semantic_threads = cfg_.semantic_threads_warm;
            d.semantic_frames  = cfg_.semantic_frames_warm;
            d.semantic_enabled = cfg_.semantic_frames_warm > 0 && cfg_.semantic_threads_warm > 0;
            break;
        case ThermalState::hot:
            d.semantic_threads = cfg_.semantic_threads_hot;
            d.semantic_frames  = cfg_.semantic_frames_hot;
            d.semantic_enabled = cfg_.semantic_frames_hot > 0 && cfg_.semantic_threads_hot > 0;
            break;
        case ThermalState::critical:
            d.semantic_threads = cfg_.semantic_threads_critical;
            d.semantic_frames  = cfg_.semantic_frames_critical;
            d.semantic_enabled = false;      // no semantic work at all at the envelope
            break;
    }
    // The invariant, stated on every decision rather than left to inference.
    d.tier0_rate_multiplier = 1.0;
    return d;
}

ThermalDecision ThermalWorkController::for_state(ThermalState s) const {
    return build(s);
}

ThermalDecision ThermalWorkController::update(const std::vector<ThermalZone>& zones) {
    ++samples_;

    // No zones reported. That is an UNKNOWN envelope, not a cool device, and the two must not be
    // confused: assuming a throttle point is how a device gets overrun by a design that looked
    // correct on paper. The state stays where it is, the work stays at normal, and the decision
    // says plainly that the envelope was not available.
    if (zones.empty()) {
        ThermalDecision d = build(have_state_ ? state_ : ThermalState::normal);
        d.envelope_known = false;
        d.reason = "envelope unknown: the device reported no thermal zones - not a claim that it is cool";
        return d;
    }

    // The envelope is the MINIMUM passive trip across every zone. On the target the binding zone
    // is the skin/quiet zone; the CPU zones trip far higher and would let the envelope be overrun.
    double envelope = 0.0;
    std::string binding;
    double hottest = -1e9;
    std::string hottest_zone;
    for (const auto& z : zones) {
        if (z.passive_trip_c > 0.0 && (envelope == 0.0 || z.passive_trip_c < envelope)) {
            envelope = z.passive_trip_c;
            binding = z.name;
        }
        if (z.current_c > hottest) {
            hottest = z.current_c;
            hottest_zone = z.name;
        }
    }

    if (envelope <= 0.0) {
        // Affirmative zeros, or zones with no passive trip declared. Still unknown.
        ThermalDecision d = build(have_state_ ? state_ : ThermalState::normal);
        d.envelope_known = false;
        d.hottest_c = hottest;
        d.hottest_zone = hottest_zone;
        d.reason = "envelope unknown: no zone declared a passive trip point";
        return d;
    }

    const double load = hottest / envelope;
    const ThermalState target = level_for(load);

    ThermalState next = state_;
    if (!have_state_) {
        next = target;                       // first reading establishes the state directly
    } else if (target > state_) {
        next = target;                       // degrade immediately: heat is not negotiable
        ++step_downs_;
    } else if (target < state_) {
        // Recover only when the reading is clear of the threshold being left. Without this a
        // device hovering at a boundary flaps between states on every sample.
        const double margin = cfg_.recovery_margin_fraction;
        if (load < threshold_of(state_) - margin) {
            next = target;
            ++step_ups_;
        } else {
            ++held_;                          // held by hysteresis, and counted so it is visible
        }
    }
    state_ = next;
    have_state_ = true;
    last_envelope_ = envelope;

    ThermalDecision d = build(state_);
    d.envelope_known = true;
    d.envelope_c = envelope;
    d.binding_zone = binding;
    d.hottest_c = hottest;
    d.hottest_zone = hottest_zone;
    d.load = load;

    switch (state_) {
        case ThermalState::normal:
            d.reason = "within the envelope: full semantic work";
            break;
        case ThermalState::warm:
            d.reason = "approaching the envelope (" + binding + "): semantic work reduced";
            break;
        case ThermalState::hot:
            d.reason = "close to the envelope (" + binding + "): semantic work reduced further";
            break;
        case ThermalState::critical:
            d.reason = "at the envelope (" + binding + "): semantic work suspended";
            break;
    }
    return d;
}

}  // namespace hg::edge
