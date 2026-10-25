#include "edge/EventPreFilter.h"

#include <cstring>

namespace hg::edge {

namespace {

// Classify a suppression so the counts distinguish "nothing happened" from "the device failed"
// and from "the lighting changed". Collapsing them would let a broken camera look like a quiet
// scene, which is the one mistake this whole path exists to avoid.
enum class Suppression { illumination, unipolar, stalled, invalid, no_motion };

Suppression classify(const TriggerEvent& ev) {
    if (ev.state == GateState::cooldown || ev.state == GateState::active) return Suppression::no_motion;
    if (std::strstr(ev.reason.c_str(), "stalled") != nullptr) return Suppression::stalled;
    if (std::strstr(ev.reason.c_str(), "invalid") != nullptr) return Suppression::invalid;
    if (std::strstr(ev.reason.c_str(), "illumination") != nullptr) return Suppression::illumination;
    if (std::strstr(ev.reason.c_str(), "unipolar") != nullptr) return Suppression::unipolar;
    return Suppression::no_motion;
}

}  // namespace

PreFilterDecision EventPreFilter::submit(const homeguardian::CameraFinding& finding,
                                         std::chrono::system_clock::time_point now) {
    const TriggerEvent ev = gate_.update(finding, now);

    PreFilterDecision d;
    d.begins_event = ev.fire;
    d.continues_event = ev.extend;
    // A frame is analysed exactly when it belongs to an event. `fire` begins one; `extend` keeps
    // analysis running through it, including the quiet frames inside the latch - a continuous
    // crossing must not be analysed as a series of disconnected instants.
    d.analyse = ev.fire || ev.extend;
    d.event_id = ev.event_id;
    d.anchor = ev.anchor;
    d.state = ev.state;
    d.reason = ev.reason;

    ++stats_.frames_seen;
    if (d.analyse) {
        ++stats_.analysed;
        if (ev.fire) ++stats_.events;
    } else {
        ++stats_.skipped;
        switch (classify(ev)) {
            case Suppression::illumination: ++stats_.suppressed_illumination; break;
            case Suppression::unipolar:     ++stats_.suppressed_unipolar;     break;
            case Suppression::stalled:      ++stats_.stalled_frames;          break;
            case Suppression::invalid:      ++stats_.invalid_frames;          break;
            case Suppression::no_motion:    ++stats_.suppressed_no_motion;    break;
        }
    }
    return d;
}

void EventPreFilter::reset() {
    gate_.reset();
}

}  // namespace hg::edge
