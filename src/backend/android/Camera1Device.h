#pragma once

// Camera1 (legacy android.hardware.Camera) acquisition device, as seen from the native side.
//
// WHY THIS TYPE EXISTS - the defect that blocked physical acceptance:
//   nativeStartCamera() built an NdkCameraDeviceImpl unconditionally and REQUIRED its initialize() to
//   succeed. On the validation device (Android 8.1 / API 27 / MT6580) the NDK camera2 API enumerates
//   ZERO cameras, so the app failed with
//       camera: initialization failed (NdkCameraDevice: no camera available)
//   and the REAL, already-implemented Camera1 path (Camera1Bridge: Camera.open(), NV21 preview
//   callbacks, per-frame native delivery gate) could never be reached. Treating the NDK device as if it
//   were the Camera1 acquisition device was the mistake.
//
// WHAT THIS TYPE IS: the native lifecycle representation of the JAVA Camera1 bridge. It owns no
// Android handles - the bridge owns Camera/SurfaceTexture and releases them. It records what the
// platform and the bridge report and enforces the SAME lifecycle contract as the NDK backend, so
// ConsentGuardedDevice, the per-frame authorization and the capture accounting work unchanged.
//
// HARD RULES ENCODED HERE:
//   * a platform camera count of 0 makes initialize() refuse - it does not pretend a camera exists;
//   * enumeration is NOT capture: initialize() only means "there is a camera to open";
//   * a failed Camera.open()/startPreview(), reported through set_java_start_failed(), forces
//     Capturing -> Stopped, so the native state can never claim capture the Java side does not have.
//
// It is deliberately Android-free (no NDK headers), so the APK and the host test suite compile the
// same code - the lifecycle is unit-tested rather than asserted in prose.

// Qualified on purpose: a bare "IMediaDevice.h" resolves only when the including file sits in
// src/core (quoted includes search that directory first). This header lives in src/backend/android,
// so it must resolve through the target's include path instead - otherwise every target that is not
// given src/core explicitly fails to compile it.
#include "core/IMediaDevice.h"

#include <mutex>
#include <string>

namespace homeguardian {

class Camera1Device : public IMediaDevice {
public:
    explicit Camera1Device(std::string label = "camera1 (java legacy bridge)");

    DeviceKind kind() const override { return DeviceKind::camera; }

    void initialize() override;   // refuses when the platform reports no legacy camera
    void start() override;        // Initialized|Stopped -> Capturing
    void stop() noexcept override;
    void close() noexcept override;
    void fail() noexcept override;

    DeviceState state() const override;
    std::string name() const override;

    // Reported by the Activity/JNI, never from a camera callback thread.
    void set_platform_camera_count(int count);       // Camera.getNumberOfCameras()
    void set_java_start_failed(const std::string& reason);  // Camera.open()/startPreview() failed

    int platform_camera_count() const;
    std::string last_java_failure() const;

private:
    mutable std::mutex m_;
    std::string label_;
    DeviceState state_ = DeviceState::Idle;
    int platform_camera_count_ = -1;   // -1 = not reported yet
    std::string last_java_failure_;
};

} // namespace homeguardian
