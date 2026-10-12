#include "ConsentGatedAcquisition.h"

#include <chrono>

namespace homeguardian {

namespace {
// Record a metadata-only audit entry for this attempt (roadmap H-03). Best-effort:
// a null log or a logging failure never affects the outcome.
void audit(const PrivacyAuditLog* log, const AcquisitionRequest& request,
           const AcquisitionResult& result) {
    if (!log) return;
    PrivacyAuditEntry e;
    e.timestamp = std::chrono::system_clock::now();
    e.subject_scope = request.subject_scope;
    e.purpose = request.purpose;
    e.data_category = request.data_category;
    e.authorized = result.authorized;
    e.decision_reason = ConsentGate::reason_to_string(result.deny_reason);
    e.acquired = result.acquired;
    const_cast<PrivacyAuditLog*>(log)->record(e);
}
}  // namespace

ConsentGatedAcquisition::ConsentGatedAcquisition(ConsentGate& gate, SimulatedSensorSource& sensor)
    : gate_(gate), sensor_(sensor) {}

AcquisitionResult ConsentGatedAcquisition::acquire(const AcquisitionRequest& request) {
    AcquisitionResult result;

    // Consult the single consent decision point first. If it denies, we stop
    // here: no sensor read is taken and no payload is produced or processed.
    GateDecision decision = gate_.check(request);
    result.authorized = decision.authorized;
    result.deny_reason = decision.reason;
    result.explanation = decision.explanation;

    if (!decision.authorized) {
        result.acquired = false;
        result.event = std::nullopt;
        audit(privacy_audit_, request, result);
        return result;
    }

    // Authorized: pull the next synthetic reading. If the sensor is
    // disconnected or empty, nothing is acquired (but it was authorized).
    auto maybe_event = sensor_.next();
    if (!maybe_event.has_value()) {
        result.acquired = false;
        audit(privacy_audit_, request, result);
        return result;
    }

    result.acquired = true;
    result.event = maybe_event;
    audit(privacy_audit_, request, result);
    return result;
}

} // namespace homeguardian
