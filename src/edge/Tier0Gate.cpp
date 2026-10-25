#include "edge/Tier0Gate.h"

namespace hg::edge {

ChangeCharacter change_character_from_string(const std::string& s) {
    // The host's closed vocabulary. An unrecognised string maps to `unknown`, which is not motion,
    // so a typo or a future addition can never be silently treated as an event.
    if (s == "quiet")                 return ChangeCharacter::quiet;
    if (s == "global_illumination")   return ChangeCharacter::global_illumination;
    if (s == "localized_unipolar")    return ChangeCharacter::localized_unipolar;
    if (s == "localized_motion")      return ChangeCharacter::localized_motion;
    if (s == "diffuse_motion")        return ChangeCharacter::diffuse_motion;
    if (s == "illumination_settling") return ChangeCharacter::illumination_settling;
    return ChangeCharacter::unknown;
}

void Tier0Gate::push_window(bool weak_hit) {
    window_.push_back(weak_hit);
    while (window_.size() > cfg_.weak_window) window_.pop_front();
}

std::size_t Tier0Gate::weak_count() const {
    std::size_t n = 0;
    for (bool b : window_) if (b) ++n;
    return n;
}

void Tier0Gate::expire_if_stale(std::chrono::system_clock::time_point now) {
    // The candidate has its OWN timeout. Reusing the latch here was a defect once already: the
    // latch is not set until an event is active, so a candidate died on the first non-motion
    // observation and the K-of-N rule silently degraded to "consecutive".
    if (state_ == GateState::candidate && now >= candidate_until_) {
        state_ = GateState::quiet;
        window_.clear();
    } else if (state_ == GateState::cooldown && now >= cooldown_until_) {
        state_ = GateState::quiet;
    }
}

void Tier0Gate::reset() {
    state_ = GateState::quiet;
    window_.clear();
    first_hit_ = {};
    latch_until_ = {};
    candidate_until_ = {};
    cooldown_until_ = {};
}

TriggerEvent Tier0Gate::update(const CameraFinding& f, std::chrono::system_clock::time_point now) {
    TriggerEvent ev;
    ev.at = now;
    ev.state = state_;
    ev.motion_level = f.motion_level;
    ev.idle_band = f.idle_band;
    ev.region_travel = f.region_travel;

    // A window the DEVICE failed to deliver is a finding about our equipment, never a quiet scene.
    // It must not fire an event, and it must not silently reset state either: a stall in the middle
    // of a crossing should leave the event open, not manufacture a clean end to it.
    if (f.kind == CameraFindingKind::frames_stalled) {
        ++stats_.stalled_windows;
        ev.reason = "stalled: the device delivered no frame (not a quiet scene)";
        return ev;
    }
    if (!f.valid) {
        ++stats_.invalid_findings;
        ev.reason = "finding invalid";
        return ev;
    }
    if (f.kind == CameraFindingKind::warming_up) {
        ev.reason = "warming up: no baseline yet";
        return ev;
    }

    ++stats_.observations;

    // A finding whose character was not computed carries no geometry. Treat it as non-motion rather
    // than guessing: an unknown character must never drive a trigger.
    const ChangeCharacter character =
        f.character_valid ? change_character_from_string(f.change_character) : ChangeCharacter::unknown;
    ev.character = character;

    const bool motion = is_motion(character);
    const double strong_t = f.idle_band > 0.0 ? f.idle_band * cfg_.strong_multiplier
                                              : cfg_.uncalibrated_strong;
    const double weak_t   = f.idle_band > 0.0 ? f.idle_band * cfg_.weak_multiplier
                                              : cfg_.uncalibrated_weak;

    // Brightness is not motion, and the host's vocabulary already says so by construction. A gate
    // that re-admitted it by thresholding a scalar would fire on every light switch and on the
    // camera's own settling tail after one.
    if (character == ChangeCharacter::global_illumination ||
        character == ChangeCharacter::illumination_settling) {
        ++stats_.rejected_illumination;
        window_.clear();
        expire_if_stale(now);
        ev.state = state_;
        ev.reason = std::string("illumination, not motion (") +
                    homeguardian::change_character_to_string(character) + ")";
        return ev;
    }
    // A compact change of ONE polarity is an object appearing or a light switching on in the scene.
    // It is a change, but it is not movement, and treating it as movement is how a gate fills with
    // false triggers.
    if (character == ChangeCharacter::localized_unipolar) {
        ++stats_.rejected_unipolar;
        window_.clear();
        expire_if_stale(now);
        ev.state = state_;
        ev.reason = "unipolar local change, not motion";
        return ev;
    }

    const bool strong = motion && f.motion_level >= strong_t;
    bool weak = motion && f.motion_level >= weak_t;

    // diffuse_motion includes rain and foliage, so it must clear a higher bar than a compact
    // crossing: require that the changed mass has actually kept travelling.
    if (weak && character == ChangeCharacter::diffuse_motion && cfg_.diffuse_requires_travel &&
        f.travel_consistent < cfg_.diffuse_min_travel_consistent) {
        weak = false;
    }

    // One strong observation fires immediately. The asymmetric trigger is what makes a fast
    // subject reachable: waiting for K of N on an unmistakable signal delays the only case that
    // has no time to spare.
    if (strong) {
        if (state_ == GateState::quiet || state_ == GateState::cooldown) {
            first_hit_ = now;
            ev.fire = true;
            ev.event_id = next_event_id_++;
            ++stats_.fired;
            ev.reason = "strong motion on a single observation";
        } else {
            ev.extend = true;
            ev.event_id = next_event_id_ - 1;
            ++stats_.extended;
            ev.reason = "strong motion extends the open event";
        }
        ev.anchor = first_hit_;
        ev.was_strong = true;
        state_ = GateState::active;
        latch_until_ = now + cfg_.latch;
        push_window(true);
        ev.state = state_;
        return ev;
    }

    // Every observation advances the K-of-N window: a weak hit counts, anything else is a miss.
    push_window(weak);

    // A lone weak hit opens a candidate and records the anchor IMMEDIATELY, on the first hit rather
    // than at confirmation. The anchor is where the pre-roll must reach back to, and by the time
    // the event is confirmed the movement may already be leaving the frame.
    if (weak && (state_ == GateState::quiet || state_ == GateState::cooldown)) {
        first_hit_ = now;
        state_ = GateState::candidate;
        candidate_until_ = now + cfg_.candidate_window;
    }

    if (state_ == GateState::candidate && weak_count() >= cfg_.weak_hits_required) {
        ev.fire = true;
        ev.anchor = first_hit_;                  // the anchor predates confirmation
        ev.event_id = next_event_id_++;
        ++stats_.fired;
        state_ = GateState::active;
        latch_until_ = now + cfg_.latch;
        ev.reason = "weak motion confirmed by K of N observations";
        ev.state = state_;
        return ev;
    }

    if (state_ == GateState::active) {
        if (weak) {
            ev.extend = true;
            ev.anchor = first_hit_;
            ev.event_id = next_event_id_ - 1;
            latch_until_ = now + cfg_.latch;
            ++stats_.extended;
            ev.reason = "weak motion extends the open event";
        } else if (now < latch_until_) {
            ev.extend = true;                    // inside the latch: the event continues
            ev.anchor = first_hit_;
            ev.event_id = next_event_id_ - 1;
            ++stats_.extended;
            ev.reason = "within latch";
        } else {
            state_ = GateState::cooldown;
            cooldown_until_ = now + cfg_.cooldown;
            window_.clear();
            ev.reason = "latch expired";
        }
        ev.state = state_;
        return ev;
    }

    expire_if_stale(now);
    ev.state = state_;
    if (weak) ev.reason = "weak hit recorded, awaiting agreement";
    else { ++stats_.rejected_weak; ev.reason = "no motion"; }
    return ev;
}

}  // namespace hg::edge
