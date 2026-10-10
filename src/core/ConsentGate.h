#pragma once

// Consent gate: a single, narrow decision point that must be consulted before
// any protected acquisition or processing. It fails closed: missing, ambiguous,
// expired, denied, or withdrawn authorization is NOT authorized.
//
// This gate centralizes the consent rule so there is exactly one definition of
// "is this acquisition allowed". Components must not re-implement the rule.

#include "core/ConsentRecord.h"
#include "persistence/IConsentRepository.h"
#include <chrono>
#include <string>
#include <vector>

namespace homeguardian {

enum class DenyReason {
    none,                 // authorized
    capture_disabled,     // media_capture_enabled is false in config
    no_consent_record,    // no record exists for subject+purpose
    decision_not_granted, // latest decision is denied or withdrawn
    purpose_mismatch,     // granted record exists but for a different purpose
    category_missing,     // required data category not in the granted record
    expired,              // grant existed but has expired
    ambiguous             // conflicting active grants for the same purpose
};

struct AcquisitionRequest {
    std::string subject_scope;   // profile_id or household scope
    std::string purpose;         // e.g. "presence", "fall_detection"
    std::string data_category;   // e.g. "camera", "audio", "sensor"
};

struct GateDecision {
    bool authorized = false;
    DenyReason reason = DenyReason::no_consent_record;
    std::string explanation;

    explicit operator bool() const { return authorized; }
};

class ConsentGate {
public:
    ConsentGate(IConsentRepository& consent_repo, bool media_capture_enabled);

    // Evaluates whether the requested acquisition is authorized right now.
    // Pure decision: performs no acquisition and no side effects.
    GateDecision check(const AcquisitionRequest& request,
                       std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;

    static std::string reason_to_string(DenyReason reason);

private:
    IConsentRepository& consent_repo_;
    bool media_capture_enabled_;
};

} // namespace homeguardian
