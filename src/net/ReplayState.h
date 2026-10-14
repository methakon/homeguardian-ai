#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace homeguardian::net {

// Durable replay protection state.
//
// THE PROBLEM: the hub's per-device sequence watermark lived only in memory, so a hub
// restart reset it to zero and an observation the hub had ALREADY accepted became
// acceptable again. "Restart the hub" must not be a replay primitive.
//
// THE DESIGN:
//   * the watermark for each device is persisted after every accepted observation,
//   * the write is ATOMIC (write a temporary file, then rename over the target), so an
//     interrupted write leaves either the old state or the new one, never a partial,
//   * the state is BOUNDED (max_devices), so a hostile registry cannot grow it without
//     limit,
//   * identity is the certificate-derived device id, so a replay cannot be laundered
//     through a different connection,
//   * it stores a COUNTER, not a timestamp: sequence numbers are unaffected by clock
//     skew or a device's clock being changed, which is why replay protection does not
//     rest on timestamp freshness.
//
// It carries no credentials: device ids and counters only.
class ReplayState {
public:
    struct Config {
        std::string path;                 // empty = disabled (no durable replay state)
        size_t max_devices = 64;          // hard bound on persisted entries
    };

    enum class LoadOutcome {
        loaded,     // usable state restored
        absent,     // first run: nothing to restore, and that is not an error
        malformed,  // unusable file: the caller MUST fail closed rather than continue,
                    // because continuing with an empty watermark would accept replays
        unbounded,  // more entries than configured
    };

    explicit ReplayState(Config config) : config_(std::move(config)) {}

    // Read the persisted watermarks. Never throws.
    LoadOutcome load(std::string* reason);

    // Raise the watermark for a device and persist the state atomically.
    // Returns false with a reason if the durable write failed.
    bool record(const std::string& device_id, uint64_t last_sequence, std::string* reason);

    // Persist current state without changing it (used at shutdown).
    bool flush(std::string* reason);

    const std::map<std::string, uint64_t>& watermarks() const { return watermarks_; }
    size_t size() const { return watermarks_.size(); }
    bool enabled() const { return !config_.path.empty(); }
    const std::string& path() const { return config_.path; }

private:
    bool write_atomically(std::string* reason);

    Config config_;
    std::map<std::string, uint64_t> watermarks_;
    uint64_t writes_ = 0;
    uint64_t write_failures_ = 0;

public:
    uint64_t write_count() const { return writes_; }
    uint64_t write_failure_count() const { return write_failures_; }
};

}  // namespace homeguardian::net
