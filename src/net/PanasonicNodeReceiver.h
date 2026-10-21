#pragma once

// PanasonicNodeReceiver - accepts the node's observation lines and hands them to the EXISTING ingest.
//
// WHY THIS EXISTS
//   The Panasonic producer (Android harness: NodeSession.pump -> ObservationTransport) emits one NDJSON
//   observation line per sensor reading, while the hub's authenticated ingest consumes a framed `DeviceMessage`.
//   Two correct halves with no adapter between them is exactly the gap this file closes. It is an ADAPTER,
//   not a second intelligence engine: identity, consent, revocation, replay, clock skew and the bounded queue
//   are all enforced by `ObservationIngest`, which this class delegates to and never re-implements.
//
// WHAT IT ADDS THAT THE INGEST CANNOT KNOW
//   1. The producer's LINE SHAPE: strict parsing of the documented fields, with bounded lengths.
//   2. SCHEMA HANDLING: the current producer sends no version field at all, so that case is named rather
//      than silently assumed, and any other version is refused instead of guessed at.
//   3. PROVENANCE DERIVATION - the security-relevant one, see the rule below.
//
// THE PROVENANCE RULE (narrow only, never widen)
//   The producer DEFAULTS its `provenance` field to "real_device" when the field is absent, so a missing
//   field becomes a real-device claim at the source. This receiver therefore treats the SESSION as the
//   authority on where an observation came from: a loopback/test session cannot be widened to "real" by
//   anything a producer says, while a claim may be narrowed (real -> synthetic) and an ABSENT claim is
//   never read as real. A `real_device` claim on a synthetic session is refused outright rather than
//   downgraded, because a producer that claims physical provenance for a loopback input is a defect worth
//   surfacing, not a value to quietly correct.
//
// THE IDENTITY RULE
//   `device_id_claim` is deliberately left EMPTY. The hub re-derives identity from the authenticated session
//   (`PeerContext`), so a producer cannot name itself into someone else's identity - the same rule the
//   existing transport applies to every other device class.
//
// MEDIA: observations only. This type carries bounded scalars. It has no path by which a frame, an audio
// sample, a transcript or a biometric template could be transported, stored or forwarded.

#include "core/SecurityObservation.h"
#include "net/DeviceProtocol.h"
#include "net/ObservationIngest.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace homeguardian {
namespace net {

// The documented producer line. `has_provenance` distinguishes an ABSENT field from an explicit claim,
// which is the whole point of the provenance rule above.
struct NodeObservationLine {
    bool valid = false;
    std::string reason;

    std::string source_type;
    std::string observation_type;
    std::string unit;
    std::string state;              // the producer sends it; the ingest has no matching field (see below)
    std::string provenance_claim;   // "" when absent
    bool has_provenance = false;

    double value = 0.0;
    double confidence = 0.0;
    bool has_confidence = false;
    int64_t acquired_at_ms = 0;
    bool has_acquired_at = false;
    int schema_version = 0;         // 0 = the field is ABSENT in the producer's current output
};

inline constexpr int kNodeSchemaVersion = 1;
inline constexpr int kSchemaVersionAbsent = 0;

// Strict parse of ONE line. Refuses anything it cannot fully understand rather than filling a default in:
// a partially-understood observation is worse than a refused one, because it looks like data.
NodeObservationLine parse_node_observation_line(const std::string& line);

class PanasonicNodeReceiver {
public:
    struct Config {
        // What the SESSION can prove. Fail-closed default: a loopback/development session is synthetic, so an
        // unqualified producer cannot promote its output to physical evidence by claiming it.
        bool session_is_synthetic = true;
        size_t max_line_bytes = kMaxFrameBytes;   // the transport's frame bound, reused rather than reinvented
        uint16_t protocol_version_major = kProtocolVersionMajor;
        uint16_t protocol_version_minor = kProtocolVersionMinor;
        bool require_schema_version = false;     // true enforces finalized contract (schema_version == 1 mandatory)
        // Supplied by the caller so a test can drive time; defaults to the system clock in the constructor.
        std::function<int64_t()> clock;
    };

    struct Result {
        bool accepted = false;
        std::string reason;   // stable, non-sensitive: mirrors the ingest's vocabulary where it delegates
        std::string detail;   // bounded, non-sensitive
    };

    explicit PanasonicNodeReceiver(ObservationIngest& ingest);
    PanasonicNodeReceiver(ObservationIngest& ingest, Config cfg);

    // Accept one producer line for an already-authenticated peer. Every identity/consent/replay/skew/bound
    // decision is the ingest's; this method only shapes the line into the wire type it expects.
    Result accept_line(const PeerContext& peer, const std::string& line, const ObservationIngest::Sink& sink);

    size_t lines_accepted() const { return accepted_; }
    size_t lines_refused() const { return refused_; }
    size_t schema_version_absent() const { return schema_absent_; }
    size_t provenance_narrowed() const { return narrowed_; }
    size_t provenance_refused() const { return provenance_refused_; }
    size_t state_unmapped() const { return state_unmapped_; }

private:
    ObservationIngest& ingest_;
    Config cfg_;
    size_t accepted_ = 0;
    size_t refused_ = 0;
    size_t schema_absent_ = 0;
    size_t narrowed_ = 0;
    size_t provenance_refused_ = 0;
    size_t state_unmapped_ = 0;
    uint64_t next_observation_ = 1;
    uint64_t next_sequence_ = 1;
};

}  // namespace net
}  // namespace homeguardian
