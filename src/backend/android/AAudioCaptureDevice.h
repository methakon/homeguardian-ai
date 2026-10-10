#pragma once

// Android NDK audio-capture backend implementing IMediaDevice.
//
// This uses AAudio (available since API level 26). It is compiled only for
// Android targets (HOMEGUARDIAN_ANDROID). It contains NO speech recognition,
// voiceprints, or emotion inference of any kind.
//
// IMPORTANT: capture is fail-closed. open_capture() requires that a target
// device has been confirmed available (see set_device_confirmed()). Because
// hardware acceptance testing has not been performed (no device attached at
// authoring time), this backend refuses to open real capture by default and
// reports DeviceState::Error rather than pretending capture works. The
// ConsentGuardedDevice wrapper still gates every lifecycle call through the
// consent gate regardless.

#include "IMediaDevice.h"

#ifdef HOMEGUARDIAN_ANDROID

#include <aaudio/AAudio.h>

#include <string>

namespace homeguardian {

class AAudioCaptureDeviceImpl : public IMediaDevice {
public:
    AAudioCaptureDeviceImpl();
    ~AAudioCaptureDeviceImpl() override;

    DeviceKind kind() const override { return DeviceKind::audio; }

    void initialize() override;
    void start() override;
    void stop() noexcept override;
    void close() noexcept override;
    void fail() noexcept override;
    DeviceState state() const override;
    std::string name() const override;

    // Must be called with true only after a physical device has been confirmed
    // available AND hardware acceptance testing has passed. Until then,
    // initialize() fails closed. Default false.
    void set_device_confirmed(bool confirmed) { device_confirmed_ = confirmed; }

private:
    DeviceState state_ = DeviceState::Idle;
    bool device_confirmed_ = false;
    AAudioStream* stream_ = nullptr;
};

} // namespace homeguardian

#endif // HOMEGUARDIAN_ANDROID
