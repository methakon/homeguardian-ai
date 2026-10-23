#include "camera/CameraEndpointResolver.h"

namespace homeguardian {
namespace v380 {

const char* to_string(ResolutionOutcome outcome) {
    switch (outcome) {
        case ResolutionOutcome::resolved: return "resolved";
        case ResolutionOutcome::resolved_moved: return "resolved_moved";
        case ResolutionOutcome::not_found: return "not_found";
        case ResolutionOutcome::conflict: return "conflict";
        case ResolutionOutcome::fingerprint_mismatch: return "fingerprint_mismatch";
    }
    return "unknown";
}

CameraEndpointResolver::CameraEndpointResolver() = default;
CameraEndpointResolver::CameraEndpointResolver(Config config) : cfg_(config) {}

void CameraEndpointResolver::expect(const std::string& device_id, const std::string& expected_mac) {
    Expected& e = expected_[device_id];
    e.mac = expected_mac;
}

bool CameraEndpointResolver::cached_address(const std::string& device_id,
                                           std::chrono::steady_clock::time_point now,
                                           std::string* address) const {
    if (address == nullptr) return false;
    const auto it = expected_.find(device_id);
    if (it == expected_.end() || !it->second.has_address) return false;
    if (now - it->second.resolved_at > cfg_.cache_ttl) return false;
    *address = it->second.address;
    return true;
}

void CameraEndpointResolver::invalidate(const std::string& device_id) {
    const auto it = expected_.find(device_id);
    if (it != expected_.end()) {
        it->second.has_address = false;
        it->second.address.clear();
    }
}

ResolvedEndpoint CameraEndpointResolver::resolve(const std::string& device_id, IDiscoverySource& source,
                                                std::chrono::steady_clock::time_point now,
                                                std::chrono::milliseconds timeout) {
    ResolvedEndpoint out;
    out.device_id = device_id;

    const auto known = expected_.find(device_id);
    if (known == expected_.end() || known->second.mac.empty()) {
        // Resolving a camera nobody registered would be onboarding by accident.
        out.outcome = ResolutionOutcome::not_found;
        out.reason = "no expected fingerprint for this device id: it is not registered here";
        ++refusals_;
        return out;
    }
    const std::string& want_mac = known->second.mac;

    // One broadcast. Not a sweep: this asks a question, it does not enumerate.
    const auto responders = source.discover(timeout);

    // Rule 5 first: contradictory observations mean the pass is not evidence, so nothing is applied.
    std::vector<const DiscoveryRecord*> claimed;
    std::unordered_map<std::string, int> address_claims;
    for (const auto& r : responders) {
        if (!r.valid) continue;
        address_claims[r.address]++;
        if (std::to_string(r.device_id) == device_id) claimed.push_back(&r);
    }
    if (claimed.size() > 1) {
        out.outcome = ResolutionOutcome::conflict;
        out.reason = "more than one responder claims this device id: applying nothing";
        ++refusals_;
        return out;
    }

    if (claimed.empty()) {
        // No id match. A responder with the RIGHT FINGERPRINT but a changed device id would be a different,
        // more interesting failure - report it as a mismatch rather than as absence, because absence is a
        // network fact and this is an identity fact.
        for (const auto& r : responders) {
            if (r.valid && r.mac == want_mac) {
                out.outcome = ResolutionOutcome::fingerprint_mismatch;
                out.observed_mac = r.mac;
                out.reason = "a device with the expected fingerprint answers under a different device id";
                ++refusals_;
                return out;
            }
        }
        out.outcome = ResolutionOutcome::not_found;
        out.reason = "no responder claimed this device id";
        return out;
    }

    const DiscoveryRecord& r = **claimed.begin();
    // Rule 2, the security-critical one: the fingerprint decides, not the claimed id.
    if (r.mac != want_mac) {
        out.outcome = ResolutionOutcome::fingerprint_mismatch;
        out.observed_mac = r.mac;
        out.reason = "the responder claims this device id with a DIFFERENT fingerprint: refused, because "
                     "accepting it would let another device inherit this camera's consent";
        ++refusals_;
        return out;
    }
    if (address_claims[r.address] > 1) {
        out.outcome = ResolutionOutcome::conflict;
        out.observed_mac = r.mac;
        out.reason = "two responders report the same address: the pass is inconsistent, applying nothing";
        ++refusals_;
        return out;
    }

    out.observed_mac = r.mac;
    out.address = r.address;
    const bool moved = known->second.has_address && known->second.address != r.address;
    out.outcome = moved ? ResolutionOutcome::resolved_moved : ResolutionOutcome::resolved;
    if (moved) {
        ++moves_detected_;
        out.reason = "same fingerprint, new address: the attachment moved, the identity did not";
    }

    Expected& e = expected_[device_id];
    e.address = r.address;
    e.resolved_at = now;
    e.has_address = true;
    ++resolutions_;
    return out;
}

}  // namespace v380
}  // namespace homeguardian
