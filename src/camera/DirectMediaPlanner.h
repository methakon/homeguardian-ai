#pragma once

// DirectMediaPlanner — decides HOW media would travel, and refuses to put it on the
// control hub (milestone HG-NET-02C / Issue #16).
//
// THE RULE
//   Media takes a direct, end-to-end encrypted path. The coordination hub carries
//   signalling only. A relay is a separate capacity/permission decision and is never an
//   automatic fallback - if no direct path exists, the honest answer is
//   `relay_required` with `needs_owner_approval`, not a silent detour through the hub.
//
// WHY `uses_control_hub` EXISTS AT ALL
//   It is always false. It is a field so that a test can assert the invariant across the
//   whole input space instead of trusting a comment: there is no code path in `plan()`
//   that sets it, and a future edit that tried to would fail those assertions.

#include "SiteProfile.h"

#include <string>

namespace homeguardian::camera {

enum class MediaPath {
    unavailable,        // nothing can carry it (e.g. the camera declares no media capability)
    direct_same_site,   // both peers on the same subnet
    direct_ice,         // ICE-assisted direct path between peers
    site_uplink,        // the site connector serves it over the SITE's own uplink
    relay_required      // no direct path: needs an explicit, separately approved relay
};

inline const char* media_path_to_string(MediaPath p) {
    switch (p) {
        case MediaPath::unavailable:      return "unavailable";
        case MediaPath::direct_same_site: return "direct_same_site";
        case MediaPath::direct_ice:       return "direct_ice";
        case MediaPath::site_uplink:      return "site_uplink";
        case MediaPath::relay_required:   return "relay_required";
    }
    return "unknown";
}

struct MediaPathRequest {
    NatClass site_nat = NatClass::unknown;
    NatClass viewer_nat = NatClass::unknown;
    bool same_subnet = false;
    bool connector_declares_media = true;
    bool direct_attempt_failed = false;   // ICE already failed for this pair
    // False when the site's own uplink cannot serve media (unavailable, metered beyond the
    // operator's policy, or policy forbids serving from the site). Then, and only then, is
    // a relay the remaining option.
    bool site_uplink_usable = true;
};

struct MediaPathDecision {
    MediaPath path = MediaPath::unavailable;
    std::string reason{};
    // True only when a relay is required AND the caller had not pre-approved one. A relay
    // is never something this planner decides on its own.
    bool needs_owner_approval = false;
    // ALWAYS false. Asserted, not assumed.
    bool uses_control_hub = false;
    // The rough direction of the bytes, so an operator can see whose uplink pays for it.
    bool uses_site_bandwidth = false;
};

class DirectMediaPlanner {
public:
    // `allow_relay` defaults to FALSE. Passing true means the owner has separately approved
    // a relay for this deployment; the planner still reports `relay_required` and simply
    // clears `needs_owner_approval`.
    MediaPathDecision plan(const MediaPathRequest& request, bool allow_relay = false) const;

    // Measured throughput of the coordination channel is irrelevant here, but the ratio is
    // worth stating in code: this is the multiplier by which media would exceed a control
    // message, used by the documentation and by the refusal in ControlChannel.
    static uint64_t media_to_control_ratio(uint64_t frame_bytes);
};

}  // namespace homeguardian::camera
