#pragma once

// V380 discovery: how a camera is found when its address is a moving target.
//
// THE PROBLEM THIS SOLVES
//   These cameras take DHCP leases, so their addresses change and any stored address rots. The cameras did
//   change: two of the three moved (.20 -> .7 and .19 -> .11) without anything about the devices changing.
//   A design that stores an address therefore breaks quietly; a design that stores it as IDENTITY breaks
//   dangerously, because the address a camera used to hold is later handed to a different device, which would
//   then inherit the camera's consent.
//
// THE ANSWER
//   Identity = the owner-confirmed device id plus the NIC fingerprint (normalized MAC), which were both
//   stable across the moves. An address is an ATTACHMENT attribute: resolved at runtime, cached briefly, and
//   re-resolved on demand. Nothing authorization-related may read it.
//
// WHY THIS IS NOT "SCANNING"
//   One UDP broadcast asking "who is out there" reaches every camera on the segment and each answers with its
//   id, fingerprint, address, mask and gateway in a single exchange. There is no sweep, no per-host probing,
//   and no traffic to anything that is not already listening for the query. A periodic subnet sweep would be
//   slower, noisier, and would enumerate devices nobody asked about - so it is deliberately not what happens
//   here. The only thing that repeats is one broadcast, on demand.
//
// PROTOCOL PROVENANCE
//   The query bytes and the response field order are facts of the documented vendor protocol (the same public
//   lineage the media codec notes). No code was vendored. The response parser was validated against a real
//   device whose device id was independently confirmed by the owner: the field the parser reads as the device
//   id reproduced that confirmed value exactly, which is what makes the field mapping trustworthy rather than
//   assumed.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {
namespace v380 {

// One camera as the device itself describes it.
struct DiscoveryRecord {
    bool valid = false;
    std::string reason;
    std::string mac;        // normalized: lower-case, colon-separated
    std::string address;    // as reported by the device; an ATTACHMENT attribute, never an identity
    std::string gateway;
    std::string netmask;
    int32_t device_id = 0;
    uint16_t media_port = 0;   // the vendor port the device advertises

    bool same_device_as(const DiscoveryRecord& other) const {
        return !mac.empty() && mac == other.mac && device_id == other.device_id;
    }
};

// The exact query, in one place: "NVDEVSEARCH^100" sent to <broadcast>:10008, replies to the BOUND port.
std::string discovery_query();

// Parse one response line. Refuses anything malformed or implausible rather than returning a half-record:
// a discovery record that is wrong is worse than no record, because it becomes an endpoint.
DiscoveryRecord parse_discovery_response(const std::string& text);

// Injection seam, so the resolver's logic is testable with no network and no cameras (the same pattern the
// camera authorizer uses). A stub in tests; UdpV380Discovery on a real LAN.
class IDiscoverySource {
public:
    virtual ~IDiscoverySource() = default;
    virtual std::vector<DiscoveryRecord> discover(std::chrono::milliseconds timeout) = 0;
};

class UdpV380Discovery : public IDiscoverySource {
public:
    struct Config {
        // The SUBNET broadcast address is more reliable than 255.255.255.255, which some stacks send out the
        // wrong interface. Callers pass their own subnet's directed broadcast.
        std::string broadcast_address = "255.255.255.255";
        uint16_t query_port = 10008;
        // The reply goes to the port the socket is BOUND to; an ephemeral source port yields zero responders,
        // which is exactly the bug that produced a false "no cameras" result once already.
        uint16_t reply_port = 10009;
    };

    UdpV380Discovery();
    explicit UdpV380Discovery(Config config);

    std::vector<DiscoveryRecord> discover(std::chrono::milliseconds timeout) override;

private:
    Config cfg_;
};

}  // namespace v380
}  // namespace homeguardian
