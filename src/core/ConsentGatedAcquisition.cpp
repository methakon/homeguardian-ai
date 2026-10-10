#include "ConsentGatedAcquisition.h"

namespace homeguardian {

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
        return result;
    }

    // Authorized: pull the next synthetic reading. If the sensor is
    // disconnected or empty, nothing is acquired (but it was authorized).
    auto maybe_event = sensor_.next();
    if (!maybe_event.has_value()) {
        result.acquired = false;
        return result;
    }

    result.acquired = true;
    result.event = maybe_event;
    return result;
}

} // namespace homeguardian
