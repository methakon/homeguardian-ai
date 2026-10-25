// RecordingFrameConsumer — the queue-owning adapter the acquisition handoff requires.
//
// WHY THIS EXISTS AND WHY IT IS NOT RingFrameConsumer
// ---------------------------------------------------
// `RingFrameConsumer` binds acquisition to the ring and nothing else. The handoff for the pre-decode
// callback states the obligation for a consumer that ALSO owns queued work:
//
//   "The consumer must clear retained bytes and invalidate queued work on false. The actual
//    RingFrameConsumer clears its EventRing; a recorder/queue-owning adapter must also invalidate
//    its own queued/in-flight generation."
//
// Discarding pending work is not enough on its own. A job already IN FLIGHT has left the queue, so
// `discard_pending()` cannot reach it, and a clip already being encoded would otherwise be
// published on the strength of authority that was withdrawn while it ran. That is the gap this
// adapter closes, and it is why the unit of invalidation is a GENERATION rather than a queue.
//
// HOW THE GENERATION WORKS
// ------------------------
// Every withdrawal bumps `generation_`. Work captures the generation it STARTED under - a token -
// and the adapter will not publish on a token that is no longer current, whether the work was
// queued, mid-encode, or about to write. A handler that is long-running is additionally handed a
// cancellation predicate so it can abandon promptly instead of running to completion and being
// discarded at the end; the token makes correctness certain, the predicate makes it cheap.
//
// The token is monotonic and never reused, so a stale token cannot accidentally name a later
// generation. Nothing here restores a previous generation: an authorization that returns is a NEW
// generation, never a resurrection of the one that was withdrawn.
#pragma once

#include "edge/EventJobQueue.h"
#include "edge/EventRing.h"
#include "edge/FrameConsumer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace hg::edge {

class RecordingFrameConsumer : public FrameConsumer {
public:
    // A generation token. Opaque to callers: they may compare it and hand it back, not interpret it.
    using Token = std::uint64_t;

    RecordingFrameConsumer(EventRing& ring, EventJobQueue& queue, FrameLimits limits = {})
        : ring_(ring), queue_(queue), limits_(limits), ring_consumer_(ring, limits), generation_(1) {}

    // Admission and refusal are the ring consumer's rules, not a second copy of them. That
    // component already decides what a validated frame is; this one adds only the queue's lifecycle.
    void on_frame(const BorrowedFrame& frame) override;
    void on_authority_changed(bool authorized) override;

    // The generation retention is currently serving. Starts at 1 so that a default-constructed
    // token (0) is never accidentally current.
    Token generation() const;

    // The token a handler captures when it STARTS work. Hand this back on completion.
    Token current_token() const;

    // True when the token no longer names the current generation - the work it belongs to was
    // conceived under authority that has since been withdrawn.
    bool superseded(Token token) const;

    // Hand to a long-running handler (EventClipRecorder::record takes exactly this) so it abandons
    // promptly rather than encoding a clip it will not be allowed to publish.
    std::function<bool()> cancellation_for(Token token) const;

    // Run `publish` only while `token` is current. Returns true when it ran.
    //
    // This is the guard that makes the obligation real: a caller cannot forget to check, because the
    // check is the only way to publish. A caller who publishes directly, bypassing this, is the
    // one thing this adapter cannot prevent - which is why the contract is stated in the type and
    // exercised by a test rather than assumed.
    bool publish_if_current(Token token, const std::function<void()>& publish);

    const RingFrameConsumer::Counts& counts() const { return ring_consumer_.counts(); }
    const FrameLimits& limits() const { return limits_; }

    struct Lifecycle {
        std::uint64_t authorize_notifications = 0;
        std::uint64_t withdraw_notifications = 0;
        std::uint64_t invalidations = 0;         // withdrawal of work that was in flight
        std::uint64_t discarded_pending = 0;     // jobs dropped from the queue
        std::uint64_t stale_publishes_refused = 0;
    };
    Lifecycle lifecycle() const;

private:
    EventRing& ring_;
    EventJobQueue& queue_;
    FrameLimits limits_;
    RingFrameConsumer ring_consumer_;

    mutable std::mutex m_;
    Token generation_;
    Lifecycle lifecycle_{};
};

}  // namespace hg::edge
