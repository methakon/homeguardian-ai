#pragma once

// BoundedMediaBuffer: where raw media lives, and the edge of where it is allowed to go.
//
// THE RULE THIS CLASS ENFORCES
//   Frames and PCM stay ON the machine that acquired them. What leaves is a bounded derived observation
//   through the network contract. This header therefore deliberately includes NO JSON header and defines no
//   serialisation for a frame: a compile-time assertion in the tests pins that a MediaFrame cannot be
//   turned into an observation payload, so the boundary is a property of the type rather than a convention
//   someone has to remember.
//
// WHY BOUNDED, AND WHAT HAPPENS WHEN IT IS FULL
//   A camera produces frames whether or not anything consumes them. An unbounded queue turns a slow
//   consumer into an out-of-memory failure, so the buffer has a frame ceiling AND a byte ceiling. When it
//   is full the policy is explicit: live media drops the OLDEST frame (the newest is what an inference step
//   wants) and counts the drop. Rejection with a code is available for callers that would rather lose the
//   new frame than the old one, and either way the drop is accounted rather than silent.
//
// WHAT A DISCONNECT MEANS
//   When the producer goes away the buffer stops accepting new frames and drains what it already holds -
//   buffered frames were really captured, so discarding them would throw away evidence, while inventing
//   more would fabricate it. An empty buffer is empty: dequeue returns false, never a blank frame.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace homeguardian {

// One locally-held media unit. No JSON, no to_json, no serialisation of any kind.
struct MediaFrame {
    uint64_t frame_id = 0;
    std::chrono::system_clock::time_point captured_at{};
    std::vector<uint8_t> bytes{};
};

enum class MediaEnqueueOutcome {
    enqueued,            // stored
    dropped_oldest,      // stored, and the oldest frame was discarded to make room
    rejected_full,       // not stored: the queue is full and the policy keeps the old frames
    rejected_oversized,  // not stored: this single frame exceeds the per-frame ceiling
    rejected_disconnected,  // not stored: the producer is gone, and no frame is invented
};

const char* media_enqueue_outcome_to_string(MediaEnqueueOutcome outcome);

struct MediaBufferConfig {
    size_t max_frames = 8;                     // queue depth ceiling
    size_t max_bytes = 4u * 1024u * 1024u;     // memory ceiling for buffered payloads
    size_t max_frame_bytes = 1u * 1024u * 1024u;  // a single frame cannot exceed this
    bool drop_oldest_when_full = true;         // live media: the newest frame matters most
};

struct MediaBufferStats {
    uint64_t enqueued = 0;
    uint64_t dequeued = 0;
    uint64_t dropped_oldest = 0;
    uint64_t rejected_full = 0;
    uint64_t rejected_oversized = 0;
    uint64_t rejected_disconnected = 0;
    uint64_t purged = 0;              // discarded because consent was withdrawn
    uint64_t purged_at_shutdown = 0;  // discarded by a graceful shutdown
    uint64_t bytes_held = 0;          // current
    uint64_t bytes_high_water = 0;    // maximum ever held: the memory ceiling that was actually reached
    uint64_t frames_high_water = 0;
};

class BoundedMediaBuffer {
public:
    explicit BoundedMediaBuffer(MediaBufferConfig cfg = MediaBufferConfig{});

    MediaEnqueueOutcome enqueue(MediaFrame frame, std::string* reason = nullptr);

    // Take the oldest buffered frame. Returns false when nothing is buffered - never a placeholder frame.
    bool dequeue(MediaFrame* out);

    // Consent withdrawn: buffered media of a revoked subject must not survive the withdrawal. Returns how
    // many frames were discarded, and leaves the buffer empty.
    size_t purge(std::string* reason = nullptr);

    // Graceful shutdown: stop accepting, release everything held, and report what was discarded. Media is
    // dropped rather than flushed to anywhere - a shutdown is not a reason to move frames out of the
    // process that acquired them.
    size_t shutdown();

    // The producer disconnected: further frames are refused with its own code, and what is already held can
    // still be drained, because those frames were really captured.
    void mark_producer_disconnected() { producer_connected_ = false; }
    bool producer_connected() const { return producer_connected_; }

    size_t size() const { return queue_.size(); }
    const MediaBufferStats& stats() const { return stats_; }
    // True when the buffer cannot take another frame of this size right now: the caller's backpressure signal.
    bool would_reject(size_t frame_bytes) const;
    // Bounded, read-only view for a consumer that processes in place: never a copy out of the process.
    const MediaFrame* peek_oldest() const { return queue_.empty() ? nullptr : &queue_.front(); }

private:
    MediaBufferConfig cfg_{};
    std::deque<MediaFrame> queue_{};
    MediaBufferStats stats_{};
    bool producer_connected_ = true;
};

}  // namespace homeguardian
