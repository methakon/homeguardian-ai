#include "AAudioCaptureDevice.h"

#ifdef HOMEGUARDIAN_ANDROID

#include <stdexcept>

namespace homeguardian {

AAudioCaptureDeviceImpl::AAudioCaptureDeviceImpl() = default;

AAudioCaptureDeviceImpl::~AAudioCaptureDeviceImpl() {
    // Deterministic cleanup: never leave a stream running.
    stop();
    close();
}

void AAudioCaptureDeviceImpl::initialize() {
    if (state_ == DeviceState::Error) {
        throw std::runtime_error("AAudioCaptureDevice: in error state");
    }
    if (state_ != DeviceState::Idle) {
        throw std::runtime_error("AAudioCaptureDevice: already initialized");
    }

    // Fail closed: refuse to activate capture against unverified hardware.
    // Hardware acceptance testing has not been performed, so we do not open a
    // real capture stream. This is deliberate and documented.
    if (!device_confirmed_) {
        state_ = DeviceState::Error;
        throw std::runtime_error(
            "AAudioCaptureDevice: device not confirmed available; real capture is "
            "disabled until hardware acceptance testing passes");
    }

    // Real device path (only reached when device_confirmed_ is true). Builds an
    // AAudio capture stream builder. Full stream configuration and start are
    // completed in F2.2 hardware testing.
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: AAudio_createStreamBuilder failed");
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        stream_ = nullptr;
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: AAudioStreamBuilder_openStream failed");
    }

    state_ = DeviceState::Initialized;
}

void AAudioCaptureDeviceImpl::start() {
    if (state_ != DeviceState::Initialized && state_ != DeviceState::Stopped) {
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: not in a startable state");
    }
    if (stream_ == nullptr) {
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: no open stream");
    }
    // Real AAudioStream_requestStart is completed during F2.2 on-device
    // verification. Until then this path is unreachable because initialize()
    // fails closed when the device is unconfirmed.
    aaudio_result_t result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: AAudioStream_requestStart failed");
    }
    state_ = DeviceState::Capturing;
}

void AAudioCaptureDeviceImpl::stop() noexcept {
    if (state_ == DeviceState::Capturing && stream_ != nullptr) {
        AAudioStream_requestStop(stream_);
        state_ = DeviceState::Stopped;
    }
}

void AAudioCaptureDeviceImpl::close() noexcept {
    if (stream_ != nullptr) {
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
    state_ = DeviceState::Idle;
}

void AAudioCaptureDeviceImpl::fail() noexcept {
    if (state_ == DeviceState::Capturing) {
        state_ = DeviceState::Stopped;
    }
    state_ = DeviceState::Error;
}

DeviceState AAudioCaptureDeviceImpl::state() const {
    return state_;
}

std::string AAudioCaptureDeviceImpl::name() const {
    return "aaudio_capture";
}

} // namespace homeguardian

#endif // HOMEGUARDIAN_ANDROID
