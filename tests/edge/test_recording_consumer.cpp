// Host tests for RecordingFrameConsumer — the queue-owning adapter's invalidation contract.
//
// The central case is the one the acquisition handoff requires: a test double that captures a
// GENERATION TOKEN when its work starts, is withdrawn while still running, and must not publish.
// A withdrawal that only discards pending work leaves exactly that case live, and a clip encoded on
// withdrawn authority is the thing this adapter exists to prevent.
#include "edge/RecordingFrameConsumer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
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

static std::vector<std::uint8_t> jpeg_of(std::size_t n, std::uint8_t tag = 0xAB) {
    return std::vector<std::uint8_t>(n, tag);
}

static BorrowedFrame frame_of(const std::vector<std::uint8_t>& b, ms at, std::uint64_t seq) {
    return BorrowedFrame{b.data(), b.size(), 640, 360, T(at), seq, true};
}

// The test double the handoff describes: a queued-generation token captured at the START of work,
// carried through it, and required at publication.
struct ClipWorker {
    RecordingFrameConsumer* consumer = nullptr;
    std::atomic<int> published{0};
    std::atomic<int> abandoned{0};
    std::atomic<std::uint64_t> token_at_start{0};

    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    bool may_finish = false;

    void operator()(const EventJob&) {
        // Capture the generation this work was conceived under. Everything after this point is
        // provisional on that generation still being current.
        const auto token = consumer->current_token();
        token_at_start = token;
        {
            std::lock_guard<std::mutex> g(m);
            started = true;
        }
        cv.notify_all();

        // Work in steps, honouring cancellation so a long encode abandons early rather than running
        // to completion only to be thrown away.
        for (int step = 0; step < 200; ++step) {
            if (consumer->cancellation_for(token)()) { ++abandoned; return; }
            std::unique_lock<std::mutex> l(m);
            if (cv.wait_for(l, ms(2), [this] { return may_finish; })) break;
        }

        // The encode is done. Publish only while the token still names the current generation.
        const bool ran = consumer->publish_if_current(token, [this] { ++published; });
        if (!ran) ++abandoned;
    }

    bool wait_started() {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::seconds(5), [this] { return started; });
    }
    void release() {
        { std::lock_guard<std::mutex> g(m); may_finish = true; }
        cv.notify_all();
    }
};

int main() {
    std::printf("== a frame is admitted exactly as the ring consumer admits it ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        EventJobQueue q(QueueBounds{}, [](const EventJob&) {});
        RecordingFrameConsumer c(ring, q);

        auto j = jpeg_of(2048);
        c.on_frame(frame_of(j, ms(1000), 1));
        ck(c.counts().accepted == 1, "a validated frame is retained");
        ck(ring.stats().stored == 1, "and the ring holds it");
        ck(ring.stats().frames == 1, "one frame");

        // The refusals are the ring consumer's rules, not a second copy - so they behave identically.
        auto unvalidated = frame_of(j, ms(1100), 2);
        unvalidated.validated = false;
        c.on_frame(unvalidated);
        ck(c.counts().refused_unvalidated == 1, "an unvalidated frame is refused");

        auto huge = jpeg_of(512u * 1024u + 1);
        c.on_frame(frame_of(huge, ms(1200), 3));
        ck(c.counts().refused_oversize == 1, "an over-cap frame is refused");

        BorrowedFrame nullf{nullptr, 0, 640, 360, T(ms(1300)), 4, true};
        c.on_frame(nullf);
        ck(c.counts().refused_null == 1, "a null frame is refused");
    }

    std::printf("== THE REQUIRED CASE: in-flight work is invalidated by the generation ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        ClipWorker worker;
        EventJobQueue q(QueueBounds{}, [&worker](const EventJob& j) { worker(j); });
        RecordingFrameConsumer c(ring, q);
        worker.consumer = &c;

        c.on_frame(frame_of(jpeg_of(4096), ms(1000), 1));
        ck(ring.stats().frames == 1, "a frame is retained before the event");

        const auto gen_before = c.current_token();
        q.start();
        q.submit(EventJob{7, T(ms(1000)), T(ms(1200)), false});
        ck(worker.wait_started(), "the worker started and captured its token");
        const auto token = worker.token_at_start.load();
        ck(token == gen_before, "the in-flight work captured the generation current at its start");
        ck(!c.cancellation_for(token)(), "and is NOT yet cancelled");

        // Withdraw WHILE the work is running. discard_pending cannot reach this job: it has already
        // left the queue. Only the generation can invalidate it.
        c.on_authority_changed(false);

        ck(c.cancellation_for(token)(), "after withdrawal the in-flight work reports cancelled");
        ck(c.superseded(token), "and its token is superseded");
        ck(c.current_token() != token, "the generation advanced");
        ck(ring.stats().frames == 0, "retained bytes were cleared");
        ck(!ring.authorized(), "and the ring is unauthorized");
        ck(c.lifecycle().invalidations == 1, "the invalidation is counted");

        worker.release();
        for (int i = 0; i < 200 && worker.abandoned.load() == 0; ++i)
            std::this_thread::sleep_for(ms(10));
        q.stop();

        ck(worker.published.load() == 0,
           "NOTHING was published: work withdrawn mid-flight produced no clip");
        ck(worker.abandoned.load() == 1, "it abandoned instead");
        ck(c.lifecycle().stale_publishes_refused >= 1, "and the stale publish was refused and counted");
    }

    std::printf("== a stale token can never publish, however it is used ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        EventJobQueue q(QueueBounds{}, [](const EventJob&) {});
        RecordingFrameConsumer c(ring, q);

        const auto old = c.current_token();
        int published = 0;
        ck(c.publish_if_current(old, [&] { ++published; }), "a current token publishes");
        ck(published == 1, "and the work ran");

        c.on_authority_changed(false);
        ck(!c.publish_if_current(old, [&] { ++published; }), "the old token is refused after withdrawal");
        ck(published == 1, "so nothing further ran");

        // Authority RETURNING must not resurrect the withdrawn generation's token.
        c.on_authority_changed(true);
        ck(!c.publish_if_current(old, [&] { ++published; }), "nor is it revived by re-authorization");
        ck(c.superseded(old), "the old token stays superseded");
        const auto fresh = c.current_token();
        ck(fresh != old, "a new generation is issued instead");
        ck(c.publish_if_current(fresh, [&] { ++published; }), "and only fresh work may publish");
        ck(published == 2, "which ran");
    }

    std::printf("== withdrawal also drops QUEUED work, not only in-flight ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        std::mutex m;
        std::condition_variable cv;
        bool hold = true;
        int handled = 0;

        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) {
            std::unique_lock<std::mutex> l(m);
            cv.wait_for(l, std::chrono::seconds(3), [&] { return !hold; });
            ++handled;
        });
        RecordingFrameConsumer c(ring, q);
        q.start();

        // One job goes in flight (the handler blocks), and the rest queue behind it. `depth()`
        // deliberately excludes the in-flight job, so a single submission leaves depth()==0 and
        // tests nothing about queued work - the fixture has to actually queue some.
        q.submit(EventJob{1, T(ms(100)), T(ms(200)), false});
        std::this_thread::sleep_for(ms(60));
        q.submit(EventJob{2, T(ms(300)), T(ms(400)), false});
        q.submit(EventJob{3, T(ms(500)), T(ms(600)), false});
        const auto d_before = q.depth();
        c.on_authority_changed(false);
        const auto d_after = q.depth();

        ck(d_before >= 1, "work was queued behind the in-flight job");
        ck(d_after == 0, "withdrawal discarded the pending work");
        ck(c.lifecycle().discarded_pending == 1, "and it is counted");

        { std::lock_guard<std::mutex> g(m); hold = false; }
        cv.notify_all();
        std::this_thread::sleep_for(ms(80));
        q.stop();
        ck(handled <= 1, "only the job already in flight could have run");
    }

    std::printf("== the adapter adds no second opinion on admission ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        EventJobQueue q(QueueBounds{}, [](const EventJob&) {});
        RecordingFrameConsumer c(ring, q);

        // With the ring unauthorized, the ring's own authority check refuses - and the adapter does
        // not re-count it as its own refusal, so the two cannot disagree about the same event.
        c.on_authority_changed(false);
        const auto refusals_before = c.counts().refused_unvalidated + c.counts().refused_oversize +
                                     c.counts().refused_bad_geometry + c.counts().refused_null;
        c.on_frame(frame_of(jpeg_of(4096), ms(1000), 1));
        const auto refusals_after = c.counts().refused_unvalidated + c.counts().refused_oversize +
                                    c.counts().refused_bad_geometry + c.counts().refused_null;
        ck(refusals_after == refusals_before,
           "an unauthorized retain is reported by the RING's stats, not double-counted here");
        ck(ring.stats().frames == 0, "and nothing was retained");
    }

    std::printf("== a generation is monotonic and never reused ==\n");
    {
        RingBounds rb; rb.max_age = ms(600000);
        EventRing ring(rb);
        ring.set_authorized(true);
        EventJobQueue q(QueueBounds{}, [](const EventJob&) {});
        RecordingFrameConsumer c(ring, q);

        std::vector<std::uint64_t> seen;
        for (int i = 0; i < 5; ++i) {
            c.on_authority_changed(false);
            seen.push_back(c.current_token());
            c.on_authority_changed(true);
        }
        bool increasing = true;
        for (std::size_t i = 1; i < seen.size(); ++i)
            if (seen[i] <= seen[i - 1]) increasing = false;
        ck(increasing, "each withdrawal advances the generation");
        ck(seen.front() > 0, "and it never starts at the default token");
        ck(c.lifecycle().authorize_notifications == 5, "authorizations are counted too");
        ck(c.lifecycle().withdraw_notifications == 5, "as are withdrawals");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
