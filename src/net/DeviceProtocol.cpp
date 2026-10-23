#include "net/DeviceProtocol.h"

#include <cmath>

namespace homeguardian::net {

namespace {

bool bounded_string(const nlohmann::json& value, size_t max_chars) {
    return value.is_string() && value.get_ref<const std::string&>().size() <= max_chars;
}

std::string bounded_detail(const std::string& raw) {
    constexpr size_t kMaxDetail = 96;
    if (raw.size() <= kMaxDetail) return raw;
    return raw.substr(0, kMaxDetail);
}

// A feature value must be a finite number, a bool, or a short string. Rejecting
// NaN/inf matters: a peer must not be able to inject a non-finite number that
// later poisons a mean or a ratio.
bool feature_value_ok(const nlohmann::json& value) {
    if (value.is_boolean()) return true;
    if (value.is_number()) {
        const double d = value.get<double>();
        return std::isfinite(d);
    }
    if (value.is_string()) {
        return value.get_ref<const std::string&>().size() <= kMaxFeatureStringChars;
    }
    return false;  // no nested objects/arrays/null: keeps the payload flat and bounded
}

bool features_ok(const nlohmann::json& features) {
    if (!features.is_object()) return false;
    if (features.size() > kMaxFeatureKeys) return false;
    for (auto it = features.begin(); it != features.end(); ++it) {
        if (!feature_value_ok(it.value())) return false;
    }
    return true;
}

// Read `sequence` from the wire. The check is deliberately NOT
// `is_number_unsigned()`: that reflects how the JSON value was CONSTRUCTED in
// process (an `int` literal is "integer", a parsed wire value is "unsigned"), so
// relying on it would make the decoder reject a legitimate payload depending on
// who built it. Any JSON integer is accepted and the RANGE is enforced instead.
bool read_sequence(const nlohmann::json& payload, uint64_t* out, RejectReason* reason,
                   std::string* detail) {
    auto fail = [reason, detail](RejectReason r, const std::string& why) {
        if (reason != nullptr) *reason = r;
        if (detail != nullptr) *detail = bounded_detail(why);
        return false;
    };
    if (!payload.contains("sequence") || !payload["sequence"].is_number_integer()) {
        return fail(RejectReason::bad_json, "sequence missing or not an integer");
    }
    uint64_t value = 0;
    if (payload["sequence"].is_number_unsigned()) {
        value = payload["sequence"].get<uint64_t>();
    } else {
        const int64_t signed_value = payload["sequence"].get<int64_t>();
        if (signed_value <= 0) return fail(RejectReason::invalid_value, "sequence must be positive");
        value = static_cast<uint64_t>(signed_value);
    }
    if (value == 0 || value > kMaxSequence) {
        return fail(RejectReason::invalid_value, "sequence out of range");
    }
    if (out != nullptr) *out = value;
    return true;
}

std::optional<double> optional_unit_double(const nlohmann::json& payload, const char* key,
                                           RejectReason* reason) {
    if (!payload.contains(key)) return std::nullopt;
    const nlohmann::json& value = payload[key];
    if (!value.is_number()) {
        if (reason != nullptr) *reason = RejectReason::bad_json;
        return std::nullopt;
    }
    const double d = value.get<double>();
    if (!std::isfinite(d) || d < 0.0 || d > 1.0) {
        if (reason != nullptr) *reason = RejectReason::invalid_value;
        return std::nullopt;
    }
    return d;
}

}  // namespace

const char* message_kind_to_string(MessageKind kind) {
    switch (kind) {
        case MessageKind::hello: return "hello";
        case MessageKind::welcome: return "welcome";
        case MessageKind::observation: return "observation";
        case MessageKind::acknowledgement: return "ack";
        case MessageKind::heartbeat: return "heartbeat";
        case MessageKind::goodbye: return "goodbye";
        case MessageKind::error: return "error";
    }
    return "error";
}

std::optional<MessageKind> message_kind_from_string(const std::string& value) {
    if (value == "hello") return MessageKind::hello;
    if (value == "welcome") return MessageKind::welcome;
    if (value == "observation") return MessageKind::observation;
    if (value == "ack") return MessageKind::acknowledgement;
    if (value == "heartbeat") return MessageKind::heartbeat;
    if (value == "goodbye") return MessageKind::goodbye;
    if (value == "error") return MessageKind::error;
    return std::nullopt;
}

const char* reject_reason_to_string(RejectReason reason) {
    switch (reason) {
        case RejectReason::none: return "ok";
        case RejectReason::bad_frame: return "bad_frame";
        case RejectReason::bad_version: return "bad_version";
        case RejectReason::bad_json: return "bad_json";
        case RejectReason::oversized: return "oversized";
        case RejectReason::unknown_kind: return "unknown_kind";
        case RejectReason::unknown_observation_type: return "unknown_observation_type";
        case RejectReason::unknown_source_type: return "unknown_source_type";
        case RejectReason::invalid_value: return "invalid_value";
        case RejectReason::malformed_timestamp: return "malformed_timestamp";
        case RejectReason::stale_timestamp: return "stale_timestamp";
        case RejectReason::replayed_sequence: return "replayed_sequence";
        case RejectReason::duplicate_observation: return "duplicate_observation";
        case RejectReason::device_unknown: return "device_unknown";
        case RejectReason::device_revoked: return "device_revoked";
        case RejectReason::device_unpaired: return "device_unpaired";
        case RejectReason::identity_mismatch: return "identity_mismatch";
        case RejectReason::consent_missing: return "consent_missing";
        case RejectReason::consent_withdrawn: return "consent_withdrawn";
        case RejectReason::capture_disabled: return "capture_disabled";
        case RejectReason::queue_full: return "queue_full";
        case RejectReason::unauthorized: return "unauthorized";
        case RejectReason::processing_failed: return "processing_failed";
    }
    return "unauthorized";
}

std::optional<std::vector<uint8_t>> frame(const nlohmann::json& payload) {
    const std::string body = payload.dump();
    if (body.size() > kMaxFrameBytes) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(body.size() + 4);
    const uint32_t length = static_cast<uint32_t>(body.size());
    out.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(length & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool frame_length_acceptable(const uint8_t* header, size_t header_len, size_t* out_length) {
    if (header == nullptr || header_len < 4) return false;
    const size_t length = (static_cast<size_t>(header[0]) << 24) |
                          (static_cast<size_t>(header[1]) << 16) |
                          (static_cast<size_t>(header[2]) << 8) |
                          static_cast<size_t>(header[3]);
    if (length == 0 || length > kMaxFrameBytes) return false;
    if (out_length != nullptr) *out_length = length;
    return true;
}

nlohmann::json encode(const DeviceMessage& message) {
    nlohmann::json out = nlohmann::json::object();
    out["protocol_version_major"] = message.protocol_version_major;
    out["protocol_version_minor"] = message.protocol_version_minor;
    out["kind"] = message_kind_to_string(message.kind);

    switch (message.kind) {
        case MessageKind::hello:
            out["device_id"] = message.device_id_claim;
            if (!message.display_name_claim.empty()) out["display_name"] = message.display_name_claim;
            out["capabilities"] = message.capabilities;
            break;
        case MessageKind::welcome:
            out["device_id"] = message.device_id_claim;  // the identity the hub derived
            // Where this device must resume. Non-sensitive (it is the device's own
            // counter), and the only correct way for a restarted app to rejoin without
            // either replaying or being locked out.
            out["last_sequence"] = message.resume_after_sequence;
            break;
        case MessageKind::observation:
            out["observation_id"] = message.observation_id;
            out["device_id"] = message.device_id_claim;
            out["source_type"] = message.source_type;
            out["observation_type"] = message.observation_type;
            out["acquisition_mode"] = message.acquisition_mode;
            out["acquired_at_ms"] = message.acquired_at_ms;
            out["sequence"] = message.sequence;
            out["features"] = message.features;
            if (!message.unit.empty()) out["unit"] = message.unit;
            if (message.scale.has_value()) out["scale"] = message.scale.value();
            if (message.confidence.has_value()) out["confidence"] = message.confidence.value();
            if (message.quality.has_value()) out["quality"] = message.quality.value();
            out["provenance_claim"] = message.provenance_claim.empty()
                                          ? std::string("synthetic")
                                          : message.provenance_claim;
            break;
        case MessageKind::acknowledgement:
            out["observation_id"] = message.observation_id;
            out["sequence"] = message.sequence;
            out["accepted"] = message.reason == RejectReason::none;
            out["reason"] = reject_reason_to_string(message.reason);
            break;
        case MessageKind::heartbeat:
            out["device_id"] = message.device_id_claim;
            out["sequence"] = message.sequence;
            break;
        case MessageKind::goodbye:
        case MessageKind::error:
            out["reason"] = reject_reason_to_string(message.reason);
            break;
    }
    if (!message.detail.empty()) out["detail"] = bounded_detail(message.detail);
    return out;
}

std::optional<RejectReason> reject_reason_from_string(const std::string& value) {
    static const std::pair<const char*, RejectReason> table[] = {
        {"ok", RejectReason::none},
        {"bad_frame", RejectReason::bad_frame},
        {"bad_version", RejectReason::bad_version},
        {"bad_json", RejectReason::bad_json},
        {"oversized", RejectReason::oversized},
        {"unknown_kind", RejectReason::unknown_kind},
        {"unknown_observation_type", RejectReason::unknown_observation_type},
        {"unknown_source_type", RejectReason::unknown_source_type},
        {"invalid_value", RejectReason::invalid_value},
        {"malformed_timestamp", RejectReason::malformed_timestamp},
        {"stale_timestamp", RejectReason::stale_timestamp},
        {"replayed_sequence", RejectReason::replayed_sequence},
        {"duplicate_observation", RejectReason::duplicate_observation},
        {"device_unknown", RejectReason::device_unknown},
        {"device_revoked", RejectReason::device_revoked},
        {"device_unpaired", RejectReason::device_unpaired},
        {"identity_mismatch", RejectReason::identity_mismatch},
        {"consent_missing", RejectReason::consent_missing},
        {"consent_withdrawn", RejectReason::consent_withdrawn},
        {"capture_disabled", RejectReason::capture_disabled},
        {"queue_full", RejectReason::queue_full},
        {"unauthorized", RejectReason::unauthorized},
        {"processing_failed", RejectReason::processing_failed},
    };
    for (const auto& entry : table) {
        if (value == entry.first) return entry.second;
    }
    return std::nullopt;
}

std::optional<DeviceMessage> decode(const nlohmann::json& payload, RejectReason* reason,
                                    std::string* detail) {
    auto fail = [reason, detail](RejectReason r, const std::string& why) {
        if (reason != nullptr) *reason = r;
        if (detail != nullptr) *detail = bounded_detail(why);
        return std::optional<DeviceMessage>{};
    };

    if (!payload.is_object()) return fail(RejectReason::bad_json, "payload is not an object");
    if (payload.dump().size() > kMaxFrameBytes) return fail(RejectReason::oversized, "frame too large");

    if (!payload.contains("kind") || !payload["kind"].is_string()) {
        return fail(RejectReason::bad_json, "kind missing or not a string");
    }
    const auto kind = message_kind_from_string(payload["kind"].get<std::string>());
    if (!kind.has_value()) return fail(RejectReason::unknown_kind, "unknown kind");

    DeviceMessage message;
    message.kind = kind.value();

    if (!payload.contains("protocol_version_major") ||
        !payload["protocol_version_major"].is_number_integer()) {
        return fail(RejectReason::bad_json, "protocol_version_major missing or not an integer");
    }
    message.protocol_version_major = payload["protocol_version_major"].get<int>();
    message.protocol_version_minor =
        payload.contains("protocol_version_minor") && payload["protocol_version_minor"].is_number_integer()
            ? payload["protocol_version_minor"].get<int>()
            : 0;
    if (message.protocol_version_major != kProtocolVersionMajor) {
        return fail(RejectReason::bad_version, "unsupported protocol version");
    }

    if (kind.value() == MessageKind::hello) {
        if (!payload.contains("device_id") || !bounded_string(payload["device_id"], 96)) {
            return fail(RejectReason::bad_json, "device_id missing, not a string, or too long");
        }
        message.device_id_claim = payload["device_id"].get<std::string>();
        if (payload.contains("display_name")) {
            if (!bounded_string(payload["display_name"], 64)) {
                return fail(RejectReason::bad_json, "display_name invalid");
            }
            message.display_name_claim = payload["display_name"].get<std::string>();
        }
        if (payload.contains("capabilities")) {
            if (!payload["capabilities"].is_array() || payload["capabilities"].size() > 16) {
                return fail(RejectReason::bad_json, "capabilities must be a bounded array");
            }
            for (const auto& cap : payload["capabilities"]) {
                if (!bounded_string(cap, 32)) return fail(RejectReason::bad_json, "capability invalid");
                message.capabilities.push_back(cap.get<std::string>());
            }
        }
        if (reason != nullptr) *reason = RejectReason::none;
        return message;
    }

    if (kind.value() == MessageKind::heartbeat) {
        if (!payload.contains("device_id") || !bounded_string(payload["device_id"], 96)) {
            return fail(RejectReason::bad_json, "device_id invalid");
        }
        message.device_id_claim = payload["device_id"].get<std::string>();
        uint64_t heartbeat_sequence = 0;
        if (!read_sequence(payload, &heartbeat_sequence, reason, detail)) {
            return std::optional<DeviceMessage>{};
        }
        message.sequence = heartbeat_sequence;
        if (reason != nullptr) *reason = RejectReason::none;
        return message;
    }

    // welcome / ack / error: these carry RESPONSE fields the hub sends, and a
    // decoder that ignored them could not tell an accepted observation from a
    // refused one - it reported "ok" for every rejection. The fields are optional
    // (a peer may omit them) but never silently dropped.
    if (kind.value() == MessageKind::welcome || kind.value() == MessageKind::acknowledgement ||
        kind.value() == MessageKind::error) {
        if (payload.contains("device_id") && bounded_string(payload["device_id"], 96)) {
            message.device_id_claim = payload["device_id"].get<std::string>();
        }
        if (payload.contains("observation_id") && bounded_string(payload["observation_id"], 96)) {
            message.observation_id = payload["observation_id"].get<std::string>();
        }
        if (payload.contains("last_sequence") && payload["last_sequence"].is_number_unsigned()) {
            message.resume_after_sequence = payload["last_sequence"].get<uint64_t>();
        }
        if (payload.contains("reason") && payload["reason"].is_string()) {
            const auto parsed = reject_reason_from_string(payload["reason"].get<std::string>());
            if (parsed.has_value()) message.reason = parsed.value();
        }
        if (payload.contains("detail") && payload["detail"].is_string() && detail != nullptr) {
            *detail = bounded_detail(payload["detail"].get<std::string>());
        }
        if (reason != nullptr) *reason = RejectReason::none;
        return message;
    }

    if (kind.value() != MessageKind::observation) {
        if (reason != nullptr) *reason = RejectReason::none;
        return message;  // other kinds carry no required payload
    }

    // ---- observation ----
    if (!payload.contains("observation_id") || !bounded_string(payload["observation_id"], 96)) {
        return fail(RejectReason::bad_json, "observation_id invalid");
    }
    message.observation_id = payload["observation_id"].get<std::string>();
    if (!payload.contains("device_id") || !bounded_string(payload["device_id"], 96)) {
        return fail(RejectReason::bad_json, "device_id invalid");
    }
    message.device_id_claim = payload["device_id"].get<std::string>();
    if (!payload.contains("source_type") || !bounded_string(payload["source_type"], 32)) {
        return fail(RejectReason::bad_json, "source_type invalid");
    }
    message.source_type = payload["source_type"].get<std::string>();
    if (!payload.contains("observation_type") || !bounded_string(payload["observation_type"], 64)) {
        return fail(RejectReason::bad_json, "observation_type invalid");
    }
    message.observation_type = payload["observation_type"].get<std::string>();
    if (!payload.contains("acquisition_mode") || !bounded_string(payload["acquisition_mode"], 16)) {
        return fail(RejectReason::bad_json, "acquisition_mode invalid");
    }
    message.acquisition_mode = payload["acquisition_mode"].get<std::string>();
    if (message.acquisition_mode != "live" && message.acquisition_mode != "synthetic") {
        return fail(RejectReason::unknown_kind, "unknown acquisition_mode");
    }
    if (!payload.contains("acquired_at_ms") || !payload["acquired_at_ms"].is_number_integer()) {
        return fail(RejectReason::malformed_timestamp, "acquired_at_ms missing or not an integer");
    }
    message.acquired_at_ms = payload["acquired_at_ms"].get<int64_t>();
    if (message.acquired_at_ms <= 0) {
        return fail(RejectReason::malformed_timestamp, "acquired_at_ms must be positive");
    }
    uint64_t sequence = 0;
    if (!read_sequence(payload, &sequence, reason, detail)) return std::optional<DeviceMessage>{};
    message.sequence = sequence;
    if (!payload.contains("features") || !features_ok(payload["features"])) {
        return fail(RejectReason::invalid_value, "features invalid, too large, or non-finite");
    }
    message.features = payload["features"];

    if (payload.contains("unit")) {
        if (!bounded_string(payload["unit"], 16)) return fail(RejectReason::bad_json, "unit invalid");
        message.unit = payload["unit"].get<std::string>();
    }
    RejectReason unit_reason = RejectReason::none;
    message.scale = optional_unit_double(payload, "scale", &unit_reason);
    if (unit_reason == RejectReason::bad_json) return fail(unit_reason, "scale not a number");
    if (unit_reason == RejectReason::invalid_value) return fail(unit_reason, "scale out of range");
    unit_reason = RejectReason::none;
    message.confidence = optional_unit_double(payload, "confidence", &unit_reason);
    if (unit_reason != RejectReason::none) return fail(unit_reason, "confidence must be in [0,1]");
    unit_reason = RejectReason::none;
    message.quality = optional_unit_double(payload, "quality", &unit_reason);
    if (unit_reason != RejectReason::none) return fail(unit_reason, "quality must be in [0,1]");

    message.provenance_claim = payload.value("provenance_claim", std::string("synthetic"));
    if (message.provenance_claim != "real" && message.provenance_claim != "synthetic") {
        return fail(RejectReason::bad_json, "unknown provenance_claim");
    }

    if (reason != nullptr) *reason = RejectReason::none;
    return message;
}

ProvenanceDecision derive_provenance(const PeerContext& peer, const DeviceMessage& message) {
    ProvenanceDecision decision;
    const bool claimed_real = message.provenance_claim == "real";
    const bool live = message.acquisition_mode == "live";

    if (!peer.peer_authenticated) {
        decision.reason = "peer is not authenticated";
    } else if (!peer.device_paired) {
        decision.reason = "device is not paired";
    } else if (!peer.device_consent_granted) {
        decision.reason = "device consent is not recorded";
    } else if (!peer.capture_enabled) {
        decision.reason = "capture is not enabled for this device";
    } else if (!live) {
        decision.reason = "acquisition mode is not live";
    } else {
        decision.real = true;
        decision.reason = "authenticated, paired, consented, capture enabled, live acquisition";
    }

    // A `real` claim that the hub cannot substantiate is never silently believed:
    // the record is stored as synthetic and flagged for audit.
    decision.claim_unsubstantiated = claimed_real && !decision.real;
    if (decision.claim_unsubstantiated) {
        decision.reason += "; claimed real, recorded synthetic (claim not substantiated)";
    }
    return decision;
}

}  // namespace homeguardian::net
