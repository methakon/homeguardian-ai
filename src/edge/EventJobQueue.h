// EventJobQueue — a bounded, coalescing work queue for gated events.
//
// Out-of-tree prototype of the component specified in
// docs/EDGE_VIDEO_INTERFACE_SPECIFICATION.md section 5.3. Mirrors the intended repository
// layout (src/edge/EventJobQueue.h) so it can be moved in unchanged once the gate opens.
//
// Why bounded and coalescing rather than a plain queue:
//
//   * A motion storm must not grow the queue without limit. The bound is HARD: the queue never
//     exceeds max_capacity, whatever arrives.
//   * Continuous movement produces one event, not one job per frame. A job that overlaps the
//     event already queued COALESCES into it; anything else is refused and counted rather than
//     silently dropped, so the refusal is observable.
//   * The worker sleeps on a condition variable. No polling: a poll loop would consume CPU
//     precisely while nothing is happening, which is the failure this architecture exists to
//     avoid.
//   * stop() returns promptly and joins, so no thread outlives its owner. A shutdown that can
//     hang is a shutdown that leaks a thread holding retained media.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hg::edge {

// An event to be recorded. `event_id` identifies one continuous occurrence: two submissions
// carrying the same event_id are the same event and must not become two jobs.
struct EventJob {
    std::uint64_t event_id = 0;
    // The look-back anchor: the FIRST weak hit, not the confirming observation. Retrospective
    // analysis begins here.
    std::chrono::system_clock::time_point anchor{};
    std::chrono::system_clock::time_point event_end{};
    bool provisional = false;   // submitted before the event closed; may be extended
};

struct QueueStats {
    std::uint64_t submitted = 0;
    std::uint64_t coalesced = 0;     // merged into a job already queued or in flight
    std::uint64_t refused = 0;       // non-overlapping arrival with no room
    std::uint64_t completed = 0;     // handed to the handler
    std::size_t peak_depth = 0;
    std::size_t depth = 0;           // snapshot at the time of the call
};

enum class SubmitResult {
    queued,
    coalesced,
    refused_full,
    // A stopped queue refuses rather than silently accepting work that will never run. This is
    // deliberately a distinct value from refused_full: "no room" and "not running" are different
    // conditions, and collapsing them would hide a shutdown race as ordinary backpressure.
    refused_stopped,
};

const char* to_string(SubmitResult r);

// Handles one job. Runs on the worker thread, so it must not call back into the queue's
// mutating API in a way that could deadlock, and must be safe to abandon on stop().
using JobHandler = std::function<void(const EventJob&)>;

struct QueueBounds {
    std::size_t max_capacity = 2;   // hard; the specification says one to two
};

class EventJobQueue {
public:
    EventJobQueue(QueueBounds bounds, JobHandler handler);
    ~EventJobQueue();

    EventJobQueue(const EventJobQueue&) = delete;
    EventJobQueue& operator=(const EventJobQueue&) = delete;

    // Start the worker. Idempotent; a second call does nothing.
    void start();

    // Submit one event. Coalesces when the event is already queued or in flight; otherwise
    // queues if there is room; otherwise refuses and counts.
    SubmitResult submit(const EventJob& job);

    // Stop and join. Prompt: the worker wakes immediately rather than finishing its sleep.
    // Idempotent. After this, submit() is refused rather than silently accepted.
    void stop();

    bool running() const;
    QueueStats stats() const;

    // True once stop() has been called. A long-running handler MUST poll this and return early,
    // or stop() cannot be prompt: stop() joins the worker, so a handler that ignores
    // cancellation makes shutdown wait for the full clip encode. Making cancellation explicit is
    // what lets "returns promptly" be a property of the queue rather than a hope.
    bool cancelled() const;

    // Drop anything not yet started. Used when authority is withdrawn: queued work must be
    // invalidated so nothing further is encoded or published.
    void discard_pending();

    // Number of jobs currently queued (not including one in flight).
    std::size_t depth() const;

private:
    void run();

    QueueBounds bounds_;
    JobHandler handler_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<EventJob> q_;
    // Event ids queued or in flight, so a repeat coalesces instead of duplicating.
    std::deque<std::uint64_t> in_flight_ids_;
    // Event ids recently COMPLETED. Without this, a fast handler reintroduces the duplicate: once
    // a job finishes it is neither queued nor in flight, so the next submission carrying the same
    // event id would be accepted as a new job and produce a second clip for one continuous event.
    // Found by a flaky test that failed 2 runs in 60 - the storm case passes or fails depending
    // on whether the worker happened to finish before the next submit.
    std::deque<std::uint64_t> completed_ids_;
    static constexpr std::size_t kCompletedMemory = 64;
    bool stop_ = false;
    bool started_ = false;
    std::thread worker_;
    QueueStats stats_{};
};

}  // namespace hg::edge
