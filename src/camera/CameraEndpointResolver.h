#pragma once

// CameraEndpointResolver - turn a logical camera into a usable address, every time, without trusting one.
//
// The registry holds LOGICAL identity ({device id, fingerprint, credential, consent}). This component answers
// the only question the transport needs: "where is that device RIGHT NOW?", and it answers it in a way that
// cannot be fooled by an address.
//
// RULES IT ENFORCES
//   1. An address is never identity. It is resolved, used, and re-resolved; nothing authorization-related reads
//      it, so a device that takes over a stale address inherits nothing.
//   2. The FINGERPRINT decides. A responder claiming a known device id but presenting a different MAC is
//      refused (`fingerprint_mismatch`) - otherwise a rogue device could claim a camera's id and inherit its
//      consent. This is the reason the check is not merely advisory.
//   3. Conflicting observations apply NOTHING. Two responders with one device id, or one address claimed by two
//      responders, means the pass is not evidence - report it and resolve nothing.
//   4. A moved address is not a new device (`resolved_moved`). Identity and consent survive; only the
//      attachment is updated. Reporting a move as a stranger is how a registry fills with phantoms.
//   5. Fail closed. `not_found` and every refusal return no address, so a caller cannot accidentally connect
//      to whatever happens to answer.

#include "camera/V380Discovery.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace homeguardian {
namespace v380 {

enum class ResolutionOutcome {
    resolved,              // found at the address already expected
    resolved_moved,        // found, at a new address: same device, updated attachment
    not_found,             // no responder claimed this device
    conflict,              // contradictory observations: nothing applied
    fingerprint_mismatch,  // a responder claimed the id with a different fingerprint: refused
};

const char* to_string(ResolutionOutcome outcome);

struct ResolvedEndpoint {
    ResolutionOutcome outcome = ResolutionOutcome::not_found;
    std::string device_id;
    std::string address;      // empty unless the outcome is resolved or resolved_moved
    std::string observed_mac;
    std::string reason;
};

class CameraEndpointResolver {
public:
    struct Config {
        // How long a resolved address may be reused before the device is asked again. Short, because a stale
        // address costs a failed connection, and a broadcast is cheap.
        std::chrono::seconds cache_ttl{120};
    };

    CameraEndpointResolver();
    explicit CameraEndpointResolver(Config config);

    // Learn what the registry already knows. Called once per registered camera.
    void expect(const std::string& device_id, const std::string& expected_mac);

    // The fast path: a still-fresh address, used without asking the network.
    bool cached_address(const std::string& device_id, std::chrono::steady_clock::time_point now,
                        std::string* address) const;

    // The real answer: one broadcast, then the rules above. Called on a cold start and whenever a connection
    // to a cached address fails, which is exactly how a DHCP move is discovered.
    ResolvedEndpoint resolve(const std::string& device_id, IDiscoverySource& source,
                             std::chrono::steady_clock::time_point now,
                             std::chrono::milliseconds timeout);

    // Invalidate one camera's cache (a failed connection means the address is suspect).
    void invalidate(const std::string& device_id);

    size_t resolutions() const { return resolutions_; }
    size_t moves_detected() const { return moves_detected_; }
    size_t refusals() const { return refusals_; }

private:
    struct Expected {
        std::string mac;
        std::string address;
        std::chrono::steady_clock::time_point resolved_at{};
        bool has_address = false;
    };
    Config cfg_;
    std::unordered_map<std::string, Expected> expected_;
    size_t resolutions_ = 0;
    size_t moves_detected_ = 0;
    size_t refusals_ = 0;
};

}  // namespace v380
}  // namespace homeguardian
