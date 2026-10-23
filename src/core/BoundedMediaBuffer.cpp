#include "core/BoundedMediaBuffer.h"

namespace homeguardian {

const char* media_enqueue_outcome_to_string(MediaEnqueueOutcome outcome) {
    switch (outcome) {
        case MediaEnqueueOutcome::enqueued:              return "enqueued";
        case MediaEnqueueOutcome::dropped_oldest:        return "dropped_oldest";
        case MediaEnqueueOutcome::rejected_full:         return "rejected_full";
        case MediaEnqueueOutcome::rejected_oversized:    return "rejected_oversized";
        case MediaEnqueueOutcome::rejected_disconnected: return "rejected_disconnected";
    }
    return "rejected_full";
}

BoundedMediaBuffer::BoundedMediaBuffer(MediaBufferConfig cfg) : cfg_(cfg) {
    if (cfg_.max_frames == 0) cfg_.max_frames = 1;
    if (cfg_.max_bytes == 0) cfg_.max_bytes = 1;
    if (cfg_.max_frame_bytes == 0) cfg_.max_frame_bytes = 1;
}

bool BoundedMediaBuffer::would_reject(size_t frame_bytes) const {
    if (!producer_connected_) return true;
    if (frame_bytes > cfg_.max_frame_bytes) return true;
    if (queue_.size() >= cfg_.max_frames) return !cfg_.drop_oldest_when_full;
    return stats_.bytes_held + frame_bytes > cfg_.max_bytes;
}

MediaEnqueueOutcome BoundedMediaBuffer::enqueue(MediaFrame frame, std::string* reason) {
    const auto fail = [&](MediaEnqueueOutcome outcome, const std::string& why) {
        if (reason != nullptr) *reason = why;
        return outcome;
    };

    if (!producer_connected_) {
        ++stats_.rejected_disconnected;
        return fail(MediaEnqueueOutcome::rejected_disconnected,
                    "the producer disconnected: no frame is invented to fill the gap");
    }
    const size_t incoming = frame.bytes.size();
    if (incoming > cfg_.max_frame_bytes) {
        ++stats_.rejected_oversized;
        return fail(MediaEnqueueOutcome::rejected_oversized,
                    "frame of " + std::to_string(incoming) + " bytes exceeds the per-frame ceiling of " +
                        std::to_string(cfg_.max_frame_bytes));
    }

    // Make room: the frame ceiling first, then the byte ceiling. Both are hard.
    bool dropped = false;
    while (!queue_.empty() &&
           (queue_.size() >= cfg_.max_frames || stats_.bytes_held + incoming > cfg_.max_bytes)) {
        if (!cfg_.drop_oldest_when_full) {
            ++stats_.rejected_full;
            return fail(MediaEnqueueOutcome::rejected_full,
                        "the buffer is full and the policy keeps the oldest frames");
        }
        stats_.bytes_held -= queue_.front().bytes.size();
        queue_.pop_front();
        ++stats_.dropped_oldest;
        dropped = true;
    }
    if (incoming > cfg_.max_bytes) {
        // A frame that cannot fit even in an empty buffer is a configuration error, not a queue state.
        ++stats_.rejected_oversized;
        return fail(MediaEnqueueOutcome::rejected_oversized,
                    "frame is larger than the whole byte ceiling");
    }

    stats_.bytes_held += incoming;
    queue_.push_back(std::move(frame));
    ++stats_.enqueued;
    if (stats_.bytes_held > stats_.bytes_high_water) stats_.bytes_high_water = stats_.bytes_held;
    if (queue_.size() > stats_.frames_high_water) stats_.frames_high_water = queue_.size();
    return dropped ? MediaEnqueueOutcome::dropped_oldest : MediaEnqueueOutcome::enqueued;
}

size_t BoundedMediaBuffer::purge(std::string* reason) {
    const size_t discarded = queue_.size();
    queue_.clear();
    stats_.bytes_held = 0;
    stats_.purged += discarded;
    if (reason != nullptr) {
        *reason = "consent withdrawn: " + std::to_string(discarded) +
                  " buffered frame(s) discarded, and the buffer is empty";
    }
    return discarded;
}

size_t BoundedMediaBuffer::shutdown() {
    producer_connected_ = false;
    const size_t discarded = queue_.size();
    queue_.clear();
    stats_.bytes_held = 0;
    stats_.purged_at_shutdown += discarded;
    return discarded;
}

bool BoundedMediaBuffer::dequeue(MediaFrame* out) {
    if (out == nullptr || queue_.empty()) return false;   // empty is empty; no placeholder
    *out = std::move(queue_.front());
    queue_.pop_front();
    stats_.bytes_held -= out->bytes.size();
    ++stats_.dequeued;
    return true;
}

}  // namespace homeguardian
