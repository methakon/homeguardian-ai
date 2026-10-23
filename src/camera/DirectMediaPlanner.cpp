#include "DirectMediaPlanner.h"

#include "ControlChannel.h"

namespace homeguardian::camera {

MediaPathDecision DirectMediaPlanner::plan(const MediaPathRequest& request, bool allow_relay) const {
    MediaPathDecision decision;

    // 1. Nothing to carry. This is camera 2's shape: the device is reachable but offers no
    //    media interface, so there is no path to plan - and no path may be invented.
    if (!request.connector_declares_media) {
        decision.path = MediaPath::unavailable;
        decision.reason = "the connector declares no media capability: nothing to transport";
        return decision;
    }

    // 2. Same subnet: the simplest and cheapest direct path.
    if (request.same_subnet) {
        decision.path = MediaPath::direct_same_site;
        decision.reason = "both peers are on the same subnet";
        return decision;
    }

    // 3. A direct ICE path, unless a previous attempt already failed for this pair and the
    //    NAT types make another attempt implausible.
    const bool hard_nat = (request.site_nat == NatClass::symmetric_nat ||
                           request.site_nat == NatClass::cgnat ||
                           request.viewer_nat == NatClass::symmetric_nat ||
                           request.viewer_nat == NatClass::cgnat);
    if (!request.direct_attempt_failed && !hard_nat) {
        decision.path = MediaPath::direct_ice;
        decision.reason = "ICE-assisted direct path (neither peer appears to be behind "
                          "symmetric NAT or CGNAT)";
        return decision;
    }

    // 4. The site connector serves the media itself, over the SITE's uplink. This is still
    //    a direct path between the site and the viewer: the bytes never enter the
    //    coordination hub, and the bandwidth bill lands on the site, which is where the
    //    camera actually is. It works behind NAT/CGNAT because the connector dials OUT.
    if (request.site_uplink_usable) {
        decision.path = MediaPath::site_uplink;
        decision.reason = request.direct_attempt_failed
                              ? "no peer-to-peer path for this pair: the site connector serves it "
                                "over the site's own uplink"
                              : "a peer-to-peer path is implausible behind symmetric NAT/CGNAT: "
                                "the site connector serves it over the site's own uplink";
        decision.uses_site_bandwidth = true;
        return decision;
    }

    // 5. Nothing direct remains. A relay is a SEPARATE capacity/permission decision, so it
    //    is reported as required and never taken automatically - and the coordination hub
    //    is not a candidate under any input.
    decision.path = MediaPath::relay_required;
    decision.reason = "no direct path and the site uplink is unavailable: a relay would be "
                      "required, which is a separate capacity and permission decision";
    decision.needs_owner_approval = !allow_relay;
    return decision;
}

uint64_t DirectMediaPlanner::media_to_control_ratio(uint64_t frame_bytes) {
    const uint64_t control = static_cast<uint64_t>(ControlChannel::kMaxEncodedBytes);
    if (control == 0) return 0;
    return frame_bytes / control;
}

}  // namespace homeguardian::camera
