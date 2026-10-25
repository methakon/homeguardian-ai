// FrameConsumer — the narrow interface between the acquisition path and the pre-event ring.
//
// WHY THIS EXISTS
// ---------------
// The ring retains compressed frame bytes so an event can be analysed retrospectively. It cannot
// fetch them: acquisition, credentials and transport belong to the existing camera path. This
// header is therefore the entire contract between the two, and nothing more - the ring never
// learns how a frame was obtained, and the acquisition path never learns what the ring does
// with it.
//
// THE BORROW
// ----------
// `on_frame` is handed bytes that are borrowed for the duration of the call only. The callee owns
// any copy it intends to keep. This is stated in the type rather than in a comment because the
// alternative - retaining a pointer past the call - is a use-after-free that would appear as
// intermittently corrupt frames rather than as a crash.
//
// VALIDATION IS THE CALLER'S OBLIGATION, AND IT IS ENFORCED HERE
// -------------------------------------------------------------
// The acquisition path's existing size/header/dimension guard runs INSIDE its JPEG decoder, so
// bytes presented BEFORE that decoder are unvalidated. `BorrowedFrame::validated` states that the
// caller applied equivalent checks first. The ring refuses a frame that does not carry it.
//
// That refusal is deliberate. Without it, a hook placed before the decoder would be a way to make
// the service retain arbitrary caller-supplied bytes, and the ring would be the instrument. A
// guard that the callee enforces cannot be forgotten by a future call site.
#pragma once

#include "edge/EventRing.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hg::edge {

// Limits the caller must have checked before setting `validated`. Mirrored here so the callee can
// state them and a test can exercise the boundary.
struct FrameLimits {
    std::size_t max_bytes = 512u * 1024u;   // the existing guard's ceiling
    std::uint32_t max_dimension = 4096;     // the existing guard's dimension ceiling
};

// Bytes borrowed for the duration of one call. The callee must copy anything it retains.
struct BorrowedFrame {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    int width = 0;      // DERIVED FROM CHECKED HEADERS by the caller, never assumed
    int height = 0;
    // The ACTUAL capture time, not the time the analysis entry point happened to be called. A
    // retainer that used the latter would anchor every clip late by the queueing delay.
    std::chrono::system_clock::time_point captured_at{};
    std::uint64_t sequence = 0;   // acquisition sequence, so a gap is visible rather than smoothed
    // The caller asserts that size, magic bytes, dimensions and the dimension ceiling were
    // checked BEFORE this call. False means the ring will refuse to retain it.
    bool validated = false;
};

class FrameConsumer {
public:
    virtual ~FrameConsumer() = default;

    // Called once per acquired frame, on the acquisition path. `frame` and the bytes it points at
    // are valid only until this returns.
    virtual void on_frame(const BorrowedFrame& frame) = 0;

    // Called on authorization withdrawal, source or enrollment change, and lifecycle stop. The
    // callee must clear retained data and invalidate queued work; nothing further should be
    // encoded or published on the strength of what it already holds.
    virtual void on_authority_changed(bool authorized) = 0;
};

// Binds a FrameConsumer to an EventRing. Kept separate from both so each is testable alone, and
// so the ring does not need to know that acquisition exists.
class RingFrameConsumer : public FrameConsumer {
public:
    RingFrameConsumer(EventRing& ring, FrameLimits limits = {}) : ring_(ring), limits_(limits) {}

    void on_frame(const BorrowedFrame& frame) override;
    void on_authority_changed(bool authorized) override;

    // Refusals, separately counted so an over-cap payload is distinguishable from a caller that
    // never ran validation, and both from an ordinary oversized frame.
    struct Counts {
        std::uint64_t accepted = 0;
        std::uint64_t refused_unvalidated = 0;
        std::uint64_t refused_oversize = 0;
        std::uint64_t refused_bad_geometry = 0;
        std::uint64_t refused_null = 0;
    };
    const Counts& counts() const { return counts_; }

private:
    EventRing& ring_;
    FrameLimits limits_;
    Counts counts_{};
};

}  // namespace hg::edge
