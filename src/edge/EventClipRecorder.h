// EventClipRecorder — bounded event recording from the retained pre-event window.
//
// This is the "bounded event recording" half of the deliverable, and it is where RULE 4 lives:
// a clip must begin at a random-access point, which means seeking BACK to the preceding keyframe
// and discarding whatever is decoded before the clip actually starts. Decoding from an arbitrary
// frame produces corrupt output, and decoding from the clip start with no look-back produces a
// clip that begins after the movement did.
//
// WHAT IS PURE HERE AND WHAT IS INJECTED
// --------------------------------------
// Policy, bounds, cancellation, the seek, the discard and the measurement are pure and host-tested.
// The decoder and the encoder are injected interfaces, so the Android codec glue stays separate
// and this logic is exercised without a phone. That separation is deliberate: a host test cannot
// establish that a phone codec works, and a phone test cannot cheaply establish that the seek and
// discard logic is correct. Each is tested where it can be.
//
// THE TWO RULES THAT MATTER MOST
// ------------------------------
// 1. Size and MD5 are computed on the FINALISED artifact, never on a partial or predicted stream.
//    A predicted size is a guess about a codec; the only honest measurement is of the bytes that
//    exist.
// 2. Finalisation is ATOMIC. A result carries bytes only when it is complete and measured, so a
//    cancelled or failed recording cannot be published as a finished clip.
#pragma once

#include "edge/EventRing.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hg::edge {

// One frame as the encoder wants it. Opaque to this file: whatever the platform decoder produces.
struct DecodedFrame {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    std::chrono::system_clock::time_point captured_at{};
    std::uint64_t sequence = 0;
};

// Injected. Returns false when the frame cannot be decoded; the recorder counts that and continues
// rather than abandoning the clip for one bad frame, unless nothing at all can be decoded.
class FrameDecoder {
public:
    virtual ~FrameDecoder() = default;
    virtual bool decode(const RetainedFrame& frame, DecodedFrame& out) = 0;
};

// Injected. `finalize` must produce the COMPLETE container and return false when it cannot.
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual bool begin(int width, int height) = 0;
    virtual bool add(const DecodedFrame& frame) = 0;
    virtual bool finalize(std::vector<std::uint8_t>& out) = 0;
    virtual void abandon() = 0;
};

struct ClipRequest {
    std::uint64_t event_id = 0;
    // The event's anchor: the FIRST WEAK HIT, not the confirmation. Reach back from here.
    std::chrono::system_clock::time_point anchor{};
    std::chrono::system_clock::time_point event_end{};
    std::chrono::milliseconds pre_roll{2000};
    // How far back to look for a random-access point before the clip start. Must exceed the GOP,
    // or the seek can fail for a clip whose start falls just after a keyframe.
    std::chrono::milliseconds keyframe_search{2000};
};

struct ClipBounds {
    std::size_t max_source_frames = 900;    // bounds the DECODE work, not just the output
    std::size_t max_output_bytes = 16u * 1024u * 1024u;
    std::chrono::milliseconds max_duration{30000};
    std::size_t min_encoded_frames = 1;
};

struct ClipResult {
    bool ok = false;                        // true only when the artifact exists and is measured
    std::string reason;                     // why not, when !ok

    std::uint64_t event_id = 0;
    std::vector<std::uint8_t> bytes;        // EMPTY unless ok - nothing partial is ever handed out
    std::size_t size_bytes = 0;
    std::string content_md5;                // base64 of the 16-byte MD5, of the FINALISED bytes
    double duration_seconds = 0.0;
    int width = 0;
    int height = 0;

    std::size_t source_frames_considered = 0;
    std::size_t frames_decoded = 0;
    std::size_t frames_discarded_before_start = 0;
    std::size_t frames_encoded = 0;
    std::size_t decode_failures = 0;

    bool starts_at_keyframe = false;
    bool range_complete = false;            // false => earlier history was already evicted
    std::chrono::system_clock::time_point clip_start{};
    std::chrono::system_clock::time_point clip_end{};
};

class EventClipRecorder {
public:
    EventClipRecorder(EventRing& ring, FrameDecoder& decoder, VideoEncoder& encoder,
                      ClipBounds bounds = {})
        : ring_(ring), decoder_(decoder), encoder_(encoder), bounds_(bounds) {}

    // `cancelled` is polled between frames. A cancelled or failed recording abandons the encoder
    // and returns no bytes, so nothing partial can be published.
    ClipResult record(const ClipRequest& request, const std::function<bool()>& cancelled = {});

    const ClipBounds& bounds() const { return bounds_; }

private:
    EventRing& ring_;
    FrameDecoder& decoder_;
    VideoEncoder& encoder_;
    ClipBounds bounds_;
};

}  // namespace hg::edge
