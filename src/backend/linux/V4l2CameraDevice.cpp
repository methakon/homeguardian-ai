#include "backend/linux/V4l2CameraDevice.h"

#include "core/BoundedFeatures.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/videodev2.h>

namespace homeguardian {

namespace {

// ioctl that retries on EINTR: a signal must not look like a device failure.
int xioctl(int fd, unsigned long request, void* arg) {
    int rc = 0;
    do {
        rc = ::ioctl(fd, request, arg);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

std::string fourcc_to_string(uint32_t f) {
    std::string s(4, ' ');
    s[0] = static_cast<char>(f & 0xFF);
    s[1] = static_cast<char>((f >> 8) & 0xFF);
    s[2] = static_cast<char>((f >> 16) & 0xFF);
    s[3] = static_cast<char>((f >> 24) & 0xFF);
    return s;
}

int64_t monotonic_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

V4l2CameraDevice::V4l2CameraDevice() : V4l2CameraDevice(Config{}) {}

V4l2CameraDevice::V4l2CameraDevice(Config cfg) : cfg_(cfg) {
    if (cfg_.buffer_count < 2) cfg_.buffer_count = 2;
    if (cfg_.buffer_count > 8) cfg_.buffer_count = 8;      // bounded by decision, not by hope
    if (cfg_.select_timeout_ms <= 0) cfg_.select_timeout_ms = 2000;
}

V4l2CameraDevice::~V4l2CameraDevice() {
    // Deterministic cleanup: a device is never left streaming, and never left open.
    close();
}

void V4l2CameraDevice::authorize_capture(bool allowed) noexcept {
    authorized_.store(allowed);
    if (!allowed && state_ == DeviceState::Capturing) stop();
}

void V4l2CameraDevice::initialize() {
    if (state_ == DeviceState::Capturing) throw std::runtime_error("already capturing");
    if (state_ == DeviceState::Error) throw std::runtime_error("device is in an error state");

    fd_ = ::open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        state_ = DeviceState::Error;
        throw std::runtime_error("cannot open " + cfg_.device + ": " + std::strerror(errno));
    }

    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error(cfg_.device + " is not a V4L2 device");
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                    : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error(std::string("no video capture capability on ") + cfg_.device);
    }
    if ((caps & V4L2_CAP_STREAMING) == 0) {
        // The metadata node the UVC driver exposes looks like a camera until this check.
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error(std::string("streaming not supported on ") + cfg_.device +
                                 " (a metadata-only V4L2 node cannot be captured from)");
    }

    // Prefer the requested format; otherwise take the first one the device offers and say so
    // in the negotiated summary. Nothing is converted either way.
    bool requested_available = false;
    uint32_t fallback = 0;
    v4l2_fmtdesc f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (f.index = 0; xioctl(fd_, VIDIOC_ENUM_FMT, &f) == 0; ++f.index) {
        if (f.pixelformat == cfg_.pixel_format) requested_available = true;
        if (fallback == 0) fallback = f.pixelformat;
    }
    const uint32_t chosen = requested_available ? cfg_.pixel_format : fallback;
    if (chosen == 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error(std::string("no capture format offered by ") + cfg_.device);
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg_.width;
    fmt.fmt.pix.height = cfg_.height;
    fmt.fmt.pix.pixelformat = chosen;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error(std::string("the device refused the requested format on ") +
                                 cfg_.device);
    }
    // Read back what the driver ACTUALLY negotiated: V4L2 is allowed to adjust it, and a
    // frame whose geometry we merely assumed is a frame we would misread.
    v4l2_format got{};
    got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_G_FMT, &got) < 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error("cannot read back the negotiated format");
    }
    negotiated_width_ = got.fmt.pix.width;
    negotiated_height_ = got.fmt.pix.height;
    negotiated_format_ = got.fmt.pix.pixelformat;
    bytes_per_line_ = got.fmt.pix.bytesperline > 0 ? got.fmt.pix.bytesperline
                                                    : negotiated_width_ * 2;

    // The bounded-feature extractor refuses anything above this, so refuse it here rather
    // than capture frames that can never be analysed.
    const size_t pixels = static_cast<size_t>(negotiated_width_) * negotiated_height_;
    if (pixels == 0 || pixels > FeatureBounds::kMaxPixels) {
        ::close(fd_);
        fd_ = -1;
        state_ = DeviceState::Error;
        throw std::runtime_error("negotiated geometry " + std::to_string(negotiated_width_) +
                                 "x" + std::to_string(negotiated_height_) +
                                 " is outside the bounded feature limits (max " +
                                 std::to_string(FeatureBounds::kMaxPixels) + " pixels)");
    }

    char summary[256];
    std::snprintf(summary, sizeof(summary), "driver=%s card=%s bus=%s %ux%u %s stride=%u",
                  reinterpret_cast<const char*>(cap.driver),
                  reinterpret_cast<const char*>(cap.card),
                  reinterpret_cast<const char*>(cap.bus_info), negotiated_width_,
                  negotiated_height_, fourcc_to_string(negotiated_format_).c_str(),
                  bytes_per_line_);
    negotiated_ = summary;
    if (!requested_available) {
        negotiated_ += " (requested format unavailable; using the first offered)";
    }
    state_ = DeviceState::Initialized;
}

void V4l2CameraDevice::start() {
    if (state_ == DeviceState::Capturing) return;
    if (state_ == DeviceState::Idle) {
        throw std::runtime_error("start() before initialize()");
    }
    if (state_ == DeviceState::Error) throw std::runtime_error("device is in an error state");
    if (!authorized_.load()) {
        // The gate, in the code path, not in the documentation.
        throw std::runtime_error("capture is not authorized: the operator must authorize it");
    }

    v4l2_requestbuffers req{};
    req.count = cfg_.buffer_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        state_ = DeviceState::Error;
        throw std::runtime_error("VIDIOC_REQBUFS failed: " + std::string(std::strerror(errno)));
    }
    if (req.count < 2) {
        state_ = DeviceState::Error;
        throw std::runtime_error("the driver granted fewer than two buffers");
    }

    buffers_.assign(req.count, Buffer{});
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            state_ = DeviceState::Error;
            release_buffers();
            throw std::runtime_error("VIDIOC_QUERYBUF failed");
        }
        buffers_[i].length = buf.length;
        buffers_[i].start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                                   buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            buffers_[i].start = nullptr;
            state_ = DeviceState::Error;
            release_buffers();
            throw std::runtime_error("mmap of a capture buffer failed");
        }
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            state_ = DeviceState::Error;
            release_buffers();
            throw std::runtime_error("VIDIOC_QBUF failed");
        }
        ++queued_;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        state_ = DeviceState::Error;
        release_buffers();
        throw std::runtime_error("VIDIOC_STREAMON failed: " + std::string(std::strerror(errno)));
    }
    state_ = DeviceState::Capturing;
}

bool V4l2CameraDevice::next_luma(LumaFrame* out, std::string* reason) {
    auto refuse = [reason](const std::string& why) {
        if (reason != nullptr) *reason = why;
        return false;
    };
    if (out == nullptr) return refuse("null output");
    if (state_ != DeviceState::Capturing) return refuse("device is not capturing");
    if (!authorized_.load()) return refuse("capture authorization was revoked");

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv{};
    tv.tv_sec = cfg_.select_timeout_ms / 1000;
    tv.tv_usec = (cfg_.select_timeout_ms % 1000) * 1000;
    const int sel = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
    if (sel == 0) {
        ++stalls_;
        return refuse("no frame within " + std::to_string(cfg_.select_timeout_ms) +
                      " ms: the device stalled or was removed");
    }
    if (sel < 0) {
        if (errno == EINTR) return refuse("frame wait interrupted");
        state_ = DeviceState::Error;
        return refuse(std::string("select failed: ") + std::strerror(errno));
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            ++stalls_;
            return refuse("no frame dequeued this time");
        }
        state_ = DeviceState::Error;
        return refuse(std::string("VIDIOC_DQBUF failed: ") + std::strerror(errno));
    }
    if (buf.index >= buffers_.size()) {
        state_ = DeviceState::Error;
        return refuse("the driver returned an out-of-range buffer index");
    }
    if (queued_ > 0) --queued_;

    // Sequence gaps mean frames were dropped by the pipeline, which the caller must be able
    // to see: a gap is evidence, not something to smooth over.
    if (have_sequence_ && buf.sequence > last_sequence_ + 1) {
        dropped_ += buf.sequence - last_sequence_ - 1;
    }
    last_sequence_ = buf.sequence;
    have_sequence_ = true;

    const Buffer& b = buffers_[buf.index];
    const size_t pixels = static_cast<size_t>(negotiated_width_) * negotiated_height_;
    const uint8_t* src = static_cast<const uint8_t*>(b.start);
    const size_t needed = static_cast<size_t>(negotiated_height_ - 1) * bytes_per_line_ +
                          static_cast<size_t>(negotiated_width_ - 1) * 2 + 2;
    if (b.start == nullptr || b.length < needed) {
        // Requeue before refusing, so one short buffer cannot deadlock the stream.
        (void)xioctl(fd_, VIDIOC_QBUF, &buf);
        ++queued_;
        return refuse("a capture buffer is shorter than the negotiated geometry");
    }

    out->width = negotiated_width_;
    out->height = negotiated_height_;
    out->sequence = buf.sequence;
    out->captured_at_ms = monotonic_ms();
    out->luma.resize(pixels);

    if (negotiated_format_ == V4L2_PIX_FMT_YUYV) {
        // YUYV interleaves: Y0 U Y1 V. The luma plane is every second byte, and the chroma
        // bytes are never copied - there is no colour conversion in this file.
        for (uint32_t row = 0; row < negotiated_height_; ++row) {
            const uint8_t* src_row = src + static_cast<size_t>(row) * bytes_per_line_;
            uint8_t* dst_row = out->luma.data() + static_cast<size_t>(row) * negotiated_width_;
            for (uint32_t col = 0; col < negotiated_width_; ++col) {
                dst_row[col] = src_row[static_cast<size_t>(col) * 2];
            }
        }
    } else if (negotiated_format_ == V4L2_PIX_FMT_GREY) {
        for (uint32_t row = 0; row < negotiated_height_; ++row) {
            std::memcpy(out->luma.data() + static_cast<size_t>(row) * negotiated_width_,
                        src + static_cast<size_t>(row) * bytes_per_line_, negotiated_width_);
        }
    } else {
        (void)xioctl(fd_, VIDIOC_QBUF, &buf);
        ++queued_;
        return refuse("unsupported pixel format " + fourcc_to_string(negotiated_format_) +
                      ": this build extracts luma from YUYV and GREY only");
    }

    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        state_ = DeviceState::Error;
        return refuse("VIDIOC_QBUF (requeue) failed");
    }
    ++queued_;
    if (reason != nullptr) reason->clear();
    return true;
}

void V4l2CameraDevice::stop() noexcept {
    if (state_ == DeviceState::Capturing && fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    release_buffers();
    if (state_ == DeviceState::Capturing) state_ = DeviceState::Stopped;
}

void V4l2CameraDevice::close() noexcept {
    stop();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (state_ != DeviceState::Error) state_ = DeviceState::Idle;
}

void V4l2CameraDevice::fail() noexcept {
    state_ = DeviceState::Error;
    release_buffers();
}

void V4l2CameraDevice::release_buffers() noexcept {
    for (auto& b : buffers_) {
        if (b.start != nullptr) {
            ::munmap(b.start, b.length);
            b.start = nullptr;
        }
    }
    buffers_.clear();
    queued_ = 0;
    have_sequence_ = false;
    if (fd_ >= 0) {
        // Tell the driver to release its own buffers too; without this a second start on the
        // same fd can be refused for no visible reason.
        v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        (void)xioctl(fd_, VIDIOC_REQBUFS, &req);
    }
}

}  // namespace homeguardian
