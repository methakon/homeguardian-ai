#pragma once

// The versioned device-to-hub observation protocol (milestone D-03).
//
// WHY THIS EXISTS: D-02 gave a phone the ability to acquire and account for
// authorized media, and D-02x gave the hub a validated observation vocabulary.
// Neither travels. This file defines the WIRE FORM that carries a compact
// observation from an authenticated Android node to the hub, and nothing else:
//   * no raw media (streaming is explicitly out of scope for D-03),
//   * no commands, no credentials in payloads, no free-form text,
//   * strict size limits and an explicitly enumerated message kind.
//
// It is deliberately transport-agnostic and OpenSSL-free so the whole protocol,
// registry and accept path can be unit-tested on a host with no TLS toolchain;
// the TLS listener lives in TlsTransport.* and is compiled only when OpenSSL is
// available.
//
// SECURITY MODEL (enforced here and in ObservationIngest):
//   * The hub derives the device identity from the AUTHENTICATED TLS peer
//     certificate, never from the payload. A `device_id` in a message is a
//     CLAIM, is checked against the authenticated identity, and a mismatch is
//     rejected (`identity_mismatch`).
//   * A claimed `real` provenance is NOT trusted. The hub derives provenance from
//     (authenticated peer) x (device paired) x (device consent) x (declared
//     acquisition mode), and records an unsubstantiated claim.
//   * Frames are length-prefixed with a hard maximum, so a peer cannot make the
//     hub allocate unbounded memory.
//
// FRAMING: uint32 big-endian payload length, then that many bytes of JSON.
// Every message is an object with a `kind` and a `protocol_version`.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace homeguardian::net {

// Protocol version this build speaks. A peer with a different MAJOR is refused;
// the negotiated version is echoed in the welcome.
inline constexpr int kProtocolVersionMajor = 1;
inline constexpr int kProtocolVersionMinor = 0;

// Hard frame cap. An observation is a compact numeric record; 4 KiB is generous
// and keeps a malicious peer from making the hub allocate.
inline constexpr size_t kMaxFrameBytes = 4096;
// Bound on the JSON object we will even parse a frame into.
inline constexpr size_t kMaxFeatureKeys = 24;
inline constexpr size_t kMaxFeatureStringChars = 64;
// Sequence numbers are unsigned and strictly increasing per device.
inline constexpr uint64_t kMaxSequence = (1ULL << 60);
// Acceptable clock skew for an acquisition timestamp.
inline constexpr int64_t kMaxFutureSkewMs = 120000;      // 2 minutes ahead
inline constexpr int64_t kMaxObservationAgeMs = 86400000;  // 24 hours old
// How far behind the device's own high-water mark an observation may be.
inline constexpr int64_t kMaxOutOfOrderMs = 60000;  // 60 seconds

enum class MessageKind { hello, welcome, observation, acknowledgement, heartbeat, goodbye, error };

const char* message_kind_to_string(MessageKind kind);
std::optional<MessageKind> message_kind_from_string(const std::string& value);

// Why a message was refused. Stable strings: they appear in acks, logs and tests.
enum class RejectReason {
    none,
    bad_frame,            // framing/parsing failure
    bad_version,          // unsupported protocol version
    bad_json,             // not an object, or a required field has the wrong type
    oversized,            // frame or payload beyond the limit
    unknown_kind,         // kind not in the enumerated set
    unknown_observation_type,
    unknown_source_type,
    invalid_value,        // NaN/inf, out of range, malformed feature
    malformed_timestamp,  // zero, in the future, or absurdly old
    stale_timestamp,      // behind the device's own out-of-order window
    replayed_sequence,    // sequence <= last accepted (duplicate or replay)
    duplicate_observation,
    device_unknown,       // not in the registry
    device_revoked,
    device_unpaired,
    identity_mismatch,    // claimed device id != authenticated peer identity
    consent_missing,      // no device consent recorded
    consent_withdrawn,
    capture_disabled,     // the operator has not enabled acquisition on the device
    queue_full,           // bounded ingest queue rejected the work
    unauthorized
};

const char* reject_reason_to_string(RejectReason reason);

// The inverse of reject_reason_to_string. Needed because the hub SENDS a reason on
// every acknowledgement and error frame: a decoder that cannot read it back leaves
// a device unable to distinguish "accepted" from "refused, here is why".
std::optional<RejectReason> reject_reason_from_string(const std::string& value);

// A wire message. Only the fields relevant to its kind are populated.
struct DeviceMessage {
    int protocol_version_major = 0;
    int protocol_version_minor = 0;
    MessageKind kind = MessageKind::error;

    // hello/welcome
    std::string device_id_claim;      // a CLAIM; the hub re-derives identity
    std::string display_name_claim;
    std::vector<std::string> capabilities;  // bounded, e.g. "camera","microphone"

    // observation
    std::string observation_id;
    std::string source_type;          // SecuritySourceType vocabulary
    std::string observation_type;     // the detection-facing feature name
    std::string acquisition_mode;     // "live" | "synthetic"
    int64_t acquired_at_ms = 0;
    uint64_t sequence = 0;
    nlohmann::json features = nlohmann::json::object();
    std::string unit;                 // optional, bounded
    std::optional<double> scale;
    std::optional<double> confidence;
    std::optional<double> quality;
    std::string provenance_claim;     // "real" | "synthetic"

    // ack/error
    RejectReason reason = RejectReason::none;
    std::string detail;               // bounded, non-sensitive
};

// ---- framing ----

// Prepend the 4-byte big-endian length. Returns std::nullopt when the payload
// exceeds kMaxFrameBytes.
std::optional<std::vector<uint8_t>> frame(const nlohmann::json& payload);

// True when `header` (the first 4 bytes) declares a length we would accept.
bool frame_length_acceptable(const uint8_t* header, size_t header_len, size_t* out_length);

// ---- encoding / decoding ----

nlohmann::json encode(const DeviceMessage& message);

// Strict decode. Returns std::nullopt and sets `reason` (+ bounded `detail`) on
// any violation: unknown kind, unsupported version, wrong field type, an
// out-of-range number, an oversized feature set, or a non-finite value.
std::optional<DeviceMessage> decode(const nlohmann::json& payload, RejectReason* reason,
                                    std::string* detail);

// ---- provenance derivation ----

// Everything the accept path knows about the peer and the device.
struct PeerContext {
    std::string authenticated_device_id;  // from the TLS certificate; never the payload
    std::string credential_id;            // certificate fingerprint or CN
    bool peer_authenticated = false;
    bool device_paired = false;
    bool device_consent_granted = false;
    bool capture_enabled = false;
};

struct ProvenanceDecision {
    bool real = false;                     // what the hub will record
    bool claim_unsubstantiated = false;    // the peer claimed real and cannot be
    std::string reason;
};

// The ONLY place provenance is decided. `real` requires: authenticated peer AND
// paired device AND device consent AND capture enabled AND acquisition_mode
// "live". Anything less is recorded as synthetic, and a `real` claim that fails
// is flagged so it can be audited rather than silently believed.
ProvenanceDecision derive_provenance(const PeerContext& peer, const DeviceMessage& message);

}  // namespace homeguardian::net
