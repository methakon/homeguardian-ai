#include "NdkCameraDevice.h"

#ifdef HOMEGUARDIAN_ANDROID

#include <stdexcept>

namespace homeguardian {

NdkCameraDeviceImpl::NdkCameraDeviceImpl() = default;

NdkCameraDeviceImpl::~NdkCameraDeviceImpl() {
    // Deterministic cleanup: never leave a capture session running.
    stop();
    close();
}

void NdkCameraDeviceImpl::initialize() {
    if (state_ == DeviceState::Error) {
        throw std::runtime_error("NdkCameraDevice: in error state");
    }
    if (state_ != DeviceState::Idle) {
        throw std::runtime_error("NdkCameraDevice: already initialized");
    }

    // Fail closed: refuse to activate capture against unverified hardware.
    // Hardware acceptance testing has not been performed, so we do not open a
    // real capture session. This is deliberate and documented.
    if (!device_confirmed_) {
        state_ = DeviceState::Error;
        throw std::runtime_error(
            "NdkCameraDevice: device not confirmed available; real capture is "
            "disabled until hardware acceptance testing passes");
    }

    // Real device path (only reached when device_confirmed_ is true, i.e. after
    // on-device verification). Selects the first back-facing camera via
    // ACameraManager. Full session setup is completed in F2.2 hardware testing.
    camera_manager_ = ACameraManager_create();
    if (camera_manager_ == nullptr) {
        state_ = DeviceState::Error;
        throw std::runtime_error("NdkCameraDevice: ACameraManager_create failed");
    }

    ACameraIdList* camera_id_list = nullptr;
    camera_status_t status = ACameraManager_getCameraIdList(camera_manager_, &camera_id_list);
    if (status != ACAMERA_OK || camera_id_list == nullptr ||
        camera_id_list->numCameras < 1) {
        if (camera_id_list) {
            ACameraManager_deleteCameraIdList(camera_id_list);
        }
        ACameraManager_delete(camera_manager_);
        camera_manager_ = nullptr;
        state_ = DeviceState::Error;
        throw std::runtime_error("NdkCameraDevice: no camera available");
    }

    camera_id_ = camera_id_list->cameraIds[0];
    ACameraManager_deleteCameraIdList(camera_id_list);

    state_ = DeviceState::Initialized;
}

void NdkCameraDeviceImpl::start() {
    if (state_ != DeviceState::Initialized && state_ != DeviceState::Stopped) {
        state_ = DeviceState::Error;
        throw std::runtime_error("NdkCameraDevice: not in a startable state");
    }
    // Real ACameraDevice_open + capture-session setup is completed during F2.2
    // on-device verification. Until then this path is unreachable because
    // initialize() fails closed when the device is unconfirmed.
    state_ = DeviceState::Capturing;
}

void NdkCameraDeviceImpl::stop() noexcept {
    // Release capture resources so a subsequent start() rebuilds a clean
    // session and repeated start/stop cannot leak session/device handles.
    if (capture_session_ != nullptr) {
        ACameraCaptureSession_close(capture_session_);
        capture_session_ = nullptr;
    }
    if (session_output_ != nullptr) {
        ACaptureSessionOutput_free(session_output_);
        session_output_ = nullptr;
    }
    if (output_container_ != nullptr) {
        ACaptureSessionOutputContainer_free(output_container_);
        output_container_ = nullptr;
    }
    if (camera_device_ != nullptr) {
        ACameraDevice_close(camera_device_);
        camera_device_ = nullptr;
    }
    if (state_ == DeviceState::Capturing) {
        state_ = DeviceState::Stopped;
    }
}

void NdkCameraDeviceImpl::close() noexcept {
    // Tear down any remaining resources in reverse order of creation. stop()
    // already released capture resources; close() also releases the manager.
    stop();
    if (camera_manager_ != nullptr) {
        ACameraManager_delete(camera_manager_);
        camera_manager_ = nullptr;
    }
    camera_id_.clear();
    state_ = DeviceState::Idle;
}

void NdkCameraDeviceImpl::fail() noexcept {
    if (state_ == DeviceState::Capturing) {
        state_ = DeviceState::Stopped;
    }
    state_ = DeviceState::Error;
}

DeviceState NdkCameraDeviceImpl::state() const {
    return state_;
}

std::string NdkCameraDeviceImpl::name() const {
    return "ndk_camera";
}

} // namespace homeguardian

#endif // HOMEGUARDIAN_ANDROID
