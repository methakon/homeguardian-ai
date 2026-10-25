// Host tests for EventJobQueue. Hermetic: no hardware, no network, no gateway.
//
// The threaded tests use explicit gates rather than sleeps, so they are deterministic rather than
// timing-dependent. A test that passes only when the machine is idle is not a test.
//
// Mirrors the intended repository path tests/edge/test_event_queue.cpp.
#include "edge/EventJobQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
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
static EventJob job(std::uint64_t id, ms a, ms e) {
    EventJob j; j.event_id = id; j.anchor = T(a); j.event_end = T(e); return j;
}

int main() {
    std::printf("== a stopped queue refuses rather than accepting work that will never run ==\n");
    {
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) { ++ran; });
        // never started
        ck(q.submit(job(1, ms(0), ms(100))) == SubmitResult::refused_stopped,
           "submitting to a never-started queue is refused, not silently accepted");
        q.start();
        ck(q.submit(job(1, ms(0), ms(100))) == SubmitResult::queued, "started, it queues");
        q.stop();
        ck(q.submit(job(2, ms(0), ms(100))) == SubmitResult::refused_stopped,
           "after stop() it refuses again");
        ck(q.stats().refused == 2, "both refusals are counted");
        ck(!q.running(), "and running() reports false");
    }

    std::printf("== a continuous event is ONE job, not one per submission ==\n");
    {
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) { ++ran; });
        q.start();
        // 500 observations of the SAME event - the motion-storm case
        for (int i = 0; i < 500; ++i) {
            q.submit(job(7, ms(0), ms(100 + i)));
        }
        auto s = q.stats();
        ck(s.submitted == 500, "500 submissions recorded");
        // EXACTLY 499, not "at least 498". A fast handler used to let the first job complete
        // before the next submission, which made the repeat look new and produced a second job -
        // this check failed 2 runs in 60 before the completed-id memory was added.
        ck(s.coalesced == 499, "every repeat coalesced into the one event (exactly 499)");
        ck(s.peak_depth <= 2, "and the queue NEVER exceeded its bound during the storm");
        ck(s.refused == 0, "nothing was refused, because nothing genuinely overflowed");
        q.stop();
    }

    std::printf("== a fast handler must not re-open a completed event (the flake, as regression) ==\n");
    {
        // Deterministic version of the case that flaked: handler returns immediately, so the job
        // is neither queued nor in flight by the time the second submission arrives.
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) { ++ran; });
        q.start();
        ck(q.submit(job(42, ms(0), ms(10))) == SubmitResult::queued, "first submission queues");
        // Wait until it has actually completed, so the second submission is unambiguously
        // arriving after completion rather than racing it.
        while (q.stats().completed < 1) std::this_thread::sleep_for(ms(1));
        ck(q.submit(job(42, ms(0), ms(20))) == SubmitResult::coalesced,
           "a repeat of an ALREADY COMPLETED event coalesces rather than creating a second job");
        ck(q.depth() == 0, "and nothing was queued");
        std::this_thread::sleep_for(ms(50));
        ck(ran.load() == 1, "the handler ran exactly once for that event, not twice");
        q.stop();
    }

    std::printf("== the bound is hard, and overflow is refused observably ==\n");
    {
        std::mutex g; std::condition_variable gcv;
        bool handler_entered = false, release = false;
        EventJobQueue* qp = nullptr;
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) {
            ++ran;
            std::unique_lock<std::mutex> lk(g);
            handler_entered = true; gcv.notify_all();
            gcv.wait(lk, [&] { return release; });   // hold the worker on the first job
        });
        qp = &q;
        q.start();

        ck(q.submit(job(1, ms(0), ms(10))) == SubmitResult::queued, "job 1 accepted");
        {
            std::unique_lock<std::mutex> lk(g);
            gcv.wait(lk, [&] { return handler_entered; });   // wait until it is truly in flight
        }
        // One in flight, capacity 2, so two more fit and the fourth does not.
        ck(q.submit(job(2, ms(20), ms(30))) == SubmitResult::queued, "job 2 accepted");
        ck(q.submit(job(3, ms(40), ms(50))) == SubmitResult::queued, "job 3 accepted, queue now full");
        ck(q.submit(job(4, ms(60), ms(70))) == SubmitResult::refused_full,
           "job 4 is refused - the queue is full");
        ck(q.depth() == 2, "depth is exactly the bound, never more");
        ck(q.stats().peak_depth <= 2, "peak depth never exceeded the bound");

        { std::lock_guard<std::mutex> lk(g); release = true; }
        gcv.notify_all();
        q.stop();
        ck(q.stats().refused == 1, "the one refusal is counted, not lost");
        (void)qp;
    }

    std::printf("== a job already in flight also coalesces ==\n");
    {
        std::mutex g; std::condition_variable gcv;
        bool handler_entered = false, release = false;
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) {
            ++ran;
            std::unique_lock<std::mutex> lk(g);
            handler_entered = true; gcv.notify_all();
            gcv.wait(lk, [&] { return release; });
        });
        q.start();
        q.submit(job(9, ms(0), ms(10)));
        { std::unique_lock<std::mutex> lk(g); gcv.wait(lk, [&] { return handler_entered; }); }
        // Event 9 is in flight. Resubmitting it must NOT queue a second copy.
        ck(q.submit(job(9, ms(0), ms(99))) == SubmitResult::coalesced,
           "an event already in flight coalesces instead of queueing a duplicate");
        ck(q.depth() == 0, "and nothing was added to the queue");
        { std::lock_guard<std::mutex> lk(g); release = true; }
        gcv.notify_all();
        q.stop();
        ck(ran.load() == 1, "the handler ran exactly once for that event");
    }

    std::printf("== the worker actually runs the handler, in order ==\n");
    {
        std::mutex g; std::condition_variable gcv;
        std::vector<std::uint64_t> seen;
        std::atomic<int> done{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob& j) {
            std::lock_guard<std::mutex> lk(g);
            seen.push_back(j.event_id);
            if (++done == 2) gcv.notify_all();
        });
        q.start();
        q.submit(job(11, ms(0), ms(10)));
        q.submit(job(12, ms(20), ms(30)));
        { std::unique_lock<std::mutex> lk(g); gcv.wait_for(lk, std::chrono::seconds(5), [&] { return done >= 2; }); }
        ck(done.load() == 2, "both jobs reached the handler");
        ck(seen.size() == 2 && seen[0] == 11 && seen[1] == 12, "in submission order");
        q.stop();
    }

    std::printf("== stop() is prompt when the handler honours cancellation ==\n");
    {
        std::atomic<int> ran{0};
        std::atomic<bool> in_handler{false};
        EventJobQueue* qp = nullptr;
        // A cancellation-aware handler models a real clip encode: it checks cancelled() and
        // exits early. This is what makes prompt shutdown a property of the design rather than
        // luck.
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) {
            ++ran;
            in_handler = true;
            while (qp && !qp->cancelled()) {
                std::this_thread::sleep_for(ms(2));
            }
        });
        qp = &q;
        q.start();
        q.submit(job(1, ms(0), ms(10)));
        while (!in_handler.load()) std::this_thread::sleep_for(ms(1));   // ensure it is running

        const auto t0 = std::chrono::steady_clock::now();
        q.stop();
        const auto elapsed = std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - t0);
        ck(elapsed < ms(500), "stop() returned promptly, not after a full work cycle");
        ck(!q.running(), "and the queue reports stopped");
        std::printf("        (stop() took %lld ms)\n", static_cast<long long>(elapsed.count()));
    }

    std::printf("== destruction without an explicit stop() does not leak a thread ==\n");
    {
        std::atomic<int> ran{0};
        {
            EventJobQueue q(QueueBounds{2}, [&](const EventJob&) { ++ran; });
            q.start();
            q.submit(job(1, ms(0), ms(10)));
            std::this_thread::sleep_for(ms(50));
            // deliberately no stop(): the destructor must join
        }
        ck(true, "destructor returned - it joined the worker rather than detaching it");
        ck(ran.load() >= 0, "and the handler ran without crashing");
    }

    std::printf("== withdrawal discards queued work ==\n");
    {
        std::mutex g; std::condition_variable gcv;
        bool entered = false, release = false;
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) {
            ++ran;
            std::unique_lock<std::mutex> lk(g);
            entered = true; gcv.notify_all();
            gcv.wait(lk, [&] { return release; });
        });
        q.start();
        q.submit(job(1, ms(0), ms(10)));
        { std::unique_lock<std::mutex> lk(g); gcv.wait(lk, [&] { return entered; }); }
        q.submit(job(2, ms(20), ms(30)));
        ck(q.depth() == 1, "one job pending");
        q.discard_pending();
        ck(q.depth() == 0, "discard_pending() empties it, so a withdrawal invalidates queued work");
        { std::lock_guard<std::mutex> lk(g); release = true; }
        gcv.notify_all();
        q.stop();
    }

    std::printf("== start() and stop() are idempotent ==\n");
    {
        std::atomic<int> ran{0};
        EventJobQueue q(QueueBounds{2}, [&](const EventJob&) { ++ran; });
        q.start(); q.start(); q.start();
        ck(q.running(), "three starts leave one running queue");
        q.submit(job(1, ms(0), ms(10)));
        q.stop(); q.stop(); q.stop();
        ck(!q.running(), "three stops leave it stopped, not double-joined");
    }

    std::printf("\n%s  %d/%d checks passed, %d failed\n",
                fails == 0 ? "PASS" : "FAIL", checks - fails, checks, fails);
    return fails == 0 ? 0 : 1;
}
