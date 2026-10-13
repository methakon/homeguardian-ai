#include "CaptureAccounting.h"

namespace homeguardian {

namespace {
// Bounded, non-sensitive explanation string.
std::string detail(const std::string& what) {
    return what.size() > 96 ? what.substr(0, 96) : what;
}
}  // namespace

CaptureAccountant::Decision CaptureAccountant::accept() {
    counters_.last_drop = DropReason::none;
    counters_.last_drop_detail.clear();
    Decision d;
    d.authorized = true;
    return d;
}

CaptureAccountant::Decision CaptureAccountant::account_frame(uint32_t width, uint32_t height,
                                                             size_t bytes, int64_t timestamp_ms,
                                                             bool authorized, int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);

    counters_.sat_add(counters_.frames_seen, 1);

    if (!authorized) {
        counters_.sat_add(counters_.frames_dropped, 1);
        counters_.last_drop = DropReason::consent_denied;
        counters_.last_drop_detail =
            detail("not authorized at delivery (consent/permission/capture state)");
        Decision d;
        d.reason = DropReason::consent_denied;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (!video_running_) {
        counters_.sat_add(counters_.frames_dropped, 1);
        counters_.last_drop = DropReason::not_capturing;
        counters_.last_drop_detail = detail("frame arrived while capture is not running");
        Decision d;
        d.reason = DropReason::not_capturing;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (width < limits_.min_width || width > limits_.max_width ||
        height < limits_.min_height || height > limits_.max_height) {
        counters_.sat_add(counters_.frames_dropped, 1);
        counters_.last_drop = DropReason::bad_dimensions;
        counters_.last_drop_detail = detail("frame dimensions outside configured bounds");
        Decision d;
        d.reason = DropReason::bad_dimensions;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (bytes == 0 || bytes > limits_.max_frame_bytes) {
        counters_.sat_add(counters_.frames_dropped, 1);
        counters_.last_drop = DropReason::oversize;
        counters_.last_drop_detail = detail("frame payload outside configured bounds");
        Decision d;
        d.reason = DropReason::oversize;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (timestamp_ms <= 0 || timestamp_ms > now_ms + limits_.future_tolerance_ms) {
        counters_.sat_add(counters_.frames_dropped, 1);
        counters_.last_drop = DropReason::bad_timestamp;
        counters_.last_drop_detail = detail("frame timestamp is not plausible");
        Decision d;
        d.reason = DropReason::bad_timestamp;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (counters_.last_frame_ms != 0 && timestamp_ms < counters_.last_frame_ms) {
        counters_.sat_add(counters_.timestamp_regressions, 1);
        // A backwards jump is recorded but the frame is still accepted: camera
        // timestamps can legitimately tie or regress slightly across a restart
        // of the preview. It is counted so the UI/health view can see it.
    }

    counters_.sat_add(counters_.frames_authorized, 1);
    counters_.sat_add(counters_.frame_bytes, static_cast<uint64_t>(bytes));
    if (counters_.first_frame_ms == 0) counters_.first_frame_ms = timestamp_ms;
    counters_.last_frame_ms = timestamp_ms;
    return accept();
}

CaptureAccountant::Decision CaptureAccountant::account_audio(uint32_t sample_rate, uint32_t channels,
                                                             uint32_t sample_frames, size_t bytes,
                                                             int64_t timestamp_ms, bool authorized,
                                                             int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);

    counters_.sat_add(counters_.audio_buffers_seen, 1);

    if (!authorized) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::consent_denied;
        counters_.last_drop_detail =
            detail("audio buffer not authorized at delivery (consent/permission/capture state)");
        Decision d;
        d.reason = DropReason::consent_denied;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (!audio_running_) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::not_capturing;
        counters_.last_drop_detail = detail("audio buffer arrived while capture is not running");
        Decision d;
        d.reason = DropReason::not_capturing;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (sample_rate < limits_.min_sample_rate || sample_rate > limits_.max_sample_rate) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::bad_sample_rate;
        counters_.last_drop_detail = detail("audio sample rate outside configured bounds");
        Decision d;
        d.reason = DropReason::bad_sample_rate;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (channels == 0 || channels > limits_.max_channels) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::bad_channels;
        counters_.last_drop_detail = detail("audio channel count outside configured bounds");
        Decision d;
        d.reason = DropReason::bad_channels;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (bytes == 0 || bytes > limits_.max_audio_bytes_per_buffer || sample_frames == 0) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::oversize;
        counters_.last_drop_detail = detail("audio buffer size outside configured bounds");
        Decision d;
        d.reason = DropReason::oversize;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (timestamp_ms <= 0 || timestamp_ms > now_ms + limits_.future_tolerance_ms) {
        counters_.sat_add(counters_.audio_buffers_dropped, 1);
        counters_.last_drop = DropReason::bad_timestamp;
        counters_.last_drop_detail = detail("audio timestamp is not plausible");
        Decision d;
        d.reason = DropReason::bad_timestamp;
        d.detail = counters_.last_drop_detail;
        return d;
    }
    if (counters_.last_audio_ms != 0 && timestamp_ms < counters_.last_audio_ms) {
        counters_.sat_add(counters_.timestamp_regressions, 1);
    }

    counters_.sat_add(counters_.audio_buffers_authorized, 1);
    counters_.sat_add(counters_.audio_frames, sample_frames);
    counters_.sat_add(counters_.audio_bytes, static_cast<uint64_t>(bytes));
    if (counters_.first_audio_ms == 0) counters_.first_audio_ms = timestamp_ms;
    counters_.last_audio_ms = timestamp_ms;
    return accept();
}

void CaptureAccountant::note_capture_started(bool video, int64_t now_ms) {
    std::lock_guard<std::mutex> lk(m_);

    if (video) {
        video_running_ = true;
        (void)now_ms;
    } else {
        audio_running_ = true;
        (void)now_ms;
    }
}

void CaptureAccountant::note_capture_stopped() {
    std::lock_guard<std::mutex> lk(m_);

    video_running_ = false;
    audio_running_ = false;
}

void CaptureAccountant::note_xrun() {
    std::lock_guard<std::mutex> lk(m_);
    counters_.sat_add(counters_.xruns, 1);
}

void CaptureAccountant::reset() {
    std::lock_guard<std::mutex> lk(m_);

    counters_ = CaptureCounters{};
    video_running_ = false;
    audio_running_ = false;
}

}  // namespace homeguardian
