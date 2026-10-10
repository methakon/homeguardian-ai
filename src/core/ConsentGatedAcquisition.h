#pragma once

// A consent-gated simulated acquisition path for Phase F1.
//
// There is NO real camera or microphone. This component demonstrates that the
// consent gate is consulted before any "acquisition" and that a denied request
// never produces or processes a protected payload. It is a stand-in for a
// future real hardware boundary and does NOT prove real hardware enforcement.

#include "ConsentGate.h"
#include "SimulatedSensorSource.h"
#include <string>
#include <optional>

namespace homeguardian {

// Result of a gated acquisition attempt.
struct AcquisitionResult {
    bool acquired = false;          // true only if the gate authorized AND a payload was produced
    bool authorized = false;        // gate decision
    DenyReason deny_reason = DenyReason::no_consent_record;
    std::string explanation;
    std::optional<Event> event;     // present only when acquired
};

class ConsentGatedAcquisition {
public:
    ConsentGatedAcquisition(ConsentGate& gate, SimulatedSensorSource& sensor);

    // Attempt to acquire one synthetic observation for the given request. If the
    // gate denies, no event is produced and no payload is processed. If the gate
    // authorizes, the next synthetic reading (if any) is returned as an event.
    AcquisitionResult acquire(const AcquisitionRequest& request);

private:
    ConsentGate& gate_;
    SimulatedSensorSource& sensor_;
};

} // namespace homeguardian
