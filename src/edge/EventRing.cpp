#include "edge/EventRing.h"

#include <algorithm>

namespace hg::edge {

const char* to_string(RetainResult r) {
    switch (r) {
        case RetainResult::stored:                return "stored";
        case RetainResult::refused_unauthorized:  return "refused_unauthorized";
        case RetainResult::refused_too_large:     return "refused_too_large";
        case RetainResult::refused_empty:         return "refused_empty";
        case RetainResult::refused_null:          return "refused_null";
    }
    return "unknown";
}

RetainResult EventRing::retain(ByteView borrowed, int width, int height,
                               std::chrono::system_clock::time_point captured_at,
                               bool keyframe, std::uint64_t sequence) {
    // Authority is re-checked on every retain, not only at admission. A withdrawal that races
    // an in-flight frame must not leave that frame retained.
    if (!authorized_) {
        ++stats_.refused;
        return RetainResult::refused_unauthorized;
    }
    // A null pointer with a nonzero size is a caller bug; refuse rather than read it.
    if (borrowed.data == nullptr && borrowed.size > 0) {
        ++stats_.refused;
        return RetainResult::refused_null;
    }
    if (borrowed.size == 0) {
        ++stats_.refused;
        return RetainResult::refused_empty;
    }
    // One oversized frame is refused rather than stored. Storing it would evict the whole
    // window to make room, so the refusal protects the history that already exists.
    if (borrowed.size > bounds_.max_frame_bytes) {
        ++stats_.refused;
        return RetainResult::refused_too_large;
    }

    RetainedFrame f;
    f.bytes.assign(borrowed.data, borrowed.data + borrowed.size);   // the ring owns its copy
    f.width = width;
    f.height = height;
    f.captured_at = captured_at;
    f.keyframe = keyframe;
    f.sequence = sequence;

    total_bytes_ += f.bytes.size();
    frames_.push_back(std::move(f));

    ++stats_.stored;
    evict_to_bounds();

    stats_.frames = frames_.size();
    stats_.bytes = total_bytes_;
    stats_.peak_bytes = std::max(stats_.peak_bytes, total_bytes_);
    stats_.peak_frames = std::max(stats_.peak_frames, frames_.size());
    return RetainResult::stored;
}

void EventRing::evict_to_bounds() {
    // Oldest first. Bounded on bytes AND count: a bound on only one of them is not a bound,
    // because many tiny frames breach the count and one medium frame breaches the bytes.
    while (!frames_.empty() &&
           (frames_.size() > bounds_.max_frames || total_bytes_ > bounds_.max_total_bytes)) {
        total_bytes_ -= frames_.front().bytes.size();
        frames_.pop_front();
        ++stats_.evicted_by_bytes;
        ++stats_.evicted_by_count;
    }
}

void EventRing::expire(std::chrono::system_clock::time_point now) {
    // Age is a third independent bound. A quiet period must not let an old window persist
    // indefinitely just because the byte and count budgets are slack.
    const auto cutoff = now - bounds_.max_age;
    while (!frames_.empty() && frames_.front().captured_at < cutoff) {
        total_bytes_ -= frames_.front().bytes.size();
        frames_.pop_front();
        ++stats_.evicted_by_age;
    }
    stats_.frames = frames_.size();
    stats_.bytes = total_bytes_;
}

void EventRing::clear() {
    // Deterministic: after this the ring holds nothing, so no in-flight decode can be handed
    // pre-withdrawal media. Stats are cumulative counters and are deliberately retained for
    // observability, but the size fields reset with the data.
    frames_.clear();
    total_bytes_ = 0;
    stats_.frames = 0;
    stats_.bytes = 0;
}

void EventRing::set_authorized(bool authorized) {
    authorized_ = authorized;
    if (!authorized_) {
        // Withdrawal clears retained data immediately. Queued work is invalidated by the
        // caller observing authorized() == false before it encodes or publishes anything.
        clear();
    }
}

RingWindow EventRing::window(std::chrono::system_clock::time_point from,
                             std::chrono::system_clock::time_point to) const {
    RingWindow w;
    w.requested_from = from;
    w.requested_to = to;

    if (!authorized_) {
        w.reason = "authority withdrawn";
        return w;
    }
    if (to < from) {
        w.reason = "requested range is inverted";
        return w;
    }

    for (const auto& f : frames_) {
        if (f.captured_at < from) continue;
        if (f.captured_at > to) break;
        w.frames.push_back(f);   // copies; the caller owns what it decodes
    }

    if (w.frames.empty()) {
        w.reason = frames_.empty() ? "ring holds nothing" : "nothing retained in that range";
        return w;
    }

    w.ok = true;
    // A clean decode can only begin at a random-access point. Reporting false is the honest
    // answer when the window starts mid-GOP, rather than silently decoding from a P frame
    // and producing corrupt output.
    w.starts_at_keyframe = w.frames.front().keyframe;
    // The range is complete only if the ring's oldest retained frame is at or before `from`.
    // Otherwise earlier history was already evicted and the caller must not believe it has
    // everything it asked for.
    w.complete = (frames_.front().captured_at <= from);
    if (!w.complete) {
        w.reason = "earlier history already evicted; the range is not fully retained";
    }
    return w;
}

bool EventRing::span(std::chrono::system_clock::time_point& oldest,
                     std::chrono::system_clock::time_point& newest) const {
    if (frames_.empty()) return false;
    oldest = frames_.front().captured_at;
    newest = frames_.back().captured_at;
    return true;
}

}  // namespace hg::edge
