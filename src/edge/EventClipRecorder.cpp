#include "edge/EventClipRecorder.h"

#include "edge/Md5.h"

#include <algorithm>

namespace hg::edge {

namespace {

// Fails closed and guarantees the encoder is abandoned. Every early return goes through this, so
// there is no path that leaves a half-built clip in the encoder or hands out partial bytes.
ClipResult fail(ClipResult r, VideoEncoder& enc, std::string reason) {
    enc.abandon();
    r.ok = false;
    r.reason = std::move(reason);
    r.bytes.clear();
    r.size_bytes = 0;
    r.content_md5.clear();
    return r;
}

std::string ms_to_string(std::chrono::milliseconds d) {
    return std::to_string(static_cast<long long>(d.count()));
}

}  // namespace

ClipResult EventClipRecorder::record(const ClipRequest& request,
                                     const std::function<bool()>& cancelled) {
    ClipResult r;
    r.event_id = request.event_id;

    // Authority is checked before anything is decoded. A recorder that only checked at admission
    // could publish a clip from media retained before a withdrawal.
    if (!ring_.authorized()) {
        r.reason = "authority withdrawn";
        return r;
    }

    r.clip_start = request.anchor - request.pre_roll;
    r.clip_end = request.event_end;
    if (r.clip_end < r.clip_start) {
        r.reason = "clip range is inverted";
        return r;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(r.clip_end - r.clip_start);
    if (duration > bounds_.max_duration) {
        r.reason = "requested duration " + ms_to_string(duration) + " ms exceeds the bound";
        return r;
    }

    // Reach BACK past the clip start to find a random-access point. Without this the decode begins
    // at an arbitrary frame and produces corrupt output.
    const auto source_from = r.clip_start - request.keyframe_search;
    const RingWindow w = ring_.window(source_from, r.clip_end);
    if (!w.ok) {
        r.reason = "no retained window: " + w.reason;
        return r;
    }
    r.range_complete = w.complete;
    r.source_frames_considered = w.frames.size();

    if (w.frames.size() > bounds_.max_source_frames) {
        // Refused rather than silently trimmed: trimming would change what the clip covers, and a
        // caller that asked for more than the decode bound is a misconfiguration, not a busy moment.
        r.reason = "source window of " + std::to_string(w.frames.size()) +
                   " frames exceeds the decode bound of " +
                   std::to_string(bounds_.max_source_frames);
        return r;
    }

    // The last random-access point at or before the clip start. Searching backwards, so a clip
    // whose start falls just after a keyframe still finds the keyframe rather than failing.
    std::size_t key_index = w.frames.size();
    for (std::size_t i = w.frames.size(); i-- > 0;) {
        if (w.frames[i].keyframe && w.frames[i].captured_at <= r.clip_start) {
            key_index = i;
            break;
        }
    }
    if (key_index == w.frames.size()) {
        r.reason = "no random-access point at or before the clip start within the search window";
        return r;
    }
    r.starts_at_keyframe = true;

    const int width = w.frames[key_index].width;
    const int height = w.frames[key_index].height;
    if (width <= 0 || height <= 0) {
        r.reason = "keyframe carries no usable geometry";
        return r;
    }
    r.width = width;
    r.height = height;

    if (!encoder_.begin(width, height)) {
        // Abandoned even though begin() reported failure: a begin that failed part-way may hold
        // state, and "every failure path abandons" is a rule that cannot be reasoned away
        // case-by-case without missing one. The test caught exactly that.
        return fail(std::move(r), encoder_, "encoder refused to begin");
    }

    std::vector<std::uint8_t> out;
    for (std::size_t i = key_index; i < w.frames.size(); ++i) {
        if (cancelled && cancelled()) {
            return fail(std::move(r), encoder_, "cancelled");
        }
        DecodedFrame df;
        if (!decoder_.decode(w.frames[i], df)) {
            ++r.decode_failures;
            continue;
        }
        ++r.frames_decoded;

        // RULE 4, second half: output decoded from the keyframe but BEFORE the clip start is
        // discarded. It was needed to reconstruct the stream, not to appear in the clip.
        if (df.captured_at < r.clip_start) {
            ++r.frames_discarded_before_start;
            continue;
        }
        if (!encoder_.add(df)) {
            return fail(std::move(r), encoder_, "encoder refused a frame");
        }
        ++r.frames_encoded;
    }

    if (r.frames_encoded < bounds_.min_encoded_frames) {
        return fail(std::move(r), encoder_,
                    "only " + std::to_string(r.frames_encoded) + " frame(s) after the clip start");
    }

    // The artifact must exist before it is measured. Nothing here predicts a size or hashes a
    // partial stream.
    if (!encoder_.finalize(out)) {
        return fail(std::move(r), encoder_, "finalisation failed");
    }
    if (out.empty()) {
        return fail(std::move(r), encoder_, "finalisation produced no container");
    }
    if (out.size() > bounds_.max_output_bytes) {
        // Refused after measuring, not before: the size of an encoded clip is a property of the
        // codec, so the only honest check is on the bytes that exist.
        return fail(std::move(r), encoder_,
                    "finalised clip of " + std::to_string(out.size()) + " bytes exceeds the bound");
    }

    // Measured on the FINALISED mp4, both of them.
    r.size_bytes = out.size();
    r.content_md5 = content_md5(out);
    r.duration_seconds = static_cast<double>(duration.count()) / 1000.0;
    r.bytes = std::move(out);
    r.ok = true;                     // bytes are handed over only now
    return r;
}

}  // namespace hg::edge
