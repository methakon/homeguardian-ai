// EventPreFilter — the insertion point between detection and analysis.
//
// WHERE THIS SITS, AND WHY IT EXISTS
// ----------------------------------
// The analyzer already detects change: `CameraActivitySession::ingest` runs the existing
// extractor per frame and classifies the change with the existing ChangeCharacter vocabulary.
// It also already produces an Observation for EVERY frame at the configured cadence, and the
// fusion engine consumes all of them. That is the cost this component exists to cut.
//
//     video feed
//        -> CameraActivitySession::ingest()      detection (exists, unchanged, runs per frame)
//        -> CameraFinding                        the verdict for this frame
//        -> [EventPreFilter]                     <-- THIS: decide whether it is an event
//        -> Observation                          only for gated events
//        -> FusionEngine                         expensive analysis, now rare
//
// So this is a pre-filter over the ANALYSIS, not over the frames. The cheap per-frame detection
// is upstream and stays where it is; what changes is that an observation is no longer built for
// every frame regardless of what it contains.
//
// WHAT IT DOES NOT DO
// -------------------
// It performs no image work of its own: no pixel comparison, no decode, no resize. It owns no
// camera, credential, socket or file. It consumes the host's finding and answers one question -
// is this frame part of an event worth analysing - with a reason attached either way.
//
// WHY THE REASON MATTERS
// ----------------------
// "Nothing was analysed" and "the analyser was not asked" are different facts, and a quiet scene
// must not look like a broken detector. Every decision carries why, and the counts separate
// ordinary non-motion from illumination, from a stalled device, from an invalid finding.
#pragma once

#include "core/CameraActivitySession.h"
#include "edge/Tier0Gate.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace hg::edge {

// The pre-filter's answer for one frame.
struct PreFilterDecision {
    bool analyse = false;                       // should the analysis process see this frame?
    bool begins_event = false;                  // this frame starts an event
    bool continues_event = false;               // it is part of the event already open
    std::uint64_t event_id = 0;
    // The look-back anchor: where the event actually began, so a clip can reach back to it.
    // Populated on the frame that begins the event and on every frame that continues it.
    std::chrono::system_clock::time_point anchor{};
    GateState state = GateState::quiet;
    std::string reason;                         // why it was analysed, or why it was not
};

// Counts, so a quiet scene can be told apart from a broken one.
struct PreFilterStats {
    std::uint64_t frames_seen = 0;
    std::uint64_t analysed = 0;
    std::uint64_t skipped = 0;
    std::uint64_t events = 0;
    std::uint64_t suppressed_illumination = 0;
    std::uint64_t suppressed_unipolar = 0;
    std::uint64_t suppressed_no_motion = 0;
    std::uint64_t stalled_frames = 0;
    std::uint64_t invalid_frames = 0;

    // The saving, stated as a ratio rather than a claim. 0 when nothing was seen.
    double analysis_ratio() const {
        if (frames_seen == 0) return 0.0;
        return static_cast<double>(analysed) / static_cast<double>(frames_seen);
    }
};

class EventPreFilter {
public:
    explicit EventPreFilter(GateConfig cfg = {}) : gate_(cfg) {}

    // One frame's verdict in, one decision out. Called from the capture loop where the analyzer
    // would otherwise build an Observation unconditionally.
    PreFilterDecision submit(const homeguardian::CameraFinding& finding,
                             std::chrono::system_clock::time_point now);

    const PreFilterStats& stats() const { return stats_; }
    const GateStats& gate_stats() const { return gate_.stats(); }
    const GateConfig& config() const { return gate_.config(); }
    GateState state() const { return gate_.state(); }

    // Authorization withdrawn, source changed, or shutdown. Closes any open event so analysis
    // cannot continue on an event that no longer has authority behind it.
    void reset();

private:
    Tier0Gate gate_;
    PreFilterStats stats_{};
};

}  // namespace hg::edge
