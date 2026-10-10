#pragma once

// Android NDK camera backend implementing IMediaDevice.
//
// This uses the NDK camera API (ACameraManager / ACameraDevice /
// ACameraCaptureSession), available since API level 24. It is compiled only
// for Android targets (HOMEGUARDIAN_ANDROID). It contains NO frame decoding,
// face recognition, or inference of any kind.
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

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraMetadata.h>

#include <memory>
#include <string>

namespace homeguardian {

class NdkCameraDeviceImpl : public IMediaDevice {
public:
    NdkCameraDeviceImpl();
    ~NdkCameraDeviceImpl() override;

    DeviceKind kind() const override { return DeviceKind::camera; }

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
    std::string camera_id_;
    ACameraManager* camera_manager_ = nullptr;
    ACameraDevice* camera_device_ = nullptr;
    ACaptureSessionOutputContainer* output_container_ = nullptr;
    ACameraCaptureSession* capture_session_ = nullptr;
    ACaptureSessionOutput* session_output_ = nullptr;
};

} // namespace homeguardian

#endif // HOMEGUARDIAN_ANDROID
