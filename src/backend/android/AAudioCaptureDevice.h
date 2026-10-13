#pragma once

// Android NDK audio-capture backend implementing IMediaDevice (milestone D-02).
//
// Uses AAudio (API 26+). Compiled only for Android targets
// (HOMEGUARDIAN_ANDROID). It contains NO speech recognition, no voiceprints, no
// speaker identity and no emotion inference of any kind: it copies bounded PCM
// buffers, validates and counts them through the shared CaptureAccountant, and
// drops anything the consent/permission check refuses. Raw audio is never
// written to disk or uploaded.
//
// DELIVERY CONTRACT (the reason this class exists):
//   * initialize() is fail-closed: without set_device_confirmed(true) it refuses
//     to open a real stream and reports Error. That flag is set only after the
//     operator explicitly started acquisition AND the OS granted RECORD_AUDIO
//     (see the JNI layer), never on library load or app launch.
//   * The AAudio data callback runs on a driver thread. For EVERY buffer it
//     asks the injected authorize callback (which re-evaluates the consent
//     gate) whether that buffer may be delivered, and hands the answer to the
//     accountant together with the stream's real rate/channel/format values. A
//     refused buffer is counted and dropped, never queued.
//   * A refusal caused by consent/permission/device state raises
//     stop_requested(), which the control thread observes and turns into a real
//     stop+close. The callback never calls requestStop() itself (not safe from
//     the driver thread).
//
// Earlier revisions of this backend opened a stream but registered no data
// callback, so no PCM was ever read. D-02 adds the callback and the accounting.

#include "IMediaDevice.h"

#ifdef HOMEGUARDIAN_ANDROID

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace homeguardian {

class CaptureAccountant;   // core/CaptureAccounting.h

class AAudioCaptureDeviceImpl : public IMediaDevice {
public:
    // Bounded, low-resource defaults suited to an older 1 GB phone. 16 kHz mono
    // 16-bit is a deliberate ceiling, not a speech pipeline.
    struct Config {
        int32_t sample_rate = 16000;
        int32_t channels = 1;
        int32_t frames_per_callback = 512;   // ~32 ms at 16 kHz
    };

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
    // available AND the operator explicitly started acquisition AND the OS
    // granted RECORD_AUDIO. Until then initialize() fails closed. Default false.
    void set_device_confirmed(bool confirmed) { device_confirmed_ = confirmed; }

    void set_config(const Config& cfg) { config_ = cfg; }

    // Non-owning. Validates/counts every delivered buffer.
    void set_accountant(CaptureAccountant* accountant) { accountant_ = accountant; }

    // Non-owning. Called for EVERY buffer; must return true only if this buffer
    // may be delivered right now (consent + permission + capture state). An
    // unset callback denies everything (fail closed).
    void set_authorize(std::function<bool()> authorize) { authorize_ = std::move(authorize); }

    // Observed by the control thread: true once a buffer was refused for a
    // consent/permission/device reason, i.e. capture must be stopped.
    bool stop_requested() const { return stop_requested_.load(); }
    void clear_stop_requested() { stop_requested_.store(false); }

    // Stream facts for the UI/health view (0 until a stream is open).
    int32_t negotiated_sample_rate() const { return negotiated_rate_.load(); }
    int32_t negotiated_channels() const { return negotiated_channels_.load(); }

private:
    static aaudio_data_callback_result_t data_callback(AAudioStream* stream, void* user_data,
                                                       void* audio_data, int32_t num_frames);
    static void error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error);

    DeviceState state_ = DeviceState::Idle;
    bool device_confirmed_ = false;
    Config config_{};
    AAudioStream* stream_ = nullptr;
    CaptureAccountant* accountant_ = nullptr;      // non-owning
    std::function<bool()> authorize_;              // empty -> deny
    std::atomic<bool> stop_requested_{false};
    std::atomic<int32_t> negotiated_rate_{0};
    std::atomic<int32_t> negotiated_channels_{0};
};

}  // namespace homeguardian

#endif  // HOMEGUARDIAN_ANDROID
