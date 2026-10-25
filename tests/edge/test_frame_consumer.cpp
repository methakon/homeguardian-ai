// Host tests for FrameConsumer + RingFrameConsumer — the acquisition-to-ring contract.
//
// The point of this suite is that the interface is EXERCISED, not merely declared. A fake
// acquisition drives the consumer exactly as the real acquisition path would, and the results are
// read back out of the ring, so the integration workstream receives a contract that has been run
// rather than a description of one.
#include "edge/FrameConsumer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
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
// A fake acquisition. It owns the buffers, hands out borrowed views, and can be made to violate
// the contract on purpose. Modelling it as a real producer is what makes the test meaningful:
// the real acquisition will do exactly this and then reuse its buffer.
// ---------------------------------------------------------------------------------------------
class FakeAcquisition {
public:
    explicit FakeAcquisition(FrameConsumer& sink) : sink_(sink) {}

    // A well-formed, validated frame.
    void emit(std::size_t bytes, ms t, std::uint64_t seq, std::uint8_t fill = 0xAB) {
        buf_.assign(bytes, fill);
        BorrowedFrame f;
        f.data = buf_.data();
        f.size = buf_.size();
        f.width = 640;
        f.height = 360;
        f.captured_at = T(t);
        f.sequence = seq;
        f.validated = true;
        sink_.on_frame(f);
    }

    // Deliberately invalid variants, so the refusals are exercised rather than assumed.
    void emit_unvalidated(std::size_t bytes, ms t, std::uint64_t seq) {
        buf_.assign(bytes, 0xCD);
        BorrowedFrame f;
        f.data = buf_.data(); f.size = buf_.size();
        f.width = 640; f.height = 360; f.captured_at = T(t); f.sequence = seq;
        f.validated = false;                 // the caller never ran the guard
        sink_.on_frame(f);
    }
    void emit_bad_geometry(ms t, std::uint64_t seq) {
        buf_.assign(64, 0xEF);
        BorrowedFrame f;
        f.data = buf_.data(); f.size = buf_.size();
        f.width = 0; f.height = 0;           // dimensions not derived from checked headers
        f.captured_at = T(t); f.sequence = seq; f.validated = true;
        sink_.on_frame(f);
    }
    void emit_null(ms t, std::uint64_t seq) {
        BorrowedFrame f;
        f.data = nullptr; f.size = 0; f.width = 640; f.height = 360;
        f.captured_at = T(t); f.sequence = seq; f.validated = true;
        sink_.on_frame(f);
    }

    // Reuse the buffer the way a real producer does, so copy semantics are observable.
    void poison_buffer() { std::memset(buf_.data(), 0x00, buf_.size()); }

private:
    FrameConsumer& sink_;
    std::vector<std::uint8_t> buf_;
};

int main() {
    std::printf("== a validated frame reaches the ring, end to end ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        ring.set_authorized(true);
        RingFrameConsumer consumer(ring);
        FakeAcquisition acq(consumer);

        for (int i = 0; i < 10; ++i) acq.emit(256, ms(i * 100), static_cast<std::uint64_t>(i));
        ck(consumer.counts().accepted == 10, "ten validated frames accepted");
        ck(ring.stats().frames == 10, "and ten are retained in the ring");

        // Read the window back out - the thing the recorder will actually do.
        auto w = ring.window(T(ms(0)), T(ms(900)));
        ck(w.ok, "a window over the retained range is served");
        ck(w.frames.size() == 10, "with all ten frames");
        ck(w.frames.front().bytes.size() == 256, "each carrying its bytes");
        ck(w.starts_at_keyframe, "and starting at a random-access point");
    }

    std::printf("== the ring owns its copy: the borrowed buffer may be reused ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        ring.set_authorized(true);
        RingFrameConsumer consumer(ring);
        FakeAcquisition acq(consumer);

        acq.emit(300, ms(0), 0, 0xAB);
        acq.poison_buffer();                 // the producer reuses/frees its buffer
        acq.emit(300, ms(100), 1, 0xAB);

        auto w = ring.window(T(ms(0)), T(ms(100)));
        ck(w.frames.size() == 2, "both frames are retained");
        bool first_intact = true;
        for (auto byte : w.frames.front().bytes) if (byte != 0xAB) { first_intact = false; break; }
        ck(first_intact,
           "the FIRST frame is still its original bytes after the producer overwrote its buffer - "
           "the ring copied rather than aliased");
        ck(w.frames.front().bytes.size() == 300, "and its length is unchanged");
    }

    std::printf("== an UNVALIDATED frame is refused: the caller's obligation is enforced ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        ring.set_authorized(true);
        RingFrameConsumer consumer(ring);
        FakeAcquisition acq(consumer);

        acq.emit_unvalidated(256, ms(0), 0);
        ck(ring.stats().frames == 0,
           "bytes nobody validated are NOT retained - a hook before the decoder cannot make the "
           "service store unchecked input");
        ck(consumer.counts().refused_unvalidated == 1, "and the refusal is counted as unvalidated");
        ck(consumer.counts().accepted == 0, "nothing was accepted");
    }

    std::printf("== oversize, bad geometry and null are each refused and distinguished ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        ring.set_authorized(true);
        FrameLimits lim; lim.max_bytes = 1024;
        RingFrameConsumer consumer(ring, lim);
        FakeAcquisition acq(consumer);

        acq.emit(2048, ms(0), 0);            // over the declared ceiling
        ck(consumer.counts().refused_oversize == 1, "an over-ceiling frame is refused as oversize");
        ck(ring.stats().frames == 0, "and not retained");

        acq.emit_bad_geometry(ms(100), 1);
        ck(consumer.counts().refused_bad_geometry == 1,
           "a frame with no dimensions derived from checked headers is refused");

        acq.emit_null(ms(200), 2);
        ck(consumer.counts().refused_null == 1, "a null payload is refused, not read");

        acq.emit(512, ms(300), 3);
        ck(consumer.counts().accepted == 1, "and a valid frame within the ceiling still gets through");
    }

    std::printf("== authority change clears retained data through the interface ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        RingFrameConsumer consumer(ring);
        FakeAcquisition acq(consumer);

        consumer.on_authority_changed(true);
        acq.emit(256, ms(0), 0);
        acq.emit(256, ms(100), 1);
        ck(ring.stats().frames == 2, "two frames retained while authorized");
        ck(consumer.counts().accepted == 2, "and accepted");

        consumer.on_authority_changed(false);
        ck(ring.stats().frames == 0, "withdrawal clears them through the interface");
        acq.emit(256, ms(200), 2);
        ck(consumer.counts().accepted == 2, "and nothing further is retained while unauthorized");
    }

    std::printf("== a gap in the acquisition sequence stays a gap ==\n");
    {
        RingBounds b; b.max_age = ms(60000);
        EventRing ring(b);
        ring.set_authorized(true);
        RingFrameConsumer consumer(ring);
        FakeAcquisition acq(consumer);

        acq.emit(128, ms(0), 0);
        acq.emit(128, ms(100), 1);
        acq.emit(128, ms(3000), 42);         // the acquisition dropped frames, visibly
        acq.emit(128, ms(3100), 43);

        auto w = ring.window(T(ms(0)), T(ms(3100)));
        ck(w.frames.size() == 4, "four frames retained");
        ck(w.frames[1].sequence == 1 && w.frames[2].sequence == 42,
           "the sequence jump is preserved, so the gap is visible");
        int inside = 0;
        for (const auto& f : w.frames)
            if (f.captured_at > T(ms(100)) && f.captured_at < T(ms(3000))) ++inside;
        ck(inside == 0, "and nothing was invented to fill it");
        // The capture times are the ones acquisition supplied, not a local clock.
        ck(w.frames[2].captured_at == T(ms(3000)),
           "the retained timestamp is the CAPTURE time the caller supplied");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
