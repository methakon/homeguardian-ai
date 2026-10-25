// Host tests for EventClipRecorder — bounded event recording from the retained window.
//
// The decoder and encoder are fakes, so this exercises the POLICY: the keyframe seek, the discard
// of output before the clip start, the bounds, cancellation, and the two rules that matter most -
// measurement of the FINALISED artifact, and atomicity (no partial clip is ever handed out).
#include "edge/EventClipRecorder.h"
#include "edge/Md5.h"

#include <chrono>
#include <cstdio>
#include <string>
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

// ---------------------------------------------------------------------------------------------
// Fakes. Configurable failure points, and they COUNT abandon() so "failed closed" is checkable
// rather than assumed.
// ---------------------------------------------------------------------------------------------
struct FakeDecoder : FrameDecoder {
    bool fail_always = false;
    int fail_every = 0;                  // fail every Nth frame when > 1
    int seen = 0;
    std::vector<std::chrono::system_clock::time_point> decoded_at;

    bool decode(const RetainedFrame& f, DecodedFrame& out) override {
        ++seen;
        if (fail_always) return false;
        if (fail_every > 1 && (seen % fail_every) == 0) return false;
        out.pixels.assign(64, 0x77);
        out.width = f.width;
        out.height = f.height;
        out.captured_at = f.captured_at;
        out.sequence = f.sequence;
        decoded_at.push_back(f.captured_at);
        return true;
    }
};

struct FakeEncoder : VideoEncoder {
    bool fail_begin = false;
    bool fail_add = false;
    bool fail_finalize = false;
    std::size_t emit_bytes = 4096;
    std::uint8_t fill = 0x5A;

    int began = 0, abandoned = 0;
    std::vector<std::chrono::system_clock::time_point> added_at;

    bool begin(int, int) override { ++began; return !fail_begin; }
    bool add(const DecodedFrame& f) override {
        if (fail_add) return false;
        added_at.push_back(f.captured_at);
        return true;
    }
    bool finalize(std::vector<std::uint8_t>& out) override {
        if (fail_finalize) return false;
        out.assign(emit_bytes, fill);
        return true;
    }
    void abandon() override { ++abandoned; }
};

// Populate a ring with a realistic sequence: a keyframe every `gop` frames.
static void fill(EventRing& ring, int count, ms step, int gop = 5, std::size_t bytes = 256) {
    for (int i = 0; i < count; ++i) {
        std::vector<std::uint8_t> payload(bytes, static_cast<std::uint8_t>(i & 0xff));
        ring.retain(ByteView{payload.data(), payload.size()}, 640, 360,
                    T(ms(i * step.count())), /*keyframe=*/((i % gop) == 0),
                    static_cast<std::uint64_t>(i));
    }
}

static constexpr ms kStep{100};

int main() {
    std::printf("== a clip is produced, and measured on the FINALISED artifact ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep);
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);

        ClipRequest req;
        req.event_id = 7;
        req.anchor = T(ms(3000));
        req.event_end = T(ms(3500));
        req.pre_roll = ms(500);

        auto r = rec.record(req);
        ck(r.ok, "the clip is produced");
        ck(!r.bytes.empty(), "and carries bytes");
        ck(r.size_bytes == r.bytes.size(), "size equals the bytes actually produced");
        ck(r.size_bytes == enc.emit_bytes, "which is what the encoder emitted");
        ck(r.content_md5 == content_md5(r.bytes),
           "the MD5 is of those bytes, not of a predicted stream");
        ck(r.content_md5 == content_md5(std::vector<std::uint8_t>(enc.emit_bytes, enc.fill)),
           "and it matches an independent digest of the same content");
        ck(r.starts_at_keyframe, "the clip starts at a random-access point");
        // anchor 3000 ms - 500 ms pre-roll = clip start 2500 ms; event end 3500 ms -> 1.0 s.
        ck(r.duration_seconds == 1.0, "duration is the span from the pre-rolled start to the event end");
    }

    std::printf("== RULE 4: output before the clip start is discarded, not encoded ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep, 5);                 // keyframes at 0, 500, 1000, 1500 ms...
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);

        ClipRequest req;
        req.anchor = T(ms(3200));
        req.event_end = T(ms(3600));
        req.pre_roll = ms(400);                   // clip starts at 2800 ms
        req.keyframe_search = ms(1000);           // reaches back to 1800 ms

        auto r = rec.record(req);
        ck(r.ok, "the clip is produced");
        ck(r.frames_decoded > r.frames_encoded,
           "MORE frames were decoded than encoded - the look-back really happened");
        ck(r.frames_discarded_before_start > 0, "and the pre-start frames are counted as discarded");
        ck(!enc.added_at.empty() && enc.added_at.front() >= T(ms(2800)),
           "the FIRST frame the encoder received is at or after the clip start");
        for (auto t : enc.added_at) {
            if (t < T(ms(2800))) { ck(false, "no encoded frame predates the clip start"); break; }
        }
        ck(true, "and no encoded frame predates the clip start");
        ck(dec.seen == static_cast<int>(r.frames_decoded) + static_cast<int>(r.decode_failures),
           "every decoded attempt is accounted for");
    }

    std::printf("== the seek finds a keyframe BEFORE a start that falls just after one ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep, 5);
        // add a second keyframe pattern so the nearest one before 2800 ms is at 2500
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);
        ClipRequest req;
        req.anchor = T(ms(2900));
        req.event_end = T(ms(3000));
        req.pre_roll = ms(100);                   // clip starts at 2800, keyframes at 0/500/...
        req.keyframe_search = ms(3000);
        auto r = rec.record(req);
        ck(r.ok, "a start just after a keyframe still records");
        ck(r.starts_at_keyframe, "because the seek found the preceding one");
        // 2800 is not a keyframe; the preceding is 2500. The first decoded frame proves it.
        ck(!dec.decoded_at.empty() && dec.decoded_at.front() <= T(ms(2800)),
           "decoding began at or before the clip start, i.e. from the keyframe");
        ck(!dec.decoded_at.empty() && dec.decoded_at.front() == T(ms(2500)),
           "and specifically from the preceding keyframe at 2500 ms");
    }

    std::printf("== NO keyframe before the start fails closed ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        // keyframes only at 0; ask for a window whose search cannot reach it
        fill(ring, 40, kStep, /*gop=*/1000);
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);
        ClipRequest req;
        req.anchor = T(ms(3900));
        req.event_end = T(ms(3950));
        req.pre_roll = ms(50);
        req.keyframe_search = ms(100);            // far too short to reach 0
        auto r = rec.record(req);
        ck(!r.ok, "it refuses");
        ck(r.reason.find("random-access") != std::string::npos, "with a reason naming the cause");
        ck(r.bytes.empty(), "and NO bytes");
        ck(enc.began == 0, "the encoder was never even started");
    }

    std::printf("== cancellation abandons and publishes nothing ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep);
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);
        ClipRequest req;
        req.anchor = T(ms(3000)); req.event_end = T(ms(3500)); req.pre_roll = ms(500);

        int calls = 0;
        auto r = rec.record(req, [&calls] { return ++calls > 3; });   // cancel part-way
        ck(!r.ok, "a cancelled recording fails");
        ck(r.reason == "cancelled", "with a cancellation reason");
        ck(r.bytes.empty(), "and NO bytes - nothing partial is published");
        ck(enc.abandoned == 1, "and the encoder was abandoned exactly once");
    }

    std::printf("== every encoder failure mode fails closed and abandons ==\n");
    {
        struct Case { const char* name; bool begin, add, fin; };
        const Case cases[] = {
            {"begin refuses",    true,  false, false},
            {"add refuses",      false, true,  false},
            {"finalize refuses", false, false, true},
        };
        for (const auto& c : cases) {
            RingBounds rb; rb.max_age = ms(600000);
            EventRing ring(rb); ring.set_authorized(true);
            fill(ring, 60, kStep);
            FakeDecoder dec; FakeEncoder enc;
            enc.fail_begin = c.begin; enc.fail_add = c.add; enc.fail_finalize = c.fin;
            EventClipRecorder rec(ring, dec, enc);
            ClipRequest req;
            req.anchor = T(ms(3000)); req.event_end = T(ms(3500)); req.pre_roll = ms(500);
            auto r = rec.record(req);
            ck(!r.ok, (std::string(c.name) + " -> not ok").c_str());
            ck(r.bytes.empty(), (std::string(c.name) + " -> no bytes").c_str());
            ck(r.size_bytes == 0 && r.content_md5.empty(),
               (std::string(c.name) + " -> no size and no digest of nothing").c_str());
            ck(enc.abandoned == 1, (std::string(c.name) + " -> encoder abandoned").c_str());
        }
    }

    std::printf("== an over-bound clip is refused AFTER measuring, not guessed at ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep);
        FakeDecoder dec; FakeEncoder enc;
        enc.emit_bytes = 100000;
        ClipBounds cb; cb.max_output_bytes = 1024;      // the produced clip will exceed this
        EventClipRecorder rec(ring, dec, enc, cb);
        ClipRequest req;
        req.anchor = T(ms(3000)); req.event_end = T(ms(3500)); req.pre_roll = ms(500);
        auto r = rec.record(req);
        ck(!r.ok, "an over-bound clip is refused");
        ck(r.reason.find("exceeds the bound") != std::string::npos, "with a size reason");
        ck(enc.began == 1, "and the encoder DID run - the refusal is after the real measurement");
        ck(r.bytes.empty(), "but no bytes are handed out");
    }

    std::printf("== authority is checked before anything is decoded ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        fill(ring, 60, kStep);
        ring.set_authorized(false);                     // withdrawn after retention
        FakeDecoder dec; FakeEncoder enc;
        EventClipRecorder rec(ring, dec, enc);
        ClipRequest req;
        req.anchor = T(ms(3000)); req.event_end = T(ms(3500)); req.pre_roll = ms(500);
        auto r = rec.record(req);
        ck(!r.ok, "recording after withdrawal fails");
        ck(r.reason == "authority withdrawn", "with the authority reason");
        ck(dec.seen == 0, "and nothing was decoded at all");
        ck(r.bytes.empty(), "no bytes");
    }

    std::printf("== a partly-decodable burst still records, counting the failures ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb); ring.set_authorized(true);
        fill(ring, 60, kStep);
        FakeDecoder dec; FakeEncoder enc;
        dec.fail_every = 5;                              // every 5th frame will not decode
        EventClipRecorder rec(ring, dec, enc);
        ClipRequest req;
        req.anchor = T(ms(3000)); req.event_end = T(ms(3500)); req.pre_roll = ms(500);
        auto r = rec.record(req);
        ck(r.ok, "one undecodable frame does not lose the clip");
        ck(r.decode_failures > 0, "the failures are counted");
        ck(r.frames_encoded > 0, "and the good frames are still encoded");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
