#pragma once

// RuntimeStatus: bounded, privacy-safe observability counters for the running
// application.
//
// All counters are in-memory and RESET on process restart (they describe the
// current process). Persistent audit metadata (delivered alerts, event records)
// lives in SQLite via PersistenceAudit — these two are deliberately separate so
// a counter can never be mistaken for durable audit history, and vice versa.
//
// Safety: this records ONLY counts and states. It never logs or stores media
// payloads, secrets, credentials, raw audio/video, or personal identifiers, and
// it cannot grant consent or enable capture. Counters are atomic and bounded
// (they are fixed-size integers, not per-sample accumulators), so observability
// never grows memory without bound and never emits one message per sample.

#include "core/AppState.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace homeguardian {

struct RuntimeStatus {
    // --- Application lifecycle / mode ---
    std::atomic<AppState> app_state{AppState::Constructed};
    std::atomic<bool> capture_enabled{false};      // config kill-switch
    std::atomic<bool> persistence_healthy{true};   // false once a write fails

    // --- Media delivery (per process) ---
    std::atomic<uint64_t> media_authorized{0};     // fan-outs authorized
    std::atomic<uint64_t> media_denied{0};         // fan-outs blocked (consent)

    // --- Event processing (per process) ---
    std::atomic<uint64_t> events_processed{0};     // reached the pipeline
    std::atomic<uint64_t> events_rejected{0};      // failed validation
    std::atomic<uint64_t> events_failed{0};        // processing threw

    // --- Alerts / notifications (per process) ---
    std::atomic<uint64_t> alerts_generated{0};     // rule fired
    std::atomic<uint64_t> alerts_delivered{0};     // passed dedup/cooldown
    std::atomic<uint64_t> alerts_suppressed{0};    // deduplicated
    std::atomic<uint64_t> alerts_persist_failed{0};// durable write failed

    // --- Queues / backpressure (current depth, not cumulative) ---
    std::atomic<uint64_t> speaker_queue_depth{0};
    std::atomic<uint64_t> speaker_queue_overflow{0};  // cumulative drops
    std::atomic<uint64_t> speaker_played{0};

    // --- Lifecycle outcomes (per process) ---
    std::atomic<uint64_t> startups{0};
    std::atomic<uint64_t> clean_shutdowns{0};
    std::atomic<uint64_t> recoveries{0};

    RuntimeStatus() = default;
    RuntimeStatus(const RuntimeStatus&) = delete;
    RuntimeStatus& operator=(const RuntimeStatus&) = delete;

    // Reset the per-process counters (used on a fresh start within a process;
    // does NOT touch persistent audit metadata).
    void reset_counters();

    // A compact, single-line, privacy-safe summary for logging/inspection.
    // Contains no media, secrets, or personal data.
    std::string summary() const;
};

}  // namespace homeguardian
