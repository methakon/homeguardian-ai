#pragma once

// SiteProfile — the per-site configuration a site connector is bound to
// (milestone HG-NET-02C / Issue #16).
//
// WHY THIS EXISTS
//   The V380 cameras are NOT VPN peers: nothing has shown that these embedded units can
//   run WireGuard. Each camera is therefore reached through an authenticated,
//   continuously running SITE CONNECTOR that lives inside the camera's own LAN/subnet,
//   dials OUT, and holds the credential the camera cannot hold. This header is that
//   connector's binding: which site it belongs to, which camera subnets it may reach,
//   and how it reconnects.
//
// THE RULES IT ENCODES
//   1. A connector has its OWN identity, distinct from every camera it represents. It is
//      a `LogicalDeviceId` (see CameraTypes.h), so it can never be an address.
//   2. REACH IS A CONFIGURED ALLOWLIST, not whatever the host happens to route. A
//      connector may only reach cameras inside the subnets its profile lists. That is
//      what makes multi-site isolation real rather than a promise: a connector at site A
//      cannot reach site B's cameras even if some route exists.
//   3. NOTHING HERE IS AN ADDRESS-BASED IDENTITY. A site is a name; a camera's subnet is
//      reachability, not identity; a camera's IP/MAC/SSID lives in the observed-endpoint
//      record as an ATTACHMENT that may change at any time.

#include "CameraTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian::camera {

// A parsed IPv4 CIDR. Kept deliberately small: the connector compares addresses against
// it, and nothing about it is an identity.
struct Ipv4Cidr {
    uint32_t network = 0;   // host byte order
    uint32_t prefix = 0;    // 0..32
    std::string text{};     // as configured, for logs

    bool contains(uint32_t addr_host_order) const {
        if (prefix == 0) return true;
        const uint32_t mask = (prefix >= 32) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - prefix));
        return (addr_host_order & mask) == (network & mask);
    }

    // Parse "a.b.c.d/n". Returns false on anything malformed - a reach rule that cannot
    // be parsed must never widen into "allow everything".
    static bool parse(const std::string& in, Ipv4Cidr* out);
};

// How a site's uplink behaves, as OBSERVED (never as an identity).
enum class NatClass {
    unknown,
    same_site,        // both peers on the same LAN/subnet
    cone_nat,         // a direct path is usually possible
    symmetric_nat,    // a direct path often is not
    cgnat             // carrier-grade NAT; direct path may be impossible
};

inline const char* nat_class_to_string(NatClass n) {
    switch (n) {
        case NatClass::unknown:       return "unknown";
        case NatClass::same_site:     return "same_site";
        case NatClass::cone_nat:      return "cone_nat";
        case NatClass::symmetric_nat: return "symmetric_nat";
        case NatClass::cgnat:         return "cgnat";
    }
    return "unknown";
}

struct ReconnectPolicy {
    uint32_t max_attempts = 5;
    uint64_t base_backoff_ms = 500;
    uint64_t max_backoff_ms = 30000;
    bool require_reauthorization = true;   // a reconnect must re-evaluate authorization

    // Bounded exponential backoff for attempt N (1-based), clamped to max_backoff_ms.
    uint64_t backoff_for(uint32_t attempt) const {
        if (attempt == 0) return base_backoff_ms;
        uint64_t delay = base_backoff_ms;
        for (uint32_t i = 1; i < attempt && delay < max_backoff_ms; ++i) delay *= 2;
        return delay > max_backoff_ms ? max_backoff_ms : delay;
    }
};

struct SiteProfile {
    // The SITE's name and the CONNECTOR's own identity -- not a camera's.
    std::string site_id;
    std::string connector_device_id;
    std::string connector_display_name;
    // The connector's credential identity (on the release route: a certificate
    // fingerprint/CN). Never the camera's, and never empty in a live profile.
    std::string connector_credential_id;

    // Reach allowlist. Empty means "reach nothing", not "reach everything".
    std::vector<Ipv4Cidr> allowed_camera_subnets{};

    // The connector's own observation of how its uplink behaves. Informational.
    NatClass observed_nat_class = NatClass::unknown;

    uint64_t heartbeat_interval_ms = 30000;
    ReconnectPolicy reconnect{};

    // Control messages the hub may carry are small; anything larger is a media path and
    // must not travel over the hub. See ControlChannel.
    uint32_t max_control_message_bytes = 1024;

    LogicalDeviceId connector_id() const {
        return LogicalDeviceId{site_id, connector_device_id};
    }

    // A profile is usable only when its own identity is valid and it can reach at least
    // one subnet. An unusable profile fails closed: it reaches nothing.
    bool valid() const {
        return connector_id().valid() && !connector_credential_id.empty() &&
               !allowed_camera_subnets.empty();
    }

    // Is this address inside a subnet the profile allows? Fail-closed: an invalid
    // profile, or an unparsable input, answers NO.
    bool permits(uint32_t addr_host_order) const {
        if (!valid()) return false;
        for (const auto& cidr : allowed_camera_subnets) {
            if (cidr.contains(addr_host_order)) return true;
        }
        return false;
    }

    // Parse "a.b.c.d" into host byte order. Returns false when malformed.
    static bool parse_ipv4(const std::string& in, uint32_t* out_host_order);
};

// The connector's OBSERVED view of where a camera currently is. Everything here is an
// attachment: none of it is an authorization input, and updating it must never change a
// device's identity, consent or permissions.
struct ObservedEndpoint {
    LogicalDeviceId device{};
    std::string observed_ip{};        // e.g. "192.168.1.20"
    std::string observed_subnet{};    // e.g. "192.168.1.0/24"
    std::string observed_ssid{};      // may be empty; never logged by default
    NatClass nat_class = NatClass::unknown;
    uint64_t first_seen_ms = 0;
    uint64_t last_seen_ms = 0;
    uint32_t observation_count = 0;

    void reobserve(const std::string& ip, const std::string& subnet, uint64_t at_ms) {
        observed_ip = ip;
        observed_subnet = subnet;
        last_seen_ms = at_ms;
        ++observation_count;
    }
};

}  // namespace homeguardian::camera
