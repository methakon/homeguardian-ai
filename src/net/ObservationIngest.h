#pragma once

// The accept path: what happens to one decoded message from an authenticated peer.
//
// This is where "server-side authorization determines acceptance" is implemented.
// For every message it answers, in order: is the peer authenticated? is this
// device known, paired and not revoked? does the credential match the device? does
// the device CLAIM the identity it authenticated as? is consent recorded and not
// withdrawn? is capture enabled? is the source type allowlisted? is the
// observation type allowlisted? are the values, timestamps and sequence sane? is
// there room? and finally - what provenance may the hub record?
//
// Every check is independent, every refusal has a stable reason string, and NO
// check is derived from another: a paired device with no consent is still refused,
// and an authenticated device with capture disabled cannot collect data.
//
// BOUNDS: the ingest holds a bounded queue of accepted-but-undelivered
// observations. A full queue refuses (`queue_full`) rather than growing, which is
// also how a stalled consumer applies back-pressure instead of exhausting memory.
// The recent-observation-id set used for duplicate detection is bounded per device.
//
// The class is deliberately free of sockets, OpenSSL, threads and SQLite so the
// entire authorization and validation path runs in the host test suite; the TLS
// listener and the persistence sink are injected.

#include "core/SecurityObservation.h"
#include "net/DeviceProtocol.h"
#include "net/DeviceRegistry.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace homeguardian::net {

class ObservationIngest {
public:
    struct Config {
        size_t max_pending = 64;             // bounded queue of undelivered observations
        size_t max_feature_keys = 11;        // the hub adds a few metadata keys of its own
        size_t max_recent_ids_per_device = 16;
        std::vector<std::string> allowed_source_types{
            "motion", "contact", "presence", "camera_availability",
            "microphone_availability", "device_availability", "tamper",
            "environmental_threshold"};
        std::vector<std::string> allowed_observation_types{
            "motion-changed-fraction", "audio-rms", "audio-peak", "audio-zero-crossing-rate",
            "activity-level", "availability", "contact-state", "temperature", "humidity",
            "pressure", "illuminance", "tamper-state"};
        int64_t max_future_skew_ms = kMaxFutureSkewMs;
        int64_t max_age_ms = kMaxObservationAgeMs;
        int64_t max_out_of_order_ms = kMaxOutOfOrderMs;
        Config() = default;
    };

    struct Result {
        bool accepted = false;
        RejectReason reason = RejectReason::none;
        std::string detail;                 // bounded, non-sensitive
        std::string observation_id;
        uint64_t sequence = 0;
        bool out_of_order = false;
        bool real = false;                  // the provenance the hub RECORDED
        bool provenance_claim_unsubstantiated = false;
    };

    // Deterministic clock injection: tests and scenarios must not depend on wall
    // time.
    using Clock = std::function<int64_t()>;
    // Where an accepted, validated observation goes. The production sink converts
    // it into the existing pipeline; tests inject their own.
    using Sink = std::function<void(const SecurityObservation&)>;

    static Config defaults() { return Config{}; }
    static Clock system_clock_ms();

    explicit ObservationIngest(DeviceRegistry& registry, Config cfg = defaults(),
                               Clock clock = system_clock_ms());

    // Validate one observation message from an authenticated peer and, when it is
    // accepted, queue it and (if `sink` is set) deliver it.
    Result accept(const PeerContext& peer, const DeviceMessage& message, const Sink& sink);

    // Deliver everything currently queued. Returns the number delivered.
    size_t drain(const Sink& sink);

    size_t pending() const { return pending_.size(); }
    size_t rejected_count() const { return rejected_; }
    size_t accepted_count() const { return accepted_; }
    size_t claim_unsubstantiated_count() const { return claim_unsubstantiated_; }
    RejectReason last_reject_reason() const { return last_reject_; }
    const Config& config() const { return cfg_; }
    void reset_counters();

private:
    // True when this observation id was accepted recently for this device.
    bool is_recent_duplicate(const std::string& device_id, const std::string& observation_id);
    void remember_id(const std::string& device_id, const std::string& observation_id);

    DeviceRegistry& registry_;
    Config cfg_;
    Clock clock_;
    std::deque<SecurityObservation> pending_{};
    std::map<std::string, std::deque<std::string>> recent_ids_{};
    size_t accepted_ = 0;
    size_t rejected_ = 0;
    size_t claim_unsubstantiated_ = 0;
    RejectReason last_reject_ = RejectReason::none;
};

}  // namespace homeguardian::net
