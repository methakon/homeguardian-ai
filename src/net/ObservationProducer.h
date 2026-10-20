#pragma once

// ObservationProducer: the device half of the network observation contract, made testable.
//
// WHY THIS EXISTS
//   The hub side was audited end to end, but nothing in this repository produced the device half in a way
//   a test could drive: the only producer was a fixture helper inside a test file. Integration work on the
//   Mi A3 hub needs the other end of the contract to be a real, bounded, testable thing - including the
//   part that is easy to get wrong and expensive to discover in the field: after a reconnect, a producer
//   must adopt the hub's watermark instead of restarting its own sequence, or every observation it sends
//   after the reconnect is a replay.
//
// WHAT IT IS NOT
//   Not a transport. It builds and consumes MESSAGES; the caller sends them (TLS in the hub deployment, a
//   pipe or a queue in a test). It stores no frame and no recording: observations are bounded scalars, and
//   the feature set it will emit is capped before a message is built.
//
// SYNTHETIC FIRST
//   The default acquisition mode is "synthetic" and the default provenance claim is "synthetic". A
//   producer only claims "real" when it is constructed to, and the hub independently decides what it will
//   record - the claim is never the verdict.

#include "net/DeviceProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace homeguardian::net {

class ObservationProducer {
public:
    struct Config {
        std::string device_id;              // the identity the transport will authenticate
        std::string credential_id;          // certificate fingerprint/CN the hub has registered
        std::string display_name = "synthetic producer";
        std::vector<std::string> capabilities{"motion"};
        std::string source_type = "motion";
        std::string observation_type = "motion-changed-fraction";
        std::string acquisition_mode = "synthetic";   // "live" only when a real sensor feeds it
        std::string provenance_claim = "synthetic";
        size_t max_features = 4;            // bounded: a producer cannot build an oversized message
        size_t max_pending = 32;            // bounded: pending observations are capped, not queued forever
        uint64_t sequence_start = 1;
    };

    using Clock = std::function<int64_t()>;   // milliseconds since the epoch

    explicit ObservationProducer(Config cfg, Clock clock = default_clock());

    static Clock default_clock();

    // Refuses an unusable configuration rather than producing messages that cannot be accepted.
    bool valid(std::string* reason) const;

    // Build the next observation. Returns false when the producer is paused, out of pending budget, or
    // otherwise unable to send - in which case NOTHING is emitted (never a placeholder observation).
    bool next(DeviceMessage* out, std::string* reason);

    // A hello, carrying the identity the transport will authenticate.
    DeviceMessage hello() const;

    // Consume a hub message. A `welcome` is how identity and sequence stay stable across a reconnect: the
    // producer adopts the hub's resume point. Returns false with a reason on a message it cannot use.
    bool handle_hub_message(const DeviceMessage& message, std::string* reason);

    // A network interruption: no messages are produced while paused, and the sequence does NOT rewind, so
    // recovery continues forward instead of replaying.
    void pause(const std::string& why);
    void resume();
    bool paused() const { return paused_; }
    const std::string& pause_reason() const { return pause_reason_; }

    // A payload the caller wants to send (features plus a quantity), converted to a bounded observation.
    // The feature map is capped: beyond max_features the call refuses instead of building a huge frame.
    bool observe(const std::map<std::string, double>& features, DeviceMessage* out, std::string* reason);

    struct Stats {
        uint64_t sent = 0;
        uint64_t refused_locally = 0;         // never left the producer
        uint64_t hub_refusals = 0;
        uint64_t resyncs = 0;                 // times the hub's watermark moved us forward
        std::map<std::string, uint64_t> refusals_by_code;
    };
    const Stats& stats() const { return stats_; }
    uint64_t sequence() const { return sequence_; }
    const std::string& device_id() const { return cfg_.device_id; }
    const std::string& credential_id() const { return cfg_.credential_id; }
    size_t pending() const { return pending_; }

private:
    bool build(uint64_t sequence, DeviceMessage* out, std::string* reason);

    Config cfg_{};
    Clock clock_{};
    uint64_t sequence_ = 1;
    size_t pending_ = 0;
    bool paused_ = false;
    std::string pause_reason_;
    Stats stats_{};
};

}  // namespace homeguardian::net
