#pragma once

// CaptureAccounting - the bounded, host-testable contract that every real
// acquisition callback must satisfy (milestone D-02).
//
// WHY THIS EXISTS: a phone node hands the app frames and PCM buffers on driver
// threads. Two things must be true for every one of them, and both must be
// provable without a phone attached:
//
//   1. It is AUTHORIZED at the moment of delivery (consent still granted, OS
//      permission still held, capture actually running). A payload that is not
//      authorized is dropped and never handed on.
//   2. It is BOUNDED and SANE. Dimensions, rate, channel count and payload size
//      are validated against configured limits, timestamps must not jump
//      forwards past a tolerance, and nothing accumulates without a ceiling.
//
// The accountant is pure and deterministic: no threads, no I/O, no globals. The
// Android backends (Camera1 preview callback, AAudio data callback) call it on
// their driver threads; the host tests drive it directly. Counters saturate
// instead of overflowing, so a long-running node cannot wrap a counter.
//
// THIS CLASS DOES NOT INTERPRET MEDIA. It never decodes, never stores payload
// bytes, and derives no features. Motion/audio feature extraction is milestone
// D-05; nothing here may claim to recognise a person, speech, a fall or an
// emergency.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace homeguardian {

// Why a payload was refused. Reported to the UI and counted; never silently
// swallowed.
enum class DropReason {
    none = 0,
    not_capturing,
    consent_denied,
    permission_revoked,
    oversize,
    bad_dimensions,
    bad_sample_rate,
    bad_channels,
    bad_timestamp,
    rate_limited
};

inline const char* drop_reason_to_string(DropReason r) {
    switch (r) {
        case DropReason::none: return "none";
        case DropReason::not_capturing: return "not_capturing";
        case DropReason::consent_denied: return "consent_denied";
        case DropReason::permission_revoked: return "permission_revoked";
        case DropReason::oversize: return "oversize";
        case DropReason::bad_dimensions: return "bad_dimensions";
        case DropReason::bad_sample_rate: return "bad_sample_rate";
        case DropReason::bad_channels: return "bad_channels";
        case DropReason::bad_timestamp: return "bad_timestamp";
        case DropReason::rate_limited: return "rate_limited";
    }
    return "unknown";
}

// Configuration ceilings. Defaults are chosen for an older 1 GB phone: small
// preview frames, one audio stream, bounded payloads.
struct CaptureLimits {
    // video
    uint32_t min_width = 64;
    uint32_t max_width = 1920;
    uint32_t min_height = 48;
    uint32_t max_height = 1080;
    size_t max_frame_bytes = 4u * 1024u * 1024u;   // 4 MiB per frame ceiling
    uint32_t max_fps = 30;
    // audio
    uint32_t min_sample_rate = 8000;
    uint32_t max_sample_rate = 48000;
    uint32_t max_channels = 2;
    size_t max_audio_bytes_per_buffer = 64u * 1024u;
    // accepted clock skew for a callback timestamp arriving "in the future"
    int64_t future_tolerance_ms = 2000;
};

// Saturating counters: a node that runs for weeks must not wrap.
struct CaptureCounters {
    uint64_t frames_seen = 0;
    uint64_t frames_authorized = 0;
    uint64_t frames_dropped = 0;
    uint64_t frame_bytes = 0;          // accepted only
    uint64_t audio_buffers_seen = 0;
    uint64_t audio_buffers_authorized = 0;
    uint64_t audio_buffers_dropped = 0;
    uint64_t audio_frames = 0;         // sample frames accepted
    uint64_t audio_bytes = 0;          // accepted only
    uint64_t xruns = 0;                // audio stream discontinuities
    uint64_t timestamp_regressions = 0;
    int64_t first_frame_ms = 0;        // 0 = none yet
    int64_t last_frame_ms = 0;
    int64_t first_audio_ms = 0;
    int64_t last_audio_ms = 0;
    // Last drop, for the UI ("why did the last payload not count?").
    DropReason last_drop = DropReason::none;
    std::string last_drop_detail;

    void sat_add(uint64_t& counter, uint64_t delta) {
        const uint64_t room = UINT64_MAX - counter;
        counter += (delta > room) ? room : delta;
    }
};

class CaptureAccountant {
public:
    struct Decision {
        bool authorized = false;      // payload may be handed on
        DropReason reason = DropReason::none;
        std::string detail;           // bounded, non-sensitive explanation
    };

    explicit CaptureAccountant(CaptureLimits limits = CaptureLimits{}) : limits_(limits) {}

    // A camera preview/analysis frame. `authorized` is the result of the
    // consent/permission check the backend performed for THIS frame.
    // `now_ms` is the backend clock reading used to sanity-check the timestamp.
    Decision account_frame(uint32_t width, uint32_t height, size_t bytes,
                          int64_t timestamp_ms, bool authorized, int64_t now_ms);

    // One PCM buffer: `sample_frames` sample frames across `channels`.
    Decision account_audio(uint32_t sample_rate, uint32_t channels,
                           uint32_t sample_frames, size_t bytes,
                           int64_t timestamp_ms, bool authorized, int64_t now_ms);

    void note_capture_started(bool video, int64_t now_ms);
    void note_capture_stopped();
    // Clear ONE kind only. Stopping the camera must not mark the microphone stopped
    // (the whole-kind reset above would report `audio=off` while audio is still running).
    void note_capture_stopped(bool video);
    void note_xrun();

    // A COPY, taken under the lock: driver threads update counters while the UI
    // thread reads them, so no reference to the live state is handed out.
    CaptureCounters counters() const {
        std::lock_guard<std::mutex> lk(m_);
        return counters_;
    }
    const CaptureLimits& limits() const { return limits_; }   // immutable after construction
    bool video_running() const { std::lock_guard<std::mutex> lk(m_); return video_running_; }
    bool audio_running() const { std::lock_guard<std::mutex> lk(m_); return audio_running_; }

    void reset();

private:
    Decision accept();

    mutable std::mutex m_;
    CaptureLimits limits_;
    CaptureCounters counters_;
    bool video_running_ = false;
    bool audio_running_ = false;
};

}  // namespace homeguardian
