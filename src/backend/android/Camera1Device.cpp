#include "backend/android/Camera1Device.h"

#include <stdexcept>
#include <utility>

namespace homeguardian {

Camera1Device::Camera1Device(std::string label) : label_(std::move(label)) {}

void Camera1Device::initialize() {
    std::lock_guard<std::mutex> lk(m_);

    if (platform_camera_count_ < 0) {
        state_ = DeviceState::Error;
        throw std::runtime_error(label_ +
                                 ": the platform camera count has not been reported yet (the Activity "
                                 "must call nativeCameraReport first)");
    }
    if (platform_camera_count_ == 0) {
        state_ = DeviceState::Error;
        throw std::runtime_error(label_ + ": no camera available (Camera.getNumberOfCameras() == 0)");
    }
    if (state_ == DeviceState::Capturing) return;   // already capturing: nothing to do
    state_ = DeviceState::Initialized;
}

void Camera1Device::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != DeviceState::Initialized && state_ != DeviceState::Stopped) {
        throw std::runtime_error(label_ + ": cannot start from state " +
                                 device_state_to_string(state_));
    }
    if (platform_camera_count_ <= 0) {
        state_ = DeviceState::Error;
        throw std::runtime_error(label_ + ": no camera available");
    }
    // A new start attempt clears the previous failure: the bridge is about to try again, and the
    // outcome will be reported through set_java_start_failed()/nativeCameraStartResult().
    last_java_failure_.clear();
    state_ = DeviceState::Capturing;
}

void Camera1Device::stop() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == DeviceState::Capturing) state_ = DeviceState::Stopped;
}

void Camera1Device::close() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    state_ = DeviceState::Idle;
}

void Camera1Device::fail() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    state_ = DeviceState::Error;
}

DeviceState Camera1Device::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
}

std::string Camera1Device::name() const { return label_; }

void Camera1Device::set_platform_camera_count(int count) {
    std::lock_guard<std::mutex> lk(m_);
    platform_camera_count_ = count;
}

void Camera1Device::set_java_start_failed(const std::string& reason) {
    std::lock_guard<std::mutex> lk(m_);
    last_java_failure_ = reason;
    // The Java side does NOT hold a capturing camera. Whatever we believed, it is not capturing now -
    // this is the rule that stops `video=on` outliving a failed Camera.open()/startPreview().
    if (state_ == DeviceState::Capturing) state_ = DeviceState::Stopped;
}

int Camera1Device::platform_camera_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return platform_camera_count_;
}

std::string Camera1Device::last_java_failure() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_java_failure_;
}

} // namespace homeguardian
