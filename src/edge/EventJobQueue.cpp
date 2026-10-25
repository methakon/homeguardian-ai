#include "edge/EventJobQueue.h"

#include <algorithm>
#include <utility>

namespace hg::edge {

const char* to_string(SubmitResult r) {
    switch (r) {
        case SubmitResult::queued:           return "queued";
        case SubmitResult::coalesced:        return "coalesced";
        case SubmitResult::refused_full:     return "refused_full";
        case SubmitResult::refused_stopped:  return "refused_stopped";
    }
    return "unknown";
}

EventJobQueue::EventJobQueue(QueueBounds bounds, JobHandler handler)
    : bounds_(bounds), handler_(std::move(handler)) {}

EventJobQueue::~EventJobQueue() {
    // Destroying a queue must not leave a thread behind. stop() is idempotent and joins.
    stop();
}

void EventJobQueue::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) return;
    started_ = true;
    stop_ = false;
    worker_ = std::thread([this] { run(); });
}

void EventJobQueue::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!started_) {
            stop_ = true;
            return;
        }
        stop_ = true;
    }
    // Wake immediately rather than letting the worker finish a sleep, so stop() is prompt.
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(m_);
    started_ = false;
}

bool EventJobQueue::running() const {
    std::lock_guard<std::mutex> lk(m_);
    return started_ && !stop_;
}

bool EventJobQueue::cancelled() const {
    // No lock ordering hazard: this is a single bool read, and the handler calling it from the
    // worker thread while stop() writes it is exactly the intended use.
    std::lock_guard<std::mutex> lk(m_);
    return stop_;
}

SubmitResult EventJobQueue::submit(const EventJob& job) {
    std::lock_guard<std::mutex> lk(m_);
    ++stats_.submitted;

    // A stopped queue refuses. Accepting work here would let a caller believe an event will be
    // recorded when nothing will ever run.
    if (stop_ || !started_) {
        ++stats_.refused;
        return SubmitResult::refused_stopped;
    }

    // Coalesce first, and check the in-flight set as well as the queue: without the in-flight
    // check, continuous movement would enqueue a second job for an event already being
    // recorded, which is exactly the duplicate the bound exists to prevent.
    auto same_event = [&](std::uint64_t id) { return id == job.event_id; };
    for (auto& queued : q_) {
        if (same_event(queued.event_id)) {
            if (job.event_end > queued.event_end) queued.event_end = job.event_end;
            queued.provisional = job.provisional;
            ++stats_.coalesced;
            return SubmitResult::coalesced;
        }
    }
    if (std::any_of(in_flight_ids_.begin(), in_flight_ids_.end(), same_event)) {
        ++stats_.coalesced;
        return SubmitResult::coalesced;
    }
    // Already completed. Checked last but checked nevertheless: without it, a handler that
    // finishes before the next submission turns a continuous event into a stream of duplicate
    // jobs, because a completed job is neither queued nor in flight.
    if (std::any_of(completed_ids_.begin(), completed_ids_.end(), same_event)) {
        ++stats_.coalesced;
        return SubmitResult::coalesced;
    }

    if (q_.size() >= bounds_.max_capacity) {
        ++stats_.refused;
        return SubmitResult::refused_full;   // observable, never a silent drop
    }

    q_.push_back(job);
    stats_.peak_depth = std::max(stats_.peak_depth, q_.size());
    cv_.notify_one();
    return SubmitResult::queued;
}

void EventJobQueue::run() {
    for (;;) {
        EventJob job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;      // stop drains nothing; pending work is dropped
            if (stop_) return;                    // prompt exit even with work queued
            job = q_.front();
            q_.pop_front();
            in_flight_ids_.push_back(job.event_id);
        }

        if (handler_) handler_(job);

        {
            std::lock_guard<std::mutex> lk(m_);
            ++stats_.completed;
            auto it = std::find(in_flight_ids_.begin(), in_flight_ids_.end(), job.event_id);
            if (it != in_flight_ids_.end()) in_flight_ids_.erase(it);
            // Remember that this event has been handled, so a later submission carrying the same
            // id coalesces instead of producing a second clip. Bounded, so the memory cannot grow
            // without limit over a long run.
            completed_ids_.push_back(job.event_id);
            while (completed_ids_.size() > kCompletedMemory) completed_ids_.pop_front();
        }
    }
}

void EventJobQueue::discard_pending() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
}

std::size_t EventJobQueue::depth() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
}

QueueStats EventJobQueue::stats() const {
    std::lock_guard<std::mutex> lk(m_);
    QueueStats s = stats_;
    s.depth = q_.size();
    return s;
}

}  // namespace hg::edge
