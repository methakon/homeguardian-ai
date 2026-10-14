#include "AAudioCaptureDevice.h"

#ifdef HOMEGUARDIAN_ANDROID

#include "core/CaptureAccounting.h"

#include <android/log.h>
#include <stdexcept>

#define ACD_LOG_TAG "HGAudio"
#define ACD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ACD_LOG_TAG, __VA_ARGS__)
#define ACD_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ACD_LOG_TAG, __VA_ARGS__)

namespace homeguardian {

namespace {
inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

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

    // Fail closed: refuse to open a real capture stream until the operator has
    // explicitly started acquisition AND the OS granted RECORD_AUDIO (the JNI
    // layer sets this flag only then).
    if (!device_confirmed_) {
        state_ = DeviceState::Error;
        throw std::runtime_error(
            "AAudioCaptureDevice: device not confirmed available; real capture is "
            "disabled until the operator starts acquisition and permission is granted");
    }

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        state_ = DeviceState::Error;
        throw std::runtime_error("AAudioCaptureDevice: AAudio_createStreamBuilder failed");
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // 16-bit PCM: the compatible choice on older devices and the format the
    // accountant's byte maths assumes. No format conversion, no resampling.
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, config_.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, config_.channels);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, config_.frames_per_callback);
    // The real delivery contract: every buffer goes through the callback below.
    AAudioStreamBuilder_setDataCallback(builder, &AAudioCaptureDeviceImpl::data_callback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AAudioCaptureDeviceImpl::error_callback, this);

    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        stream_ = nullptr;
        state_ = DeviceState::Error;
        ACD_LOGW("openStream failed: %s", AAudio_convertResultToText(result));
        throw std::runtime_error("AAudioCaptureDevice: AAudioStreamBuilder_openStream failed");
    }

    // Record what the device actually gave us (the requested rate/channels are
    // only a request; the accountant validates against these real values).
    negotiated_rate_.store(AAudioStream_getSampleRate(stream_));
    negotiated_channels_.store(AAudioStream_getChannelCount(stream_));
    stop_requested_.store(false);
    ACD_LOGI("stream open: rate=%d channels=%d format=%d frames/cb=%d",
             negotiated_rate_.load(), negotiated_channels_.load(),
             (int)AAudioStream_getFormat(stream_), config_.frames_per_callback);

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
    stop_requested_.store(false);
    aaudio_result_t result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
        state_ = DeviceState::Error;
        ACD_LOGW("requestStart failed: %s", AAudio_convertResultToText(result));
        throw std::runtime_error("AAudioCaptureDevice: AAudioStream_requestStart failed");
    }
    state_ = DeviceState::Capturing;
}

// Runs on the AAudio driver thread. Must stay bounded and must never throw: an
// escaping exception would abort the process.
aaudio_data_callback_result_t AAudioCaptureDeviceImpl::data_callback(
    AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames) {
    auto* self = static_cast<AAudioCaptureDeviceImpl*>(user_data);
    if (self == nullptr || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    const int32_t channels = self->negotiated_channels_.load();
    const int32_t rate = self->negotiated_rate_.load();
    // PCM_I16 => 2 bytes per sample per channel.
    const size_t bytes = static_cast<size_t>(num_frames) *
                         static_cast<size_t>(channels > 0 ? channels : 1) * sizeof(int16_t);

    // The delivery decision for THIS buffer: an injected callback that
    // re-evaluates the consent gate, OS permission and capture state.
    bool authorized = false;
    try {
        authorized = self->authorize_ ? self->authorize_() : false;   // empty -> deny
    } catch (...) {
        authorized = false;
    }

    // The payload itself is never inspected, copied or stored here beyond the
    // accountant's bounded bookkeeping: no decode, no feature, no retention.
    CaptureAccountant::Decision decision;
    if (self->accountant_ != nullptr) {
        decision = self->accountant_->account_audio(
            static_cast<uint32_t>(rate > 0 ? rate : 0),
            static_cast<uint32_t>(channels > 0 ? channels : 0),
            static_cast<uint32_t>(num_frames), bytes, now_ms(), authorized, now_ms());
    }

    if (authorized && self->frame_observer_ != nullptr && audio_data != nullptr) {
        // ONLY an authorized buffer reaches the feature observer. Bounded, non-blocking, and a
        // throw here never takes the capture thread down.
        try {
            self->frame_observer_(static_cast<const int16_t*>(audio_data), num_frames);
        } catch (...) {
        }
    }

    if (!authorized) {
        // Consent/permission/state refusal: ask the control thread to stop. Do
        // NOT call requestStop() from the driver thread.
        self->stop_requested_.store(true);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// Runs on an AAudio thread on stream error (disconnect, xrun, server death).
void AAudioCaptureDeviceImpl::error_callback(AAudioStream* /*stream*/, void* user_data,
                                             aaudio_result_t error) {
    auto* self = static_cast<AAudioCaptureDeviceImpl*>(user_data);
    if (self == nullptr) return;
    if (self->accountant_ != nullptr) self->accountant_->note_xrun();
    ACD_LOGW("audio stream error: %s", AAudio_convertResultToText(error));
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        self->stop_requested_.store(true);   // control thread stops and reports
    }
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
    negotiated_rate_.store(0);
    negotiated_channels_.store(0);
    stop_requested_.store(false);
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

}  // namespace homeguardian

#endif  // HOMEGUARDIAN_ANDROID
