#include "edge/FrameConsumer.h"

namespace hg::edge {

void RingFrameConsumer::on_frame(const BorrowedFrame& frame) {
    // Refuse a null or empty payload rather than reading it.
    if (frame.data == nullptr || frame.size == 0) {
        ++counts_.refused_null;
        return;
    }
    // The caller did not assert that the pre-decode validation ran. Refuse. This is the guard
    // that makes a hook placed before the decoder safe: the service cannot be made to retain
    // bytes nobody checked, because the ring will not accept them without the assertion.
    if (!frame.validated) {
        ++counts_.refused_unvalidated;
        return;
    }
    // The caller's own limits, re-checked. A caller that validated against different limits than
    // the ones it declared is refused rather than trusted.
    if (frame.size > limits_.max_bytes) {
        ++counts_.refused_oversize;
        return;
    }
    if (frame.width <= 0 || frame.height <= 0 ||
        static_cast<std::uint32_t>(frame.width) > limits_.max_dimension ||
        static_cast<std::uint32_t>(frame.height) > limits_.max_dimension) {
        ++counts_.refused_bad_geometry;
        return;
    }

    // The ring copies. Nothing here outlives the call, so the borrowed pointer is never retained.
    const RetainResult r = ring_.retain(ByteView{frame.data, frame.size},
                                        frame.width, frame.height,
                                        frame.captured_at,
                                        /*keyframe=*/true,     // an independent JPEG is a
                                                              // random-access point by construction
                                        frame.sequence);
    if (r == RetainResult::stored) ++counts_.accepted;
    else if (r == RetainResult::refused_too_large) ++counts_.refused_oversize;
    else if (r == RetainResult::refused_null || r == RetainResult::refused_empty) ++counts_.refused_null;
    // refused_unauthorized is the ring's own authority check, deliberately not re-counted here:
    // it is reported through the ring's stats, and double-counting it would make the two
    // disagree about the same event.
}

void RingFrameConsumer::on_authority_changed(bool authorized) {
    // The ring clears retained data on withdrawal itself. Forwarding rather than duplicating
    // keeps one implementation of the clearing rule.
    ring_.set_authorized(authorized);
}

}  // namespace hg::edge
