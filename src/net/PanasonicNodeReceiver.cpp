#include "net/PanasonicNodeReceiver.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>

namespace homeguardian {
namespace net {
namespace {

using nlohmann::json;

bool too_long(const std::string& s) { return s.size() > kMaxFeatureStringChars; }

// One place decides what "real" and "synthetic" mean, so no caller has to guess at a spelling.
bool claim_is_real(const std::string& c) { return c == "real" || c == "real_device"; }
bool claim_is_synthetic(const std::string& c) { return c == "synthetic"; }

int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

NodeObservationLine parse_node_observation_line(const std::string& line) {
    NodeObservationLine out;
    if (line.empty()) {
        out.reason = "empty line";
        return out;
    }
    if (line.size() > kMaxFrameBytes) {
        out.reason = "line exceeds the transport's frame bound";
        return out;
    }

    json j;
    try {
        j = json::parse(line);
    } catch (const std::exception&) {
        out.reason = "not valid JSON";
        return out;
    }
    if (!j.is_object()) {
        out.reason = "not a JSON object";
        return out;
    }

    const auto get_string = [&j](const char* key, std::string* dst) -> bool {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string()) return false;
        *dst = it->get<std::string>();
        return true;
    };

    if (!get_string("source_type", &out.source_type) || out.source_type.empty()) {
        out.reason = "source_type is missing or not a non-empty string";
        return out;
    }
    if (!get_string("observation_type", &out.observation_type) || out.observation_type.empty()) {
        out.reason = "observation_type is missing or not a non-empty string";
        return out;
    }

    const auto value = j.find("value");
    if (value == j.end() || !value->is_number()) {
        out.reason = "value is missing or not a number";
        return out;
    }
    out.value = value->get<double>();
    if (!std::isfinite(out.value)) {
        // A NaN/inf measurement is not a measurement; the producer silently drops these, and so do we - but
        // as a NAMED refusal rather than a default.
        out.reason = "value is not finite";
        return out;
    }

    if (const auto it = j.find("unit"); it != j.end() && it->is_string()) out.unit = it->get<std::string>();
    if (const auto it = j.find("state"); it != j.end() && it->is_string()) out.state = it->get<std::string>();
    if (const auto it = j.find("confidence"); it != j.end() && it->is_number()) {
        out.confidence = it->get<double>();
        out.has_confidence = std::isfinite(out.confidence);
        if (!out.has_confidence) out.confidence = 0.0;
    }
    if (const auto it = j.find("acquired_at_ms"); it != j.end() && it->is_number()) {
        out.acquired_at_ms = static_cast<int64_t>(it->get<double>());
        out.has_acquired_at = true;
    }
    if (const auto it = j.find("provenance"); it != j.end() && it->is_string()) {
        out.provenance_claim = it->get<std::string>();
        out.has_provenance = true;
    }
    if (const auto it = j.find("schema_version"); it != j.end()) {
        if (!it->is_number_integer()) {
            out.reason = "schema_version is present but not an integer";
            return out;
        }
        out.schema_version = it->get<int>();
    } else {
        out.schema_version = kSchemaVersionAbsent;
    }

    if (too_long(out.source_type) || too_long(out.observation_type) || too_long(out.unit) || too_long(out.state) ||
        too_long(out.provenance_claim)) {
        out.reason = "a string field exceeds the documented bound";
        return out;
    }

    out.valid = true;
    return out;
}

PanasonicNodeReceiver::PanasonicNodeReceiver(ObservationIngest& ingest)
    : PanasonicNodeReceiver(ingest, Config{}) {}

PanasonicNodeReceiver::PanasonicNodeReceiver(ObservationIngest& ingest, Config cfg)
    : ingest_(ingest), cfg_(cfg) {
    if (!cfg_.clock) cfg_.clock = system_now_ms;
}

PanasonicNodeReceiver::Result PanasonicNodeReceiver::accept_line(const PeerContext& peer,
                                                                const std::string& line,
                                                                const ObservationIngest::Sink& sink) {
    Result out;

    if (line.size() > cfg_.max_line_bytes) {
        out.reason = "oversized";
        ++refused_;
        return out;
    }

    const NodeObservationLine parsed = parse_node_observation_line(line);
    if (!parsed.valid) {
        out.reason = "malformed_payload";
        out.detail = parsed.reason;
        ++refused_;
        return out;
    }

    // Schema handling. The current producer sends NO version field, so that case is counted and accepted as
    // the documented shape; any version this receiver does not implement is refused rather than guessed at.
    if (parsed.schema_version == kSchemaVersionAbsent) {
        // Counted, never ASSUMED: an absent version is not silently read as the version this receiver
        // implements. Under strict mode it is refused outright.
        ++schema_absent_;
        if (cfg_.require_schema_version) {
            out.reason = "missing_schema_version";
            out.detail = "strict mode: the producer must send schema_version on every line";
            ++refused_;
            return out;
        }
    } else if (parsed.schema_version != kNodeSchemaVersion) {
        out.reason = "unsupported_schema_version";
        out.detail = "this receiver implements schema version " + std::to_string(kNodeSchemaVersion);
        ++refused_;
        return out;
    }

    // Closed vocabulary, borrowed from the hub's own decoder - not a second list that could drift from it.
    if (!security_source_type_from_string(parsed.source_type)) {
        out.reason = "unknown_source_type";
        ++refused_;
        return out;
    }

    // ---- provenance: narrow only, never widen; absence is never real ----
    std::string effective;
    if (parsed.has_provenance) {
        if (!claim_is_real(parsed.provenance_claim) && !claim_is_synthetic(parsed.provenance_claim)) {
            out.reason = "unknown_provenance_claim";
            out.detail = "claims must be 'real'/'real_device' or 'synthetic'";
            ++refused_;
            return out;
        }
        if (claim_is_real(parsed.provenance_claim) && cfg_.session_is_synthetic) {
            // Refused rather than quietly corrected: a producer claiming physical provenance for a loopback
            // session is a defect worth surfacing, and silently downgrading it would hide that.
            out.reason = "provenance_claim_widens_session";
            out.detail = "a real-device claim cannot originate from a synthetic session";
            ++refused_;
            ++provenance_refused_;
            return out;
        }
        effective = claim_is_synthetic(parsed.provenance_claim) ? "synthetic" : "real";
        if (claim_is_synthetic(parsed.provenance_claim) && !cfg_.session_is_synthetic) ++narrowed_;
    } else {
        // The producer's own default would make this "real_device". Here it stays what the session proves.
        effective = cfg_.session_is_synthetic ? "synthetic" : "real";
    }

    // ---- shape it into the wire type the existing ingest consumes ----
    DeviceMessage message;
    message.protocol_version_major = cfg_.protocol_version_major;
    message.protocol_version_minor = cfg_.protocol_version_minor;
    message.kind = MessageKind::observation;
    // device_id_claim is deliberately EMPTY: identity comes from the authenticated session, so a producer
    // cannot name itself into another device's identity or consent.
    message.observation_id = peer.authenticated_device_id + "-" + std::to_string(next_observation_++);
    message.source_type = parsed.source_type;
    message.observation_type = parsed.observation_type;
    message.acquisition_mode = cfg_.session_is_synthetic ? "synthetic" : "live";
    message.acquired_at_ms = parsed.has_acquired_at ? parsed.acquired_at_ms : cfg_.clock();
    message.sequence = next_sequence_++;
    message.features = nlohmann::json::object();
    message.features[parsed.observation_type] = parsed.value;
    message.unit = parsed.unit;
    if (parsed.has_confidence) message.confidence = parsed.confidence;
    message.provenance_claim = effective;
    if (!parsed.state.empty()) {
        // The producer sends `state`, and the hub's wire type has no field for it. Rather than invent a
        // semantic mapping (a fabricated number is worse than a missing one), it is counted and reported as
        // an interface gap for the contract to settle.
        ++state_unmapped_;
    }

    // ---- every identity/consent/revocation/replay/skew/queue decision is the ingest's ----
    const ObservationIngest::Result result = ingest_.accept(peer, message, sink);
    if (!result.accepted) {
        // `reason` is a RejectReason enum: its stable string form is the vocabulary the hub already sends on
        // the wire, so the adapter converts instead of inventing a parallel set of words.
        out.reason = reject_reason_to_string(result.reason);
        out.detail = result.detail;
        if (result.provenance_claim_unsubstantiated) ++provenance_refused_;
        ++refused_;
        return out;
    }
    // The ingest RECORDS the provenance it accepted; the adapter reports that back so a caller cannot mistake
    // a claim for a fact.
    out.detail = result.real ? "recorded_provenance=real" : "recorded_provenance=synthetic";
    out.accepted = true;
    out.reason = "accepted";
    ++accepted_;
    return out;
}

}  // namespace net
}  // namespace homeguardian
