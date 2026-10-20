#include "RegistryAuthorizer.h"

namespace homeguardian::camera {

RegistryAuthorizer::RegistryAuthorizer(const homeguardian::net::DeviceRegistry& registry,
                                       std::string expected_purpose)
    : registry_(registry), purpose_(std::move(expected_purpose)) {}

AuthorizationOutcome RegistryAuthorizer::authorize(const LogicalDeviceId& id,
                                                   const std::string& credential_id,
                                                   Capability capability) const {
    // The capability is part of the seam for future per-capability consent records.
    // It deliberately does NOT select the decision today: the registry keeps
    // permission facts (paired / revoked / consent / capture) separately from what a
    // device claims it can do, and a claimed capability must never widen permission.
    // The connector enforces declared capabilities as their own, separate check.
    (void)capability;

    if (!id.valid()) return AuthorizationOutcome::unknown_device;

    const auto* device = registry_.find(id.device_id);
    if (device == nullptr) return AuthorizationOutcome::unknown_device;

    // Revocation first: a revoked device must be reported as revoked even if its
    // pairing or consent was also cleared, because "revoked" is the fact an operator
    // needs to see.
    if (device->revoked || device->revoked_by_file) return AuthorizationOutcome::revoked;

    if (!device->paired) return AuthorizationOutcome::not_paired;

    // Identity: bind the presented credential to the record. An empty credential is
    // NOT "no check needed" - a physical device record without a bound credential
    // cannot be used at all.
    if (device->credential_id.empty() || credential_id.empty()) {
        return AuthorizationOutcome::credential_mismatch;
    }
    switch (registry_.authenticate(id.device_id, credential_id)) {
        case homeguardian::net::DeviceRegistry::AuthResult::ok:
            break;
        case homeguardian::net::DeviceRegistry::AuthResult::revoked:
            return AuthorizationOutcome::revoked;
        case homeguardian::net::DeviceRegistry::AuthResult::unpaired:
            return AuthorizationOutcome::not_paired;
        case homeguardian::net::DeviceRegistry::AuthResult::unknown_device:
            return AuthorizationOutcome::unknown_device;
        case homeguardian::net::DeviceRegistry::AuthResult::credential_mismatch:
        case homeguardian::net::DeviceRegistry::AuthResult::no_credential:
            return AuthorizationOutcome::credential_mismatch;
    }

    // Device-scoped consent, evaluated for the purpose this deployment records.
    if (!device->consent_granted) return AuthorizationOutcome::consent_missing;
    if (!purpose_.empty() && !device->consent_purpose.empty() &&
        device->consent_purpose != purpose_) {
        return AuthorizationOutcome::consent_missing;
    }

    // The operator's switch is the last gate, and it is never inferred from consent.
    if (!device->capture_enabled) return AuthorizationOutcome::capture_disabled;

    return AuthorizationOutcome::allowed;
}

}  // namespace homeguardian::camera
