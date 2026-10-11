#pragma once

// PersistenceAudit: durable, privacy-safe audit of DELIVERED alerts using the
// EXISTING SQLite persistence layer (SQLiteDatabase + SchemaManager +
// AlertRepository). It does not create a parallel storage engine.
//
// WHAT IS PERSISTED (approved metadata only):
//  - Delivered alert records: stable alert_id, severity, message,
//    contributing_event_ids, detected_at timestamp, confidence, rule_name, and
//    details JSON. These already live in the `alerts` table via AlertRepository.
//
// WHAT IS NOT PERSISTED:
//  - Raw audio, raw video, biometric data, or any media payload.
//  - Unnecessary personal identifiers. (Alert messages/details are synthetic
//    simulator strings in this build.)
//  - Consent is NEVER treated as permanent authorization: consent lives only in
//    the in-memory gate for the running process and fails closed after restart.
//    This audit writes alerts, not consent.
//
// RESTART SEMANTICS:
//  - Persisted alerts survive restart and are distinguishable from current-
//    process alerts: records written before this process's start are
//    "historical"; this class records the process start time and exposes a
//    count of pre-existing (historical) alerts loaded at startup.
//  - AlertPolicy dedup/cooldown state is intentionally NOT persisted: the
//    simulator-only lifecycle treats each process start as a fresh condition
//    (documented decision — see docs/ARCHITECTURE.md). Persisting cooldown would
//    add complexity with no concrete requirement for a simulator-only runtime.
//
// FAILURE HANDLING:
//  - A write failure (DB unavailable/error) is caught, recorded, and surfaced as
//    a DEGRADED status; it never reports success when a write failed and never
//    corrupts runtime state. The caller keeps running (degraded) rather than
//    crashing.
//
// RETENTION:
//  - delete_older_than(cutoff) prunes alerts older than a retention cutoff,
//    transactionally, preserving unrelated/newer records.

#include "persistence/AlertRepository.h"
#include "persistence/SQLiteDatabase.h"
#include "persistence/SchemaManager.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace homeguardian {

class PersistenceAudit {
public:
    // Opens (or creates) the database at `path`, initializes/migrates the
    // schema, and records how many alerts already existed (historical). Throws
    // std::runtime_error if the database cannot be opened/initialized, so the
    // caller can decide to run degraded.
    explicit PersistenceAudit(const std::string& path);
    ~PersistenceAudit();

    PersistenceAudit(const PersistenceAudit&) = delete;
    PersistenceAudit& operator=(const PersistenceAudit&) = delete;

    // Persist a delivered alert (metadata only). Returns true on success. On
    // failure, records the failure, marks persistence degraded, and returns
    // false — it does NOT throw and does NOT corrupt runtime state.
    bool record_alert(const Alert& alert);

    // Number of alerts that existed BEFORE this process started (historical).
    size_t historical_alert_count() const { return historical_count_; }

    // Total alerts currently persisted.
    size_t total_alert_count();

    // Retention: delete alerts older than `cutoff` (transactional). Returns the
    // number deleted. On failure returns 0 and marks degraded.
    size_t prune_older_than(std::chrono::system_clock::time_point cutoff);

    // Retention by age window: prune alerts older than `max_age` relative to
    // `now`. Convenience wrapper over prune_older_than used by the scheduled
    // retention lifecycle. `max_age <= 0` means keep forever (no pruning).
    // Returns the number deleted; on failure returns 0 and marks degraded.
    size_t prune_older_than_age(std::chrono::system_clock::duration max_age,
                                std::chrono::system_clock::time_point now =
                                    std::chrono::system_clock::now());

    // Bounded periodic retention: prune alerts older than `max_age` at most
    // once per `interval` (relative to `now`). Returns the number deleted; 0
    // when retention is disabled (max_age <= 0), not yet due, or on failure.
    // This is the production scheduling entry point — it never prunes on every
    // main-loop tick.
    size_t maybe_prune_retention(std::chrono::system_clock::duration max_age,
                                 std::chrono::system_clock::duration interval,
                                 std::chrono::system_clock::time_point now =
                                     std::chrono::system_clock::now());

    // Health / counters.
    bool healthy() const { return healthy_.load(); }
    uint64_t write_failures() const { return write_failures_.load(); }
    uint64_t alerts_written() const { return alerts_written_.load(); }

    // Direct access to the repository (for tests/inspection).
    AlertRepository& alerts() { return *alert_repo_; }

    void close();

private:
    std::shared_ptr<SQLiteDatabase> db_;
    std::unique_ptr<SchemaManager> schema_;
    std::unique_ptr<AlertRepository> alert_repo_;

    std::chrono::system_clock::time_point process_start_;
    size_t historical_count_ = 0;

    // Timestamp of the last retention prune (bounded periodic scheduling).
    std::optional<std::chrono::system_clock::time_point> last_retention_prune_;

    std::atomic<bool> healthy_{true};
    std::atomic<uint64_t> write_failures_{0};
    std::atomic<uint64_t> alerts_written_{0};
    std::mutex m_;
    bool closed_ = false;
};

}  // namespace homeguardian
