#pragma once

// REAL camera capture on Ubuntu, behind the existing IMediaDevice contract.
//
// WHY V4L2 AND NOT A LIBRARY
// --------------------------
// Every other Linux capture path in this project hits a missing dependency: the audio
// backend needs ALSA headers (absent) and falls back to a subprocess, and there is no
// OpenCV here at all. V4L2 needs NO library: /usr/include/linux/videodev2.h is part of the
// kernel UAPI that is already installed, and capture is a set of ioctls on a device node.
// So this is real frames from the real device, with no new dependency, no root, and no
// package installation.
//
// WHAT THIS IS NOT
// ----------------
//   * NOT video recording, streaming or storage. No frame is written to disk, kept in a
//     queue beyond `buffer_count`, or transmitted. A frame exists only long enough for the
//     caller to derive scalars from it, which is the whole privacy argument.
//   * NOT interpretation. It yields luma planes; the feature extractor turns them into
//     bounded scalars. Nothing here recognises a person, an animal, a face or an event.
//   * NOT continuous or unattended: start() REFUSES unless the operator authorized capture
//     explicitly (see authorize_capture), so no code path can open the camera merely
//     because an object was constructed.
//
// SAFETY AND LIFECYCLE
// --------------------
//   * Idle -> initialize() -> Initialized -> start() -> Capturing -> stop() -> Stopped
//     -> close() -> Idle, following IMediaDevice exactly; the destructor guarantees
//     stop()+close(), so a device is never left streaming.
//   * Buffers are mmap'd and BOUNDED in number; a frame that the caller does not consume
//     is dropped rather than queued, so a stalled consumer cannot grow memory.
//   * Device removal appears as a capture timeout or an ioctl failure, and the device goes
//     to Error with a reason rather than pretending to deliver a black frame.
//   * authorize_capture(false) stops an active session immediately.

#include "core/IMediaDevice.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {

// One captured frame, reduced to what the feature layer needs: a contiguous luma plane and
// its geometry. The interleaved chroma a YUYV frame carries is never copied.
struct LumaFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sequence = 0;        // driver sequence number, for gap detection
    int64_t captured_at_ms = 0;   // monotonic-ish receive time
    std::vector<uint8_t> luma;    // width*height, row-major
};

class V4l2CameraDevice : public IMediaDevice {
public:
    struct Config {
        std::string device = "/dev/video0";
        uint32_t width = 640;
        uint32_t height = 480;
        // YUYV is the format the host camera offers natively; asking for it avoids any
        // conversion, which is what keeps this dependency-free.
        uint32_t pixel_format = 0x56595559;  // V4L2_PIX_FMT_YUYV ('YUYV')
        uint32_t buffer_count = 4;           // bounded; more is not more useful here
        int32_t select_timeout_ms = 2000;    // a stalled device must not hang the caller
    };

    // Two constructors rather than one defaulted argument: a nested aggregate with default
    // member initializers cannot be spelled `= {}` inside this class body.
    V4l2CameraDevice();
    explicit V4l2CameraDevice(Config cfg);
    ~V4l2CameraDevice() override;

    V4l2CameraDevice(const V4l2CameraDevice&) = delete;
    V4l2CameraDevice& operator=(const V4l2CameraDevice&) = delete;

    // ---- IMediaDevice ----
    DeviceKind kind() const override { return DeviceKind::camera; }
    void initialize() override;
    void start() override;
    void stop() noexcept override;
    void close() noexcept override;
    void fail() noexcept override;
    DeviceState state() const override { return state_; }
    std::string name() const override { return cfg_.device; }

    // The operator's authorization. Capture is REFUSED without it, and revoking it stops an
    // active session immediately. Mirrors the audio backend's gate so the two physical
    // paths cannot drift apart on the one rule that matters.
    void authorize_capture(bool allowed) noexcept;
    bool capture_authorized() const noexcept { return authorized_.load(); }

    // Identity of what was actually negotiated, for evidence. Empty until initialize().
    std::string negotiated_summary() const { return negotiated_; }
    const Config& config() const { return cfg_; }

    // Grab one frame's luma plane. Returns false with `reason` on timeout, a stalled
    // device, a state error, or an oversized frame. Never allocates for an unbounded frame:
    // a geometry exceeding FeatureBounds::kMaxPixels is refused.
    bool next_luma(LumaFrame* out, std::string* reason);

    // Evidence counters: frames the driver dropped (sequence gaps) and waits that timed out.
    uint64_t dropped_frames() const { return dropped_; }
    uint32_t stalls() const { return stalls_; }

private:
    void release_buffers() noexcept;

    Config cfg_{};
    DeviceState state_ = DeviceState::Idle;
    std::string negotiated_{};
    std::atomic<bool> authorized_{false};

    int fd_ = -1;
    uint32_t negotiated_width_ = 0;
    uint32_t negotiated_height_ = 0;
    uint32_t negotiated_format_ = 0;
    uint32_t bytes_per_line_ = 0;
    // mmap'd capture buffer. Defined here, not in the .cpp: a vector of an incomplete type
    // cannot be instantiated in this class body.
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    std::vector<Buffer> buffers_{};
    uint32_t queued_ = 0;
    uint32_t last_sequence_ = 0;
    uint64_t dropped_ = 0;
    uint32_t stalls_ = 0;
    bool have_sequence_ = false;
};

}  // namespace homeguardian
