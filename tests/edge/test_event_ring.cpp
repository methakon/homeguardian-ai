// Host tests for EventRing. Hermetic: no hardware, no network, no gateway, no clock coupling.
//
// Mirrors the intended repository path tests/edge/test_event_ring.cpp.
#include "edge/EventRing.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace hg::edge;
using Clock = std::chrono::system_clock;
using ms = std::chrono::milliseconds;

static int fails = 0, checks = 0;
static void ck(bool c, const char* what) {
    ++checks;
    if (c) { std::printf("  ok    %s\n", what); }
    else   { ++fails; std::printf("  FAIL  %s\n", what); }
}

static Clock::time_point T(ms t) { return Clock::time_point{} + t; }

// A synthetic payload of `n` bytes. Contract use only; no camera data.
static std::vector<std::uint8_t> payload(std::size_t n, std::uint8_t fill = 0xAB) {
    return std::vector<std::uint8_t>(n, fill);
}

static RetainResult put(EventRing& r, const std::vector<std::uint8_t>& b,
                        ms t, bool key = true, std::uint64_t seq = 0) {
    return r.retain(ByteView{b.data(), b.size()}, 640, 360, T(t), key, seq);
}

int main() {
    std::printf("== authority is re-checked on every retain ==\n");
    {
        EventRing r;
        auto b = payload(100);
        ck(put(r, b, ms(0)) == RetainResult::refused_unauthorized,
           "an unauthorized ring refuses to retain");
        ck(r.stats().stored == 0, "and nothing was stored");
        ck(r.stats().refused == 1, "the refusal is counted, not silently dropped");
        r.set_authorized(true);
        ck(put(r, b, ms(0)) == RetainResult::stored, "authorized, it stores");
    }

    std::printf("== withdrawal clears retained data immediately ==\n");
    {
        EventRing r;
        auto b = payload(100);
        r.set_authorized(true);
        put(r, b, ms(0));
        put(r, b, ms(100));
        ck(r.stats().frames == 2, "two frames retained");
        r.set_authorized(false);
        ck(r.stats().frames == 0, "withdrawal clears them");
        ck(r.stats().bytes == 0, "and frees the byte count");
        auto w = r.window(T(ms(0)), T(ms(1000)));
        ck(!w.ok && w.reason == "authority withdrawn",
           "a window request after withdrawal is refused with a reason");
    }

    std::printf("== malformed payloads are refused, not stored ==\n");
    {
        EventRing r;
        r.set_authorized(true);
        ck(r.retain(ByteView{nullptr, 0}, 1, 1, T(ms(0)), true, 0) == RetainResult::refused_empty,
           "a zero-length payload is refused");
        ck(r.retain(ByteView{nullptr, 10}, 1, 1, T(ms(0)), true, 0) == RetainResult::refused_null,
           "a null pointer with a nonzero size is refused rather than read");
        ck(r.stats().frames == 0, "neither reached the ring");
    }

    std::printf("== one oversized frame must not evict the history ==\n");
    {
        RingBounds b; b.max_frame_bytes = 1024; b.max_total_bytes = 100000;
        EventRing r(b);
        r.set_authorized(true);
        auto small = payload(100);
        for (int i = 0; i < 5; ++i) put(r, small, ms(i * 100), true, static_cast<std::uint64_t>(i));
        ck(r.stats().frames == 5, "five frames retained");
        auto huge = payload(2048);
        ck(put(r, huge, ms(1000)) == RetainResult::refused_too_large, "the oversized frame is refused");
        ck(r.stats().frames == 5, "and the five are still there (the refusal protected them)");
    }

    std::printf("== bounded on bytes AND count: either alone is not a bound ==\n");
    {
        RingBounds b; b.max_frames = 4; b.max_total_bytes = 1000000; b.max_frame_bytes = 1000;
        EventRing r(b);
        r.set_authorized(true);
        auto p = payload(100);
        for (int i = 0; i < 10; ++i) put(r, p, ms(i * 100), true, static_cast<std::uint64_t>(i));
        ck(r.stats().frames == 4, "count bound holds after ten inserts");
        ck(r.stats().peak_frames == 4, "peak frame count never exceeded the bound");
    }
    {
        RingBounds b; b.max_frames = 1000; b.max_total_bytes = 500; b.max_frame_bytes = 1000;
        EventRing r(b);
        r.set_authorized(true);
        auto p = payload(100);
        for (int i = 0; i < 10; ++i) put(r, p, ms(i * 100), true, static_cast<std::uint64_t>(i));
        ck(r.stats().bytes <= 500, "byte bound holds after ten inserts");
        ck(r.stats().frames == 5, "and it evicted down to five 100-byte frames");
    }

    std::printf("== age is a third, independent bound ==\n");
    {
        RingBounds b; b.max_age = ms(1000); b.max_frames = 1000; b.max_total_bytes = 1000000;
        EventRing r(b);
        r.set_authorized(true);
        auto p = payload(50);
        for (int i = 0; i < 8; ++i) put(r, p, ms(i * 100), true, static_cast<std::uint64_t>(i));
        ck(r.stats().frames == 8, "eight retained within the age bound");
        r.expire(T(ms(5000)));          // cutoff 4000ms: everything older than 4s goes
        ck(r.stats().frames == 0, "expire() drops everything past the age bound");
        ck(r.stats().evicted_by_age == 8, "and attributes the eviction to age");
    }

    std::printf("== the window reports its own limits honestly ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing r(b);
        r.set_authorized(true);
        auto p = payload(64);
        // 0..900ms, keyframe only at 0 and 500 (GOP-ish), rest P frames
        for (int i = 0; i < 10; ++i) {
            const bool key = (i == 0 || i == 5);
            put(r, p, ms(i * 100), key, static_cast<std::uint64_t>(i));
        }
        auto w = r.window(T(ms(0)), T(ms(900)));
        ck(w.ok, "a range inside the retained window is served");
        ck(w.frames.size() == 10, "and returns every retained frame in the range");
        ck(w.starts_at_keyframe, "starting at a keyframe is reported true");
        ck(w.complete, "and the range is reported complete");

        // Starting mid-GOP must NOT claim a clean start - that is the honest answer.
        auto w2 = r.window(T(ms(250)), T(ms(900)));
        ck(w2.ok, "a mid-GOP range is still served");
        ck(!w2.starts_at_keyframe, "but it does NOT claim to start at a random-access point");
        ck(w2.frames.front().sequence == 3, "and it begins at the first frame actually retained in range");

        // A range whose earlier history was evicted must not claim completeness.
        RingBounds nb; nb.max_age = ms(60000); nb.max_frames = 4;
        EventRing r2(nb);
        r2.set_authorized(true);
        for (int i = 0; i < 10; ++i) put(r2, p, ms(i * 100), true, static_cast<std::uint64_t>(i));
        auto w3 = r2.window(T(ms(0)), T(ms(900)));
        ck(w3.ok, "an over-wide range still returns what is retained");
        ck(!w3.complete, "but reports it is NOT complete, because earlier history was evicted");
        ck(std::strstr(w3.reason.c_str(), "not fully retained") != nullptr,
           "with a reason the caller can act on");
    }

    std::printf("== a gap is never interpolated ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing r(b);
        r.set_authorized(true);
        auto p = payload(64);
        put(r, p, ms(0), true, 0);
        put(r, p, ms(100), true, 1);
        // A genuine capture gap: nothing was captured between 100 ms and 3000 ms, and the
        // acquisition sequence jumps 1 -> 7 across it. This is what a real gap looks like.
        put(r, p, ms(3000), true, 7);
        put(r, p, ms(3100), true, 8);
        auto w = r.window(T(ms(0)), T(ms(3100)));
        ck(w.frames.size() == 4, "only genuinely captured frames are returned");

        // The gap is visible in the acquisition sequence, and nothing was invented to fill it.
        bool seq_jump = (w.frames[1].sequence == 1 && w.frames[2].sequence == 7);
        ck(seq_jump, "the gap is visible as a sequence jump (1 -> 7)");

        // Nothing was synthesised inside the gap. This check is UNCONDITIONAL on purpose:
        // an assertion nested behind a condition that never fires reads as coverage while
        // testing nothing, which is how the previous version of this test was misleading.
        int inside_gap = 0;
        for (const auto& f : w.frames) {
            if (f.captured_at > T(ms(100)) && f.captured_at < T(ms(3000))) ++inside_gap;
        }
        ck(inside_gap == 0, "not one frame exists inside the gap - none was interpolated");

        // The gap's true duration survives: it is not smoothed or averaged away.
        const auto gap = w.frames[2].captured_at - w.frames[1].captured_at;
        ck(gap == ms(2900), "the gap's real duration (2900 ms) is preserved, not smoothed");
    }

    std::printf("== ordering and empty cases ==\n");
    {
        EventRing r; r.set_authorized(true);
        auto w = r.window(T(ms(0)), T(ms(100)));
        ck(!w.ok && w.reason == "ring holds nothing", "an empty ring says so");
        auto p = payload(10);
        put(r, p, ms(500), true, 0);
        auto inv = r.window(T(ms(900)), T(ms(100)));
        ck(!inv.ok && inv.reason == "requested range is inverted", "an inverted range is refused");
        auto outside = r.window(T(ms(5000)), T(ms(6000)));
        ck(!outside.ok && outside.reason == "nothing retained in that range", "an empty range says so");
        Clock::time_point o, n;
        ck(r.span(o, n), "span() reports the retained extent");
        ck(o == T(ms(500)) && n == T(ms(500)), "and it is the single retained frame");
        r.clear();
        ck(!r.span(o, n), "after clear() there is no span");
    }

    std::printf("== clear() is deterministic and idempotent ==\n");
    {
        EventRing r; r.set_authorized(true);
        auto p = payload(128);
        for (int i = 0; i < 5; ++i) put(r, p, ms(i * 10), true, static_cast<std::uint64_t>(i));
        r.clear(); r.clear(); r.clear();
        ck(r.stats().frames == 0 && r.stats().bytes == 0, "three clears leave it empty, not negative");
        ck(put(r, p, ms(0)) == RetainResult::stored, "and it still accepts new frames afterwards");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
