#pragma once

// ControlChannel — the ONLY thing the coordination hub (Oracle) is allowed to carry
// (milestone HG-NET-02C / Issue #16).
//
// THE CONSTRAINT THIS FILE MAKES ENFORCEABLE
//   "Oracle must not carry continuous CCTV video." A byte cap and a closed message
//   vocabulary turn that from a promise into a property of the code: there is no message
//   kind that can describe a video or audio frame, and any payload over the cap is
//   refused rather than truncated. Registration, heartbeat, authorization, consent,
//   revocation, signalling and non-media telemetry are the whole vocabulary; media
//   travels on a direct, independently authenticated path (see DirectMediaPlanner).
//
// WHY A CLOSED VOCABULARY RATHER THAN A FLAG
//   A `is_media=false` boolean can be set wrongly. A kind enum with no media member means
//   a media path over the hub is not expressible, which is the same "unrepresentable
//   rather than discouraged" rule the rest of this boundary uses.

#include "CameraTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian::camera {

enum class ControlKind {
    connector_register,      // "I am this connector, at this site"
    heartbeat,               // liveness + observed capabilities
    authorization_request,   // ask the control plane to evaluate a device request
    authorization_result,    // the answer (allowed / a named denial)
    consent_change,          // a consent grant or withdrawal took effect
    revocation,              // a device was revoked
    ice_candidate,           // signalling for a DIRECT media path
    stream_control,          // start/stop a stream (control, not content)
    session_terminated,      // a session ended, and why
    telemetry                // bounded, non-media counters (never A/V)
};

inline const char* control_kind_to_string(ControlKind k) {
    switch (k) {
        case ControlKind::connector_register:    return "connector_register";
        case ControlKind::heartbeat:             return "heartbeat";
        case ControlKind::authorization_request: return "authorization_request";
        case ControlKind::authorization_result:  return "authorization_result";
        case ControlKind::consent_change:        return "consent_change";
        case ControlKind::revocation:            return "revocation";
        case ControlKind::ice_candidate:         return "ice_candidate";
        case ControlKind::stream_control:        return "stream_control";
        case ControlKind::session_terminated:    return "session_terminated";
        case ControlKind::telemetry:             return "telemetry";
    }
    return "unknown";
}

struct ControlMessage {
    ControlKind kind = ControlKind::heartbeat;
    LogicalDeviceId device{};
    uint64_t seq = 0;
    uint64_t at_ms = 0;
    std::string detail{};      // bounded, non-sensitive; never media and never a credential

    static constexpr size_t kMaxDetailChars = 192;
};

// A wire form with a hard byte ceiling. The ceiling is what makes "no continuous media"
// structural: a frame cannot fit, so a media path over this channel is impossible rather
// than merely forbidden.
class ControlChannel {
public:
    // Maximum encoded message size. Comparable to a WireGuard keepalive plus a small JSON
    // body; orders of magnitude below a single video frame, and the point is that it is a
    // hard refusal, not a soft suggestion.
    static constexpr size_t kMaxEncodedBytes = 1024;
    // A documented reference figure, so the ceiling is not arbitrary: one 1080p H.265
    // keyframe is tens of kilobytes, i.e. this channel cannot express even one frame.
    static constexpr size_t kReferenceFrameBytes = 32768;

    explicit ControlChannel(size_t max_bytes = kMaxEncodedBytes) : max_bytes_(max_bytes) {}

    size_t max_bytes() const { return max_bytes_; }

    // Encode for the hub. Returns empty when the message must not be sent:
    //   - the detail is over kMaxDetailChars (a long payload is a media path in disguise)
    //   - the encoded form would exceed max_bytes()
    //   - the device identity is invalid
    // The caller must treat an empty result as a REFUSAL, never as an empty message.
    std::string encode(const ControlMessage& message) const;

    // Decode a hub message. Returns false on anything malformed, oversized, or carrying an
    // unknown kind - a control-plane message we cannot classify must not be acted on.
    bool decode(const std::string& wire, ControlMessage* out, std::string* reason) const;

    // Refuse a payload that is not a control message at all. Named so a caller cannot
    // confuse "refused" with "empty".
    bool reject_bulk(const std::string& payload, std::string* reason) const;

    // Optional wire log: every message this channel ACCEPTS is appended here. Useful for a
    // control-plane pilot's evidence and for tests. It cannot leak media, because nothing
    // media-shaped can be encoded in the first place.
    void set_capture(std::vector<std::string>* sink) { capture_ = sink; }

    uint64_t accepted() const { return accepted_; }
    uint64_t refused() const { return refused_; }
    uint64_t refused_bulk() const { return refused_bulk_; }

private:
    size_t max_bytes_;
    // A wire log is named explicitly, so it is a deliberate side effect rather than a
    // hidden one.
    std::vector<std::string>* capture_ = nullptr;
    // Instrumentation counters are mutable so the conversions below stay `const`: encoding
    // and decoding do not change what this channel IS, they only record what happened.
    mutable uint64_t accepted_ = 0;
    mutable uint64_t refused_ = 0;
    mutable uint64_t refused_bulk_ = 0;
};

// Parse a kind from its wire name. Returns false for an unknown name.
bool control_kind_from_string(const std::string& name, ControlKind* out);

}  // namespace homeguardian::camera
