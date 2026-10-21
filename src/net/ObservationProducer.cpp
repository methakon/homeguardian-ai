#include "net/ObservationProducer.h"

#include <algorithm>

namespace homeguardian::net {

ObservationProducer::Clock ObservationProducer::default_clock() {
    return [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
}

ObservationProducer::ObservationProducer(Config cfg, Clock clock)
    : cfg_(std::move(cfg)), clock_(std::move(clock)), sequence_(cfg_.sequence_start) {}

bool ObservationProducer::valid(std::string* reason) const {
    const auto fail = [&](const char* why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (cfg_.device_id.empty()) return fail("no device id: the hub authenticates an identity, not a claim");
    if (cfg_.credential_id.empty()) return fail("no credential: the hub must have a registered fingerprint");
    if (cfg_.source_type.empty() || cfg_.observation_type.empty()) {
        return fail("no source or observation type: the hub refuses unknown types");
    }
    if (cfg_.acquisition_mode != "live" && cfg_.acquisition_mode != "synthetic") {
        return fail("acquisition mode must be 'live' or 'synthetic'");
    }
    if (cfg_.provenance_claim != "real" && cfg_.provenance_claim != "synthetic") {
        return fail("provenance claim must be 'real' or 'synthetic'");
    }
    if (cfg_.acquisition_mode == "synthetic" && cfg_.provenance_claim == "real") {
        // Refused locally rather than at the hub: a synthetic source claiming real evidence is the exact
        // inconsistency the hub records as unsubstantiated, and a producer should not send it at all.
        return fail("a synthetic acquisition cannot claim real provenance");
    }
    if (cfg_.max_features == 0) return fail("max_features must be positive");
    return true;
}

DeviceMessage ObservationProducer::hello() const {
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::hello;
    m.device_id_claim = cfg_.device_id;
    m.display_name_claim = cfg_.display_name;
    m.capabilities = cfg_.capabilities;
    m.sequence = sequence_;
    return m;
}

bool ObservationProducer::build(uint64_t sequence, DeviceMessage* out, std::string* reason) {
    if (out == nullptr) return false;
    DeviceMessage m;
    m.protocol_version_major = kProtocolVersionMajor;
    m.protocol_version_minor = kProtocolVersionMinor;
    m.kind = MessageKind::observation;
    m.observation_id = cfg_.device_id + "-obs-" + std::to_string(sequence);
    m.device_id_claim = cfg_.device_id;
    m.source_type = cfg_.source_type;
    m.observation_type = cfg_.observation_type;
    m.acquisition_mode = cfg_.acquisition_mode;
    m.acquired_at_ms = clock_();
    m.sequence = sequence;
    m.features = nlohmann::json::object();
    m.unit = "fraction";
    m.provenance_claim = cfg_.provenance_claim;
    (void)reason;
    *out = std::move(m);
    return true;
}

bool ObservationProducer::next(DeviceMessage* out, std::string* reason) {
    if (paused_) {
        ++stats_.refused_locally;
        if (reason != nullptr) *reason = "paused: " + pause_reason_ + " (no placeholder observation is sent)";
        return false;
    }
    if (pending_ >= cfg_.max_pending) {
        ++stats_.refused_locally;
        if (reason != nullptr) *reason = "the pending budget is exhausted; nothing was emitted";
        return false;
    }
    std::string why;
    if (!valid(&why)) {
        ++stats_.refused_locally;
        if (reason != nullptr) *reason = why;
        return false;
    }
    if (!build(sequence_, out, &why)) {
        ++stats_.refused_locally;
        if (reason != nullptr) *reason = why;
        return false;
    }
    out->features[cfg_.observation_type] = 0.25;      // a bounded synthetic scalar by default
    ++sequence_;
    ++stats_.sent;
    return true;
}

bool ObservationProducer::observe(const std::map<std::string, double>& features, DeviceMessage* out,
                                  std::string* reason) {
    if (features.size() > cfg_.max_features) {
        ++stats_.refused_locally;
        if (reason != nullptr) {
            *reason = "the feature set is larger than the bounded maximum (" + std::to_string(features.size()) +
                      " > " + std::to_string(cfg_.max_features) + "); no message was built";
        }
        return false;
    }
    if (!next(out, reason)) return false;
    // next() consumed a sequence number for a message we are about to REPLACE, so build it here instead.
    --sequence_;
    --stats_.sent;
    if (!build(sequence_, out, reason)) {
        ++stats_.refused_locally;
        return false;
    }
    for (const auto& [key, value] : features) {
        out->features[key] = value;
    }
    // The frame bound is the hub's; a message we cannot frame is refused here rather than at the wire.
    if (!frame(encode(*out)).has_value()) {
        ++stats_.refused_locally;
        if (reason != nullptr) *reason = "the message exceeds the frame bound; it was not emitted";
        return false;
    }
    ++sequence_;
    ++stats_.sent;
    return true;
}

bool ObservationProducer::handle_hub_message(const DeviceMessage& message, std::string* reason) {
    switch (message.kind) {
        case MessageKind::welcome: {
            // The reconnect case. The hub says the highest sequence it has ALREADY accepted; the producer
            // resumes above it. Without this, every message after a reconnect would be a replay.
            const uint64_t resume_after = message.resume_after_sequence;
            if (resume_after >= sequence_) {
                sequence_ = resume_after + 1;
                ++stats_.resyncs;
            }
            return true;
        }
        case MessageKind::acknowledgement:
            // An acknowledgement clears one pending observation.
            if (pending_ > 0) --pending_;
            return true;
        case MessageKind::error: {
            ++stats_.hub_refusals;
            const std::string code = reject_reason_to_string(message.reason);
            ++stats_.refusals_by_code[code];
            if (reason != nullptr) *reason = "the hub refused a message: " + code +
                                             (message.detail.empty() ? "" : " (" + message.detail + ")");
            // A replayed or duplicate sequence means our view of the watermark is stale: the next hello
            // will adopt the hub's, which is how a producer recovers without skipping observations.
            return true;
        }
        case MessageKind::goodbye:
            pause("the hub said goodbye");
            return true;
        case MessageKind::heartbeat:
            return true;
        case MessageKind::observation:
        case MessageKind::hello:
            if (reason != nullptr) *reason = "a hub message of this kind is not expected from the hub";
            return false;
    }
    return false;
}

void ObservationProducer::pause(const std::string& why) {
    paused_ = true;
    pause_reason_ = why;
}

void ObservationProducer::resume() {
    paused_ = false;
    pause_reason_.clear();
}

}  // namespace homeguardian::net
