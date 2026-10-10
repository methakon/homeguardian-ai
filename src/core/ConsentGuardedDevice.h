#pragma once

// A consent-guarded wrapper around an IMediaDevice.
//
// Every protected operation — initialize (when it activates capture), start,
// and any frame/sample acquisition — is preceded by a ConsentGate check. The
// guard fails closed: if authorization cannot be evaluated or is absent,
// denied, withdrawn, expired, or the config kill switch is on, the operation is
// refused and the device is not advanced into (or is forced out of) capture.
//
// The guard also exposes recheck_and_enforce(), which must be called before
// each acquisition. If consent has since been withdrawn/expired, or the device
// has failed, or OS permission has been revoked (via the permission callback),
// the guard forces stop()+close() and reports that acquisition must not
// proceed. This is the single place that ties device lifecycle to consent.

#include "IMediaDevice.h"
#include "ConsentGate.h"
#include <functional>
#include <chrono>
#include <string>

namespace homeguardian {

// Result of a guarded operation.
struct GuardResult {
    bool allowed = false;
    DenyReason deny_reason = DenyReason::no_consent_record;
    std::string explanation;
    explicit operator bool() const { return allowed; }
};

class ConsentGuardedDevice {
public:
    // permission_ok: returns false when the OS has revoked the app's permission
    // for this device (camera/microphone). Defaults to always-true for mocks
    // that do not model OS permissions.
    ConsentGuardedDevice(IMediaDevice& device,
                         ConsentGate& gate,
                         AcquisitionRequest request,
                         std::function<bool()> permission_ok = [] { return true; });

    // Initialize the device, but only if the gate authorizes capture. On deny,
    // the device is left Idle and not initialized.
    GuardResult initialize();

    // Start capture, but only if the gate currently authorizes. On deny, the
    // device is not started.
    GuardResult start();

    // Stop capture. Always allowed (stopping never requires consent).
    void stop() noexcept;

    // Close and release the device. Always allowed.
    void close() noexcept;

    // Called before each frame/sample acquisition. Re-evaluates the gate and
    // the device/permission state; if anything is wrong it forces the device to
    // stop (and close on failure) and returns a denying GuardResult. Returns an
    // allowing GuardResult only when acquisition may proceed right now.
    GuardResult recheck_and_enforce(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    // The delivery gate a real backend MUST call immediately before handing a
    // frame/sample to processing. This is the time-of-check/time-of-use closing
    // point: it re-runs the full consent/permission/device evaluation and only
    // returns true if the payload may be delivered right now. If it returns
    // false the backend MUST drop the payload and must not deliver it, and the
    // device has been forced out of capture.
    bool authorize_delivery(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    DeviceState state() const { return device_.state(); }
    DeviceKind kind() const { return device_.kind(); }

private:
    GuardResult evaluate(std::chrono::system_clock::time_point now) const;

    IMediaDevice& device_;
    ConsentGate& gate_;
    AcquisitionRequest request_;
    std::function<bool()> permission_ok_;
};

} // namespace homeguardian
