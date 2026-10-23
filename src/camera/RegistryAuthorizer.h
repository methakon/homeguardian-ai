#pragma once

// RegistryAuthorizer — adapts the existing bounded device registry to the camera
// layer's authorization seam (HG-V380-09, task 1).
//
// It reads, in this order, the facts the registry already keeps separately:
//   1. the device record exists                -> unknown_device
//   2. authenticated credential binding        -> credential_mismatch / not_paired
//   3. revocation (operator record or file)    -> revoked
//   4. device-scoped consent for a purpose     -> consent_missing
//   5. the operator's capture switch           -> capture_disabled
//
// ORDER MATTERS: identity is checked before permission, and revocation before
// consent, so the reported reason is the one that actually stands in the way rather
// than the last one evaluated. Nothing here derives one fact from another — the
// registry's own invariant, restated at its header: network authentication is not
// permission to collect.
//
// NO WRITES. This class is const-correct against the registry; pairing, consent,
// revocation and the capture switch remain the operator's actions through the
// registry's own API and the registry CLI.

#include "IDeviceAuthorizer.h"

#include "net/DeviceRegistry.h"

namespace homeguardian::camera {

class RegistryAuthorizer final : public IDeviceAuthorizer {
public:
    // `registry` is borrowed and must outlive this object. `expected_purpose` is the
    // purpose string this deployment records consent under (e.g. "presence"); an
    // empty purpose means consent is evaluated by grant alone.
    explicit RegistryAuthorizer(const homeguardian::net::DeviceRegistry& registry,
                                std::string expected_purpose = "presence");

    AuthorizationOutcome authorize(const LogicalDeviceId& id,
                                   const std::string& credential_id,
                                   Capability capability) const override;

    const std::string& expected_purpose() const { return purpose_; }

private:
    const homeguardian::net::DeviceRegistry& registry_;
    std::string purpose_;
};

}  // namespace homeguardian::camera
