// EventRing — bounded pre-event retention of compressed frame bytes.
//
// Out-of-tree prototype of the component specified in
// docs/EDGE_VIDEO_INTERFACE_SPECIFICATION.md section 5.1. Mirrors the intended repository
// layout (src/edge/EventRing.h) so it can be moved in unchanged once the gate opens.
//
// Design constraints taken from the interface specification and the integration ruling:
//
//   * The acquisition callback BORROWS immutable bytes for the duration of the call. The ring
//     owns any retained copy. Nothing here may outlive the call holding a borrowed pointer.
//   * Bounded on FOUR axes: total bytes, frame count, age, and per-call work. A ring bounded on
//     only some of those grows without limit on the others.
//   * Retention must exceed the GOP, or a decode cannot start at a random-access point.
//   * Missing history is NEVER interpolated and presented as captured frames. A gap stays a gap
//     and is reported as such.
//   * Authorization is re-checked before retaining, not only at admission. Withdrawal clears
//     retained data and invalidates queued work.
//   * No network and no disk work happens here. This is memory only.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace hg::edge {

// A non-owning view of bytes borrowed for the duration of a call.
struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// One retained frame. Owns its bytes.
struct RetainedFrame {
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    std::chrono::system_clock::time_point captured_at{};
    bool keyframe = false;   // a random-access point: a decode may begin here
    std::uint64_t sequence = 0;
};

struct RingBounds {
    std::size_t max_total_bytes = 4u * 1024u * 1024u;   // 4 MiB, per the low-memory budget
    std::size_t max_frames = 240;
    std::chrono::milliseconds max_age{4000};            // > pre-roll + GOP + safety
    // A single frame larger than this is refused rather than stored, so one pathological
    // frame cannot evict the entire window. Mirrors the acquisition-side size guard.
    std::size_t max_frame_bytes = 512u * 1024u;
};

// Why a retain() call was refused. Returned rather than muted, so the caller can count.
enum class RetainResult {
    stored,
    refused_unauthorized,   // no consent / authority withdrawn
    refused_too_large,      // a single frame over max_frame_bytes
    refused_empty,          // zero-length payload
    refused_null,           // null pointer, or size with no data
};

const char* to_string(RetainResult r);

// What a retrospective decode is given. Deliberately reports its own limits.
struct RingWindow {
    bool ok = false;
    std::string reason;                  // populated when ok == false
    std::vector<RetainedFrame> frames;   // in capture order
    bool starts_at_keyframe = false;     // false => a decode cannot begin cleanly here
    bool complete = false;               // false => the requested range is not fully retained
    std::chrono::system_clock::time_point requested_from{};
    std::chrono::system_clock::time_point requested_to{};
};

struct RingStats {
    std::size_t frames = 0;
    std::size_t bytes = 0;
    std::uint64_t stored = 0;
    std::uint64_t evicted_by_bytes = 0;
    std::uint64_t evicted_by_count = 0;
    std::uint64_t evicted_by_age = 0;
    std::uint64_t refused = 0;
    std::size_t peak_bytes = 0;
    std::size_t peak_frames = 0;
};

class EventRing {
public:
    explicit EventRing(RingBounds bounds = {}) : bounds_(bounds) {}

    // Called from the borrowed acquisition callback. Copies what it retains; borrows nothing
    // beyond the call.
    RetainResult retain(ByteView borrowed, int width, int height,
                        std::chrono::system_clock::time_point captured_at,
                        bool keyframe, std::uint64_t sequence);

    // Consent withdrawal, source change, shutdown. Idempotent and deterministic.
    void clear();

    // Authority re-checked on every retain. Setting false clears immediately.
    void set_authorized(bool authorized);
    bool authorized() const { return authorized_; }

    // The retained window covering [from, to]. Never fabricates frames for a gap.
    RingWindow window(std::chrono::system_clock::time_point from,
                      std::chrono::system_clock::time_point to) const;

    // Drop anything older than the bound relative to `now`. Called by the owner; not done
    // implicitly on read, so a read cannot change retention.
    void expire(std::chrono::system_clock::time_point now);

    const RingStats& stats() const { return stats_; }
    const RingBounds& bounds() const { return bounds_; }

    // The oldest and newest capture times retained, for sizing the pre-roll.
    bool span(std::chrono::system_clock::time_point& oldest,
              std::chrono::system_clock::time_point& newest) const;

private:
    void evict_to_bounds();
    std::size_t total_bytes_ = 0;

    RingBounds bounds_;
    bool authorized_ = false;
    std::deque<RetainedFrame> frames_;
    RingStats stats_{};
};

}  // namespace hg::edge
