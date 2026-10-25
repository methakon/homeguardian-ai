#include "edge/RecordingFrameConsumer.h"

namespace hg::edge {

void RecordingFrameConsumer::on_frame(const BorrowedFrame& frame) {
    // The ring consumer owns admission: validation, byte ceiling, geometry, and the copy. Delegating
    // rather than re-checking keeps ONE definition of what a retainable frame is, so the two cannot
    // drift into disagreeing about the same payload.
    ring_consumer_.on_frame(frame);
}

void RecordingFrameConsumer::on_authority_changed(bool authorized) {
    // The ring is cleared through its own consumer, which forwards to `set_authorized` - one
    // implementation of the clearing rule, not a second one here.
    ring_consumer_.on_authority_changed(authorized);

    std::lock_guard<std::mutex> guard(m_);
    if (authorized) {
        ++lifecycle_.authorize_notifications;
        // Deliberately NO decrement of the generation. Authority returning does not resurrect the
        // work that was conceived before it left: that work carries a token from an earlier
        // generation and stays invalid. Only new work, started now, is current.
        return;
    }

    ++lifecycle_.withdraw_notifications;

    // Queued work is dropped by the queue's own primitive.
    queue_.discard_pending();
    ++lifecycle_.discarded_pending;

    // In-flight work cannot be reached by discard_pending: it has already left the queue. Bumping
    // the generation is what invalidates it, because the token it captured no longer names the
    // current generation - so it can neither publish nor be published on its behalf.
    ++generation_;
    ++lifecycle_.invalidations;
}

RecordingFrameConsumer::Token RecordingFrameConsumer::generation() const {
    std::lock_guard<std::mutex> guard(m_);
    return generation_;
}

RecordingFrameConsumer::Token RecordingFrameConsumer::current_token() const {
    return generation();
}

bool RecordingFrameConsumer::superseded(Token token) const {
    std::lock_guard<std::mutex> guard(m_);
    return token != generation_;
}

std::function<bool()> RecordingFrameConsumer::cancellation_for(Token token) const {
    // Captured by value, so the predicate outlives nothing and touches nothing borrowed. Reading the
    // generation under the lock each call is what makes it correct under a concurrent withdrawal.
    return [this, token] { return superseded(token); };
}

bool RecordingFrameConsumer::publish_if_current(Token token, const std::function<void()>& publish) {
    {
        std::lock_guard<std::mutex> guard(m_);
        if (token != generation_) {
            ++lifecycle_.stale_publishes_refused;
            return false;
        }
    }
    // Called OUTSIDE the lock: publishing may be slow, and holding the mutex across it would make
    // a withdrawal wait on the very work it is trying to invalidate. The window this opens is
    // closed by the caller's own cancellation predicate being polled during the work; this method
    // guarantees only that a publish begun AFTER a withdrawal is refused.
    publish();
    return true;
}

RecordingFrameConsumer::Lifecycle RecordingFrameConsumer::lifecycle() const {
    std::lock_guard<std::mutex> guard(m_);
    return lifecycle_;
}

}  // namespace hg::edge
