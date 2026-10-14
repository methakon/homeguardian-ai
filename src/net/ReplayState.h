#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace homeguardian::net {

// Durable replay protection state.
//
// THE PROBLEM IT SOLVES: the hub's per-device sequence watermark lived only in memory,
// so a hub restart reset it to zero and an observation the hub had ALREADY accepted
// became acceptable again. "Restart the hub" must not be a replay primitive.
//
// THE PROBLEM IT MUST NOT CREATE: replay protection that commits BEFORE downstream
// processing turns a processing failure into silent loss. The device is told
// `processing_failed`, retries, and is then refused as a replay because the watermark
// moved anyway. So the record is TWO-PHASE:
//
//   begin(device, sequence, observation_id)
//       IN MEMORY: this exact attempt is now in flight. A concurrent attempt at the same
//       sequence is refused while the marker is fresh; a marker older than the in-flight
//       timeout means the attempt stalled, and the retry may proceed. Deliberately NOT
//       durable: its only job is to stop a second attempt INSIDE this process, so
//       persisting it bought nothing and doubled the cost of every observation.
//   complete(device, sequence, observation_id)
//       DURABLE: the watermark advances and the completed observation id is remembered.
//       This is the one write the replay guarantee rests on, and it happens only after the
//       downstream steps reported success.
//   abandon(device, sequence)
//       memory: processing failed, so the in-flight marker is dropped and the device's
//       retry is accepted rather than refused.
//
// WHAT THIS GUARANTEES
//   * the watermark only ever advances after successful processing, so a failure never
//     consumes a sequence,
//   * the watermark never moves backwards, and an already-accepted observation cannot be
//     replayed by restarting the hub,
//   * a repeat of the most recently COMPLETED observation is recognised
//     (`completed_repeat`) so an unacknowledged retry is answered from the durable record
//     instead of being processed a second time,
//   * it stores a COUNTER, not a timestamp: sequence numbers are immune to clock skew and
//     to a device's clock being changed.
//
// WHAT IT DOES NOT GUARANTEE: exactly-once processing. If the process dies between the
// sink's own durable commit and `complete()`, the next retry is processed again unless the
// device repeats the same observation id (in which case `completed_repeat` answers it).
// The contract is AT-LEAST-ONCE processing with idempotent acknowledgment of an
// unacknowledged repeat, and explicit behavior for every uncertain outcome.
//
// DURABILITY: the state is written to a sibling temporary file which is fsync'd, then
// renamed over the target, and the containing DIRECTORY is fsync'd so the rename itself
// survives a power loss. Every step is explicit and a failed write is reported, never
// swallowed. It carries no credentials: device ids, counters and observation ids only.
class ReplayState {
public:
    struct Config {
        std::string path;                      // empty = disabled (isolated tests only)
        size_t max_devices = 64;               // hard bound on persisted entries
        int64_t in_flight_timeout_ms = 30000;  // a stalled attempt is re-openable after this
    };

    enum class LoadOutcome {
        loaded,     // usable state restored
        absent,     // first run: nothing to restore, and that is not an error
        malformed,  // unusable file: the caller MUST fail closed rather than continue,
                    // because continuing with an empty watermark would accept replays
        unbounded,  // more entries than configured
    };

    // The result of asking to start processing one observation.
    enum class BeginOutcome {
        marked,            // in flight: process it, then call complete()
        completed_repeat,  // this exact observation was already completed durably:
                           // answer it, DO NOT process it again
        replayed,          // at or behind the watermark and not the recorded one
        duplicate,         // the same sequence is already in flight (concurrent attempt)
        unavailable,       // the durable write failed: fail closed
        disabled,          // no durable state configured (isolated tests only)
    };

    using Clock = std::function<int64_t()>;
    static Clock system_clock_ms();

    explicit ReplayState(Config config, Clock clock = system_clock_ms());

    // Read the persisted state. Never throws. A fresh process has no in-flight markers:
    // an attempt whose outcome was never recorded leaves only its watermark behind, so the
    // device's retry is accepted rather than refused as a replay.
    LoadOutcome load(std::string* reason);

    BeginOutcome begin(const std::string& device_id, uint64_t sequence,
                       const std::string& observation_id, std::string* reason);

    // Advance the watermark. Returns false (with a reason) if the durable write failed;
    // the caller must not report a success it cannot stand behind.
    bool complete(const std::string& device_id, uint64_t sequence,
                  const std::string& observation_id, std::string* reason);

    // READ-ONLY query with no side effect: true when this exact observation is the one
    // durably recorded as completed for this device. The ingest consults it before the
    // sequence check, so an unacknowledged retry is answered instead of refused.
    bool is_completed_repeat(const std::string& device_id, uint64_t sequence,
                             const std::string& observation_id) const;

    // Drop the in-flight marker after a failed processing attempt (best effort).
    void abandon(const std::string& device_id, uint64_t sequence);

    // Persist current state without changing it (used at shutdown).
    bool flush(std::string* reason);

    const std::map<std::string, uint64_t>& watermarks() const { return watermarks_; }
    size_t size() const { return watermarks_.size(); }
    bool enabled() const { return !config_.path.empty(); }
    const std::string& path() const { return config_.path; }
    uint64_t write_count() const { return writes_; }
    uint64_t write_failure_count() const { return write_failures_; }
    uint64_t duplicate_refusals() const { return duplicate_refusals_; }
    uint64_t completed_repeats() const { return completed_repeats_; }

private:
    struct Entry {
        uint64_t last_sequence = 0;         // highest COMPLETED sequence
        std::string last_observation_id;    // the observation that completed it
        // IN MEMORY ONLY: never serialised.
        bool in_flight = false;
        uint64_t in_flight_sequence = 0;
        int64_t in_flight_marked_at_ms = 0;
    };

    bool write_atomically(std::string* reason);
    Entry* find(const std::string& device_id);
    bool reserve(const std::string& device_id, Entry** entry, std::string* reason);

    Config config_;
    Clock clock_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, uint64_t> watermarks_;  // mirror for callers (registry seeding)
    uint64_t writes_ = 0;
    uint64_t write_failures_ = 0;
    uint64_t duplicate_refusals_ = 0;
    uint64_t completed_repeats_ = 0;
};

}  // namespace homeguardian::net
