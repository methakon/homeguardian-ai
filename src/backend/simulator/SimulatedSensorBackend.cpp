#include "SimulatedSensorBackend.h"

#include <cstring>
#include <stdexcept>

namespace homeguardian {

SimulatedSensorBackend::SimulatedSensorBackend(DeviceKind kind,
                                               std::string name,
                                               int width,
                                               int height,
                                               int sample_rate,
                                               int samples_per_buffer,
                                               uint64_t seed)
    : kind_(kind),
      name_(std::move(name)),
      width_(width),
      height_(height),
      sample_rate_(sample_rate),
      samples_per_buffer_(samples_per_buffer),
      rng_state_(seed ? seed : 0x5EED) {}

DeviceState SimulatedSensorBackend::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
}

void SimulatedSensorBackend::initialize() {
    std::lock_guard<std::mutex> lk(m_);
    if (has_error_) {
        throw std::runtime_error("simulated: in error state, close before re-init");
    }
    if (state_ == DeviceState::Idle) {
        state_ = DeviceState::Initialized;
        init_count_++;
        return;
    }
    if (state_ == DeviceState::Initialized) {
        throw std::runtime_error("simulated: already initialized");
    }
    // Capturing/Stopped must be stopped/closed first.
    throw std::runtime_error("simulated: initialize requires Idle state");
}

void SimulatedSensorBackend::start() {
    std::lock_guard<std::mutex> lk(m_);
    if (has_error_) {
        throw std::runtime_error("simulated: cannot start in error state");
    }
    if (state_ != DeviceState::Initialized) {
        throw std::runtime_error("simulated: start requires Initialized state");
    }
    state_ = DeviceState::Capturing;
    start_count_++;
}

void SimulatedSensorBackend::stop() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == DeviceState::Capturing) {
        state_ = DeviceState::Stopped;
        stop_count_++;
    }
    // Safe no-op from Idle/Stopped/Initialized/Error.
}

void SimulatedSensorBackend::close() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == DeviceState::Capturing) {
        stop_count_++;
    }
    state_ = DeviceState::Idle;
    has_error_ = false;
    released_ = true;
    close_count_++;
    ring_.clear();  // release acquisition buffer on close
}

void SimulatedSensorBackend::fail() noexcept { inject_error(); }

void SimulatedSensorBackend::inject_error() {
    std::lock_guard<std::mutex> lk(m_);
    // Force out of capture and mark error, releasing capture resources.
    if (state_ == DeviceState::Capturing) {
        stop_count_++;
    }
    state_ = DeviceState::Error;
    has_error_ = true;
}

void SimulatedSensorBackend::recover() {
    std::lock_guard<std::mutex> lk(m_);
    // Recovery path used by error-recovery tests: close first (deterministic
    // cleanup), then allow re-initialization.
    if (state_ == DeviceState::Capturing) {
        stop_count_++;
    }
    state_ = DeviceState::Idle;
    has_error_ = false;
    close_count_++;
    ring_.clear();
}

bool SimulatedSensorBackend::has_error() const {
    std::lock_guard<std::mutex> lk(m_);
    return has_error_;
}

uint64_t SimulatedSensorBackend::next_rand() {
    // Deterministic 64-bit LCG (constants from Knuth/MMIX). Fully reproducible
    // for a given seed and call sequence; no dependence on system state.
    rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng_state_;
}

void SimulatedSensorBackend::fill_camera_frame(std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(width_) * height_);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>(next_rand() & 0xFF);
    }
}

void SimulatedSensorBackend::fill_audio_buffer(std::vector<uint8_t>& out) {
    // 16-bit PCM mono.
    out.resize(static_cast<size_t>(samples_per_buffer_) * 2);
    for (int s = 0; s < samples_per_buffer_; ++s) {
        uint16_t v = static_cast<uint16_t>(next_rand() & 0xFFFF);
        std::memcpy(&out[s * 2], &v, 2);
    }
}

std::vector<uint8_t> SimulatedSensorBackend::generate_frame() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<uint8_t> out;
    if (kind_ == DeviceKind::camera) {
        fill_camera_frame(out);
    } else {
        fill_audio_buffer(out);
    }
    sequence_++;
    return out;
}

void SimulatedSensorBackend::record_delivered(std::vector<uint8_t> payload) {
    // Only records the in-memory payload into the bounded ring. The caller
    // must have already passed authorize_delivery() at the delivery boundary.
    // This never writes to disk, never opens a device, never uploads.
    ring_.push(std::move(payload));
}

} // namespace homeguardian
