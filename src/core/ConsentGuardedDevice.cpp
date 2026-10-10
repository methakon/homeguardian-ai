#include "ConsentGuardedDevice.h"

namespace homeguardian {

ConsentGuardedDevice::ConsentGuardedDevice(IMediaDevice& device,
                                           ConsentGate& gate,
                                           AcquisitionRequest request,
                                           std::function<bool()> permission_ok)
    : device_(device), gate_(gate), request_(std::move(request)),
      permission_ok_(std::move(permission_ok)) {}

GuardResult ConsentGuardedDevice::evaluate(std::chrono::system_clock::time_point now) const {
    GuardResult result;

    // 1. OS permission must still be granted. Revocation fails closed.
    if (permission_ok_ && !permission_ok_()) {
        result.deny_reason = DenyReason::capture_disabled;
        result.explanation = "OS permission revoked";
        return result;
    }

    // 2. Device must not be in an error state.
    if (device_.state() == DeviceState::Error) {
        result.deny_reason = DenyReason::capture_disabled;
        result.explanation = "device in error state";
        return result;
    }

    // 3. The single consent decision point.
    GateDecision decision = gate_.check(request_, now);
    result.allowed = decision.authorized;
    result.deny_reason = decision.reason;
    result.explanation = decision.explanation;
    return result;
}

GuardResult ConsentGuardedDevice::initialize() {
    auto now = std::chrono::system_clock::now();
    GuardResult result = evaluate(now);
    if (!result.allowed) {
        // Fail closed: do not initialize/activate capture.
        return result;
    }
    device_.initialize();
    result.allowed = true;
    result.deny_reason = DenyReason::none;
    result.explanation = "initialized";
    return result;
}

GuardResult ConsentGuardedDevice::start() {
    auto now = std::chrono::system_clock::now();
    GuardResult result = evaluate(now);
    if (!result.allowed) {
        return result;
    }
    // Only start from a valid state.
    if (device_.state() != DeviceState::Initialized &&
        device_.state() != DeviceState::Stopped) {
        result.allowed = false;
        result.deny_reason = DenyReason::capture_disabled;
        result.explanation = "device not in a startable state";
        return result;
    }
    device_.start();
    result.allowed = true;
    result.deny_reason = DenyReason::none;
    result.explanation = "capturing";
    return result;
}

void ConsentGuardedDevice::stop() noexcept {
    device_.stop();
}

void ConsentGuardedDevice::close() noexcept {
    device_.close();
}

GuardResult ConsentGuardedDevice::recheck_and_enforce(std::chrono::system_clock::time_point now) {
    GuardResult result = evaluate(now);
    if (!result.allowed) {
        // Authorization or device/permission state is no longer valid: force the
        // device out of capture and release resources so no further acquisition
        // can occur. On a device error we release resources but do NOT return
        // the device to Idle, because an errored device must stay closed until
        // it is explicitly re-initialized; otherwise a later check could see a
        // healthy Idle device and wrongly re-authorize delivery.
        if (device_.state() == DeviceState::Capturing ||
            device_.state() == DeviceState::Initialized) {
            device_.stop();
        }
        if (device_.state() == DeviceState::Error) {
            device_.close();
            device_.fail();  // keep the device in Error after releasing resources
        }
        return result;
    }
    result.explanation = "acquisition permitted";
    return result;
}

bool ConsentGuardedDevice::authorize_delivery(std::chrono::system_clock::time_point now) {
    // Re-run the full evaluation immediately before delivery. On any denial the
    // device is forced out of capture and the payload must be dropped.
    GuardResult result = recheck_and_enforce(now);
    return result.allowed;
}

} // namespace homeguardian
