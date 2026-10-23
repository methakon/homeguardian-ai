#pragma once

// IDeviceAuthorizer — the narrow seam the registry satisfies (HG-V380-09, task 1).
//
// WHY A SEAM INSTEAD OF THE REGISTRY DIRECTLY
//   The connector must ask one question — "may this device, presenting this
//   credential, use this capability, right now?" — and must be unable to ask
//   anything else. Depending on the whole `net::DeviceRegistry` would let camera code
//   read or mutate unrelated state; a narrow interface keeps the camera layer on a
//   single, auditable decision point.
//
//   `RegistryAuthorizer` (see RegistryAuthorizer.h) is the production implementation
//   over `homeguardian::net::DeviceRegistry`. A test can substitute a scripted
//   implementation, and a deployment that has no registry can substitute a
//   fail-closed one.
//
// THE INVARIANT THIS PRESERVES
//   NETWORK AUTHENTICATION IS NOT PERMISSION TO COLLECT. The registry keeps paired,
//   revoked, credential binding, consent and the operator's capture switch as
//   separate, independently revocable facts, and this seam returns exactly one of
//   them as the refusal reason instead of collapsing them into a boolean.

#include "CameraTypes.h"

#include <string>

namespace homeguardian::camera {

class IDeviceAuthorizer {
public:
    virtual ~IDeviceAuthorizer() = default;

    // Evaluate one request. Fail-closed: any state the implementation cannot evaluate
    // must return a denial, never `allowed`.
    //
    // `credential_id` is the identity the caller is presenting (on the release route
    // this is the certificate fingerprint/CN bound to the device record). Passing an
    // empty credential must never be treated as "no check needed".
    virtual AuthorizationOutcome authorize(const LogicalDeviceId& id,
                                           const std::string& credential_id,
                                           Capability capability) const = 0;
};

// A fail-closed authorizer for deployments with no registry wired up yet. Every
// request is refused, and the reason names why rather than pretending the device is
// merely unpaired.
class DenyAllAuthorizer final : public IDeviceAuthorizer {
public:
    AuthorizationOutcome authorize(const LogicalDeviceId&,
                                   const std::string&,
                                   Capability) const override {
        return AuthorizationOutcome::unknown_device;
    }
};

}  // namespace homeguardian::camera
