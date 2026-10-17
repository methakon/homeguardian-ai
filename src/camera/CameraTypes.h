#pragma once

// Camera-integration vocabulary (milestone HG-V380-09).
//
// WHY THIS EXISTS
//   Two physical V380 cameras are identified on the development LAN and neither can
//   be handed to HomeGuardian as a normal device yet:
//     * camera 1's ONVIF/RTSP interface served live video and device state with NO
//       authentication at all, so it is disabled and must stay disabled;
//     * camera 2 has no RTSP/ONVIF/HTTP interface at all — only the vendor's own
//       ports — and its media and authentication behaviour are UNVERIFIED.
//   So the deliverable at this stage is a BOUNDARY, not a driver: a set of types and
//   seams that make the unsafe outcomes unrepresentable, and that let the registry,
//   consent and rejection rules be exercised today with deterministic mocks while
//   no real camera media is consumed.
//
// THE RULES THIS FILE ENCODES
//   1. Identity is logical. A camera is identified by {site_id, device_id}; an IP
//      address, MAC, SSID or subnet is an ATTRIBUTE that may change at any moment and
//      is never an authorization input. (Both cameras already moved once: one from a
//      NAT'ed 192.168.0.x segment to 192.168.1.20.)
//   2. A mock can never be reported as a real camera. `AdapterClass` is carried
//      through the whole path and the connector derives an Observation's provenance
//      from it, so "the mock tests pass" can never be read as "V380 media works".
//   3. Every refusal has a NAME. A starvation of capabilities, an authorization
//      failure and a transport failure are different facts and must not collapse
//      into a single "failed" boolean.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian::camera {

// ---------------------------------------------------------------- identity

// A stable, operator-assigned identity. Deliberately NOT derived from any address,
// MAC, SSID, model string or serial: camera 1 reports SerialNumber "123456789" (a
// placeholder shared across units) and Model "IPCamera" (generic), so neither could
// serve as one.
struct LogicalDeviceId {
    std::string site_id;     // e.g. "site.home.dhar"
    std::string device_id;   // e.g. "cam.v380.ec39.01"

    // Bounds chosen for storage sanity, not for aesthetics.
    static constexpr size_t kMaxSegmentChars = 64;

    bool valid() const { return valid_segment(site_id) && valid_segment(device_id); }

    // Stable string form used for logs, registry keys and observation `source`.
    std::string canonical() const { return site_id + "/" + device_id; }

    // True when the whole string is a dotted-quad IPv4 literal. Used to REFUSE an
    // address as an identity: both cameras have already changed address once (one
    // moved from a NAT'ed 192.168.0.x segment to 192.168.1.20), so an address in an
    // identity field is a latent identity bug, not a stylistic one.
    static bool is_ipv4_literal(const std::string& s) {
        int groups = 0;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
            if (j == i) return false;                 // empty group
            if (j - i > 3) return false;              // not an octet
            int value = std::stoi(s.substr(i, j - i));
            if (value > 255) return false;
            ++groups;
            i = j;
            if (i < s.size()) {
                if (s[i] != '.') return false;
                ++i;
                if (i == s.size()) return false;      // trailing dot
            }
        }
        return groups == 4;
    }

    static bool valid_segment(const std::string& s) {
        if (s.empty() || s.size() > kMaxSegmentChars) return false;
        if (is_ipv4_literal(s)) return false;         // an address is never an identity
        for (char c : s) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '-' || c == '_';
            if (!ok) return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------- capability

// Bounded capability vocabulary. A declaration is NOT a grant: the connector still
// consults the registry before any frame or control command moves.
enum class Capability {
    video,
    audio_capture,
    audio_output_talkback,
    ptz,
    illumination,
    motion_events
};

inline const char* capability_to_string(Capability c) {
    switch (c) {
        case Capability::video:                 return "video";
        case Capability::audio_capture:         return "audio_capture";
        case Capability::audio_output_talkback: return "audio_output_talkback";
        case Capability::ptz:                   return "ptz";
        case Capability::illumination:          return "illumination";
        case Capability::motion_events:         return "motion_events";
    }
    return "unknown";
}

// A bounded, explicit set. Stored as a vector of the closed enum above so an adapter
// cannot invent a capability string that the rest of the system has never heard of.
struct CapabilitySet {
    std::vector<Capability> items{};
    static constexpr size_t kMaxItems = 6;

    bool has(Capability c) const {
        for (auto i : items) if (i == c) return true;
        return false;
    }
    bool add(Capability c) {
        if (items.size() >= kMaxItems || has(c)) return false;
        items.push_back(c);
        return true;
    }
    std::vector<std::string> to_strings() const {
        std::vector<std::string> out;
        out.reserve(items.size());
        for (auto i : items) out.emplace_back(capability_to_string(i));
        return out;
    }
};

// ---------------------------------------------------------------- adapter class

// What is actually on the other end of this adapter. Carried everywhere because the
// difference between the first three values is the difference between a demo and a
// deployment.
enum class AdapterClass {
    mock,                      // deterministic in-process stand-in; NO device involved
    rtsp_onvif,                // a documented standard interface (not yet reachable here)
    vendor_protocol_unverified // the vendor's own protocol, authentication UNVERIFIED
};

inline const char* adapter_class_to_string(AdapterClass a) {
    switch (a) {
        case AdapterClass::mock:                       return "mock";
        case AdapterClass::rtsp_onvif:                 return "rtsp_onvif";
        case AdapterClass::vendor_protocol_unverified: return "vendor_protocol_unverified";
    }
    return "unknown";
}

// ---------------------------------------------------------------- outcomes

// Why an operation did not succeed. Closed vocabulary: an adapter must classify its
// own failures rather than relaying device text, because a silent V380 close carries
// no diagnosis at all (measured: no banner, 0 bytes back, ~5.0 s idle close, ~0.21 s
// close on an unrecognized payload).
enum class ConnectOutcome {
    ok,
    unreachable,               // no path / no listener
    credentials_rejected,      // the device refused the credential
    stream_absent,             // connected, but no media is being offered
    capability_absent,         // asked for something this device does not declare
    transport_closed,          // closed mid-operation (normal for this family)
    refused_unsafe_interface   // the interface exists but is not safe to use
};

inline const char* connect_outcome_to_string(ConnectOutcome o) {
    switch (o) {
        case ConnectOutcome::ok:                      return "ok";
        case ConnectOutcome::unreachable:             return "unreachable";
        case ConnectOutcome::credentials_rejected:    return "credentials_rejected";
        case ConnectOutcome::stream_absent:           return "stream_absent";
        case ConnectOutcome::capability_absent:       return "capability_absent";
        case ConnectOutcome::transport_closed:        return "transport_closed";
        case ConnectOutcome::refused_unsafe_interface: return "refused_unsafe_interface";
    }
    return "unknown";
}

// The registry's answer. Mirrors the facts the registry already keeps separately
// (paired / revoked / consent / capture) rather than inventing a second policy, and
// deliberately does NOT derive one from another.
enum class AuthorizationOutcome {
    allowed,
    unknown_device,
    not_paired,
    revoked,
    credential_mismatch,
    consent_missing,
    capture_disabled
};

inline const char* authorization_outcome_to_string(AuthorizationOutcome a) {
    switch (a) {
        case AuthorizationOutcome::allowed:             return "allowed";
        case AuthorizationOutcome::unknown_device:      return "unknown_device";
        case AuthorizationOutcome::not_paired:          return "not_paired";
        case AuthorizationOutcome::revoked:             return "revoked";
        case AuthorizationOutcome::credential_mismatch: return "credential_mismatch";
        case AuthorizationOutcome::consent_missing:     return "consent_missing";
        case AuthorizationOutcome::capture_disabled:    return "capture_disabled";
    }
    return "unknown";
}

inline bool is_denied(AuthorizationOutcome a) { return a != AuthorizationOutcome::allowed; }

// ---------------------------------------------------------------- media metadata

// Frame METADATA only. The connector never buffers or forwards pixel data; it needs
// a count and a size to produce a bounded observation, and nothing more.
struct FrameMeta {
    uint32_t bytes = 0;
    bool keyframe = false;
    int64_t pts_ms = 0;
    std::string codec{};   // bounded; "unknown" when the adapter cannot say
};

// What the connector asks for. Bounded so a caller cannot request an unbounded stream.
struct StreamRequest {
    uint32_t max_frames = 1;      // the connector pulls a bounded number, then re-checks
    bool keyframes_only = false;
};

// Control commands. Closed vocabulary, all optional per declared capability.
enum class ControlCommand { ptz_left, ptz_right, ptz_up, ptz_down, ptz_home, light_on, light_off };

inline const char* control_command_to_string(ControlCommand c) {
    switch (c) {
        case ControlCommand::ptz_left:  return "ptz_left";
        case ControlCommand::ptz_right: return "ptz_right";
        case ControlCommand::ptz_up:    return "ptz_up";
        case ControlCommand::ptz_down:  return "ptz_down";
        case ControlCommand::ptz_home:  return "ptz_home";
        case ControlCommand::light_on:  return "light_on";
        case ControlCommand::light_off: return "light_off";
    }
    return "unknown";
}

}  // namespace homeguardian::camera
