// test_media_backpressure: a camera produces frames whether or not anything consumes them.
//
// The properties under test are the ones that decide whether a hub survives a slow consumer or an
// out-of-memory failure, and whether a disconnected camera turns into invented evidence.

#include <catch2/catch.hpp>

#include "core/BoundedMediaBuffer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

using namespace homeguardian;

namespace {

template <typename T>
constexpr bool has_to_json = requires(T value) { value.to_json(); };

MediaFrame frame(uint64_t id, size_t bytes) {
    MediaFrame f;
    f.frame_id = id;
    f.captured_at = std::chrono::system_clock::now();
    f.bytes.assign(bytes, static_cast<uint8_t>(id & 0xFF));
    return f;
}

}  // namespace

TEST_CASE("media: raw frames cannot be serialized into the observation path", "[media][boundary]") {
    // The boundary is a property of the TYPE, not a convention: no to_json, no json include, no
    // serialisation surface at all. The check must be templated or a non-dependent requires-expression is
    // a hard error rather than a false - which is exactly how the first version of this test failed to
    // compile, and worth keeping as a comment because it is a real C++ trap.
    REQUIRE_FALSE(has_to_json<MediaFrame>);
}

TEST_CASE("media: the queue depth is a hard ceiling under a slow consumer", "[media][backpressure]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 4;
    cfg.max_bytes = 64u * 1024u;
    BoundedMediaBuffer buffer(cfg);

    // Producer runs freely; the consumer takes one frame in ten.
    uint64_t delivered = 0;
    for (uint64_t id = 0; id < 100; ++id) {
        std::string why;
        (void)buffer.enqueue(frame(id, 1024), &why);
        if (id % 10 == 0) {
            MediaFrame out;
            if (buffer.dequeue(&out)) ++delivered;
        }
    }

    const auto& s = buffer.stats();
    INFO("enqueued=" << s.enqueued << " dequeued=" << s.dequeued << " dropped=" << s.dropped_oldest
                     << " frames_high_water=" << s.frames_high_water << " bytes_held=" << s.bytes_held);
    REQUIRE(s.frames_high_water <= 4);                       // the depth ceiling held
    REQUIRE(buffer.size() <= 4);
    REQUIRE(s.bytes_held <= cfg.max_bytes);                  // and so did the byte ceiling
    REQUIRE(s.enqueued == 100);                              // every frame was accepted OR dropped, not lost
    REQUIRE(s.dropped_oldest > 0);                           // a slow consumer means drops, accounted
    REQUIRE(s.dequeued == delivered);
    // What survives is the NEWEST frame: for live media the recent past is what an inference step wants.
    const MediaFrame* newest = buffer.peek_oldest();
    REQUIRE(newest != nullptr);
    REQUIRE(newest->frame_id > 50);
}

TEST_CASE("media: the byte ceiling bounds memory independently of the frame count",
          "[media][backpressure][bounds]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 1000;              // deliberately not the binding constraint
    cfg.max_bytes = 16u * 1024u;
    cfg.max_frame_bytes = 8u * 1024u;
    BoundedMediaBuffer buffer(cfg);

    for (uint64_t id = 0; id < 50; ++id) (void)buffer.enqueue(frame(id, 4096));
    const auto& s = buffer.stats();
    INFO("bytes_held=" << s.bytes_held << " high_water=" << s.bytes_high_water
                       << " frames=" << buffer.size());
    REQUIRE(s.bytes_held <= cfg.max_bytes);
    REQUIRE(s.bytes_high_water <= cfg.max_bytes);            // the ceiling was never exceeded, not merely restored
    REQUIRE(buffer.size() <= 4);
}

TEST_CASE("media: an oversized frame is refused rather than truncated", "[media][bounds][refusal]") {
    MediaBufferConfig cfg;
    cfg.max_frame_bytes = 2048;
    BoundedMediaBuffer buffer(cfg);
    std::string why;
    const auto outcome = buffer.enqueue(frame(1, 4096), &why);
    REQUIRE(outcome == MediaEnqueueOutcome::rejected_oversized);
    REQUIRE(why.find("exceeds the per-frame ceiling") != std::string::npos);
    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.stats().rejected_oversized == 1);
}

TEST_CASE("media: a disconnect stops intake, drains what was real, and invents nothing",
          "[media][offline]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 4;
    BoundedMediaBuffer buffer(cfg);
    REQUIRE(buffer.enqueue(frame(1, 512)) == MediaEnqueueOutcome::enqueued);
    REQUIRE(buffer.enqueue(frame(2, 512)) == MediaEnqueueOutcome::enqueued);

    buffer.mark_producer_disconnected();
    std::string why;
    const auto refused = buffer.enqueue(frame(3, 512), &why);
    REQUIRE(refused == MediaEnqueueOutcome::rejected_disconnected);
    REQUIRE(why.find("no frame is invented") != std::string::npos);
    REQUIRE(buffer.stats().rejected_disconnected == 1);
    REQUIRE(buffer.would_reject(100));                       // the backpressure signal says so up front

    // The frames already held were really captured: they drain.
    MediaFrame out;
    REQUIRE(buffer.dequeue(&out));
    REQUIRE(out.frame_id == 1);
    REQUIRE(buffer.dequeue(&out));
    REQUIRE(out.frame_id == 2);
    // And then it is empty - not a blank frame, not a placeholder.
    REQUIRE_FALSE(buffer.dequeue(&out));
    REQUIRE(buffer.size() == 0);
}

TEST_CASE("media: rejection instead of dropping is available, and is accounted",
          "[media][backpressure]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 2;
    cfg.drop_oldest_when_full = false;      // the caller would rather lose the new frame
    BoundedMediaBuffer buffer(cfg);
    REQUIRE(buffer.enqueue(frame(1, 256)) == MediaEnqueueOutcome::enqueued);
    REQUIRE(buffer.enqueue(frame(2, 256)) == MediaEnqueueOutcome::enqueued);
    std::string why;
    const auto third = buffer.enqueue(frame(3, 256), &why);
    REQUIRE(third == MediaEnqueueOutcome::rejected_full);
    REQUIRE(why.find("keeps the oldest frames") != std::string::npos);
    REQUIRE(buffer.stats().rejected_full == 1);
    REQUIRE(buffer.stats().dropped_oldest == 0);
    // The oldest frame is still there: that is what "reject" means.
    const MediaFrame* oldest = buffer.peek_oldest();
    REQUIRE(oldest != nullptr);
    REQUIRE(oldest->frame_id == 1);
}

TEST_CASE("media: local processing cost and latency, measured not asserted", "[media][resource]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 16;
    cfg.max_bytes = 64u * 1024u * 1024u;
    BoundedMediaBuffer buffer(cfg);

    constexpr size_t kFrames = 20000;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kFrames; ++i) {
        (void)buffer.enqueue(frame(i, 64 * 1024));          // a 64 KiB frame: a plausible luma plane
        MediaFrame out;
        (void)buffer.dequeue(&out);                          // a consumer that keeps up
    }
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
            .count();
    const double per_frame_us = static_cast<double>(elapsed_us) / static_cast<double>(kFrames);
    std::printf("media: %zu frames of 64 KiB enqueued+dequeued in %lld us (%.2f us/frame); "
                "bytes held high-water %llu\n",
                kFrames, static_cast<long long>(elapsed_us), per_frame_us,
                static_cast<unsigned long long>(buffer.stats().bytes_high_water));
    REQUIRE(buffer.stats().bytes_high_water <= cfg.max_bytes);
    REQUIRE(buffer.stats().dropped_oldest == 0);             // a consumer that keeps up never forces a drop
    REQUIRE(per_frame_us < 1000.0);                          // sanity: not a pathological cost
}

TEST_CASE("media: consent revocation discards pending media rather than holding it",
          "[media][consent][refusal]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 8;
    BoundedMediaBuffer buffer(cfg);
    for (uint64_t id = 1; id <= 5; ++id) (void)buffer.enqueue(frame(id, 1024));
    REQUIRE(buffer.size() == 5);
    REQUIRE(buffer.stats().bytes_held == 5 * 1024);

    std::string why;
    const size_t discarded = buffer.purge(&why);
    REQUIRE(discarded == 5);
    REQUIRE(why.find("consent withdrawn") != std::string::npos);
    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.stats().bytes_held == 0);          // the memory is released, not merely unreferenced
    REQUIRE(buffer.stats().purged == 5);
    MediaFrame out;
    REQUIRE_FALSE(buffer.dequeue(&out));               // and nothing is left to hand out
}

TEST_CASE("media: graceful shutdown releases everything and stops intake", "[media][shutdown]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 16;
    BoundedMediaBuffer buffer(cfg);
    for (uint64_t id = 1; id <= 12; ++id) (void)buffer.enqueue(frame(id, 2048));

    const size_t discarded = buffer.shutdown();
    REQUIRE(discarded == 12);
    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.stats().bytes_held == 0);
    REQUIRE(buffer.stats().purged_at_shutdown == 12);
    REQUIRE_FALSE(buffer.producer_connected());
    // After shutdown the buffer refuses new frames and never invents one.
    REQUIRE(buffer.enqueue(frame(99, 128)) == MediaEnqueueOutcome::rejected_disconnected);
    MediaFrame out;
    REQUIRE_FALSE(buffer.dequeue(&out));
}

TEST_CASE("media: a long burst never allocates beyond the ceilings", "[media][bounds][resource]") {
    MediaBufferConfig cfg;
    cfg.max_frames = 8;
    cfg.max_bytes = 256u * 1024u;
    cfg.max_frame_bytes = 64u * 1024u;
    BoundedMediaBuffer buffer(cfg);

    uint64_t accepted_or_dropped = 0;
    for (uint64_t id = 0; id < 10000; ++id) {
        const auto outcome = buffer.enqueue(frame(id, 32 * 1024));
        if (outcome == MediaEnqueueOutcome::enqueued || outcome == MediaEnqueueOutcome::dropped_oldest) {
            ++accepted_or_dropped;
        }
    }
    const auto& s = buffer.stats();
    std::printf("media burst: 10000 frames -> enqueued=%llu dropped=%llu bytes_high_water=%llu "
                "frames_high_water=%llu\n",
                static_cast<unsigned long long>(s.enqueued),
                static_cast<unsigned long long>(s.dropped_oldest),
                static_cast<unsigned long long>(s.bytes_high_water),
                static_cast<unsigned long long>(s.frames_high_water));
    // The ceilings held for the whole burst, not just at the end.
    REQUIRE(s.frames_high_water <= cfg.max_frames);
    REQUIRE(s.bytes_high_water <= cfg.max_bytes);
    REQUIRE(s.bytes_held <= cfg.max_bytes);
    REQUIRE(accepted_or_dropped == 10000);            // every frame was stored or explicitly dropped
    REQUIRE(s.enqueued == 10000);
    // Memory is bounded by the ceiling, so the resident set cannot grow with the burst: the buffer holds at
    // most max_frames payloads of at most max_frame_bytes each.
    REQUIRE(s.bytes_held <= cfg.max_frames * cfg.max_frame_bytes);
}
