#include "SimulatedSensorBackend.h"

#include <cstring>
#include <cmath>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace homeguardian {

SimulatedSensorBackend::SimulatedSensorBackend(DeviceKind kind,
                                               std::string name,
                                               Config cfg)
    : kind_(kind),
      name_(std::move(name)),
      width_(cfg.width),
      height_(cfg.height),
      frame_rate_(cfg.frame_rate > 0 ? cfg.frame_rate : 0.0),
      sample_rate_(cfg.sample_rate),
      samples_per_buffer_(cfg.samples_per_buffer),
      channels_(cfg.channels >= 1 ? cfg.channels : 1),
      signal_(cfg.signal),
      tone_hz_(cfg.tone_hz),
      rng_state_(cfg.seed ? cfg.seed : 0x5EED),
      ring_(cfg.ring_capacity > 0 ? cfg.ring_capacity : 256) {}

// Positional convenience overload: preserves existing call sites. Delegates to
// the Config constructor so all state is initialized in exactly one place.
SimulatedSensorBackend::SimulatedSensorBackend(DeviceKind kind,
                                               std::string name,
                                               int width,
                                               int height,
                                               int sample_rate,
                                               int samples_per_buffer,
                                               uint64_t seed)
    : SimulatedSensorBackend(kind, std::move(name),
                             Config{width, height, 0.0, sample_rate,
                                    samples_per_buffer, 1, TestSignal::noise,
                                    440.0, 256, seed}) {}

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
    playing_ = false;  // stop any simulated playback on stop
    // Safe no-op from Idle/Stopped/Initialized/Error.
}

void SimulatedSensorBackend::close() noexcept {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == DeviceState::Capturing) {
        stop_count_++;
    }
    state_ = DeviceState::Idle;
    has_error_ = false;
    playback_error_ = false;
    released_ = true;
    playing_ = false;
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
    playing_ = false;
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
    // 16-bit PCM, `channels_` interleaved (1 = mono, 2 = stereo).
    const size_t total_samples = static_cast<size_t>(samples_per_buffer_) * channels_;
    out.resize(total_samples * 2);
    for (size_t s = 0; s < total_samples; ++s) {
        int16_t v = 0;
        switch (signal_) {
            case TestSignal::silence:
                v = 0;
                break;
            case TestSignal::sine: {
                // Continuous phase across the whole stream so the tone is
                // smooth and reproducible: sample index advances the phase.
                double t = static_cast<double>(
                    sequence_ * samples_per_buffer_ * channels_ + s);
                double phase = 2.0 * M_PI * tone_hz_ * t / static_cast<double>(sample_rate_);
                v = static_cast<int16_t>(12000.0 * std::sin(phase));
                break;
            }
            case TestSignal::square: {
                double t = static_cast<double>(
                    sequence_ * samples_per_buffer_ * channels_ + s);
                double phase = tone_hz_ * t / static_cast<double>(sample_rate_);
                v = static_cast<int16_t>((std::fmod(phase, 1.0) < 0.5) ? 12000 : -12000);
                break;
            }
            case TestSignal::noise:
            default:
                v = static_cast<int16_t>(next_rand() & 0xFFFF);
                break;
        }
        std::memcpy(&out[s * 2], &v, 2);
    }
}

void SimulatedSensorBackend::pace_frame() {
    // Enforce frame_rate_ (frames/sec) using wall-clock sleep between frames.
    // A rate of 0 means unpaced. Pacing is best-effort; it never affects the
    // bytes produced (still fully deterministic) and is only for tests that
    // want realistic timing.
    if (frame_rate_ <= 0.0) return;
    static thread_local std::chrono::steady_clock::time_point last{};
    auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0) {
        auto interval = std::chrono::duration<double>(1.0 / frame_rate_);
        auto next_due =
            last + std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        if (now < next_due) {
            std::this_thread::sleep_for(next_due - now);
        }
    }
    last = std::chrono::steady_clock::now();
}

std::vector<uint8_t> SimulatedSensorBackend::generate_frame() {
    std::lock_guard<std::mutex> lk(m_);
    pace_frame();
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

size_t SimulatedSensorBackend::audio_frame_bytes() const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<size_t>(samples_per_buffer_) * channels_ * 2;
}

bool SimulatedSensorBackend::play(const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lk(m_);
    // Playback models the audio OUTPUT path only (no capture, so no consent
    // delivery gate). It requires an initialized device and no playback error.
    if (playback_error_) return false;
    if (state_ == DeviceState::Error) return false;
    if (state_ == DeviceState::Idle) return false;  // not initialized
    playing_ = true;
    played_count_++;
    // The payload is consumed in memory only; never written or uploaded.
    (void)payload;
    return true;
}

void SimulatedSensorBackend::inject_playback_error() {
    std::lock_guard<std::mutex> lk(m_);
    playback_error_ = true;
    playing_ = false;
}

bool SimulatedSensorBackend::playing() const {
    std::lock_guard<std::mutex> lk(m_);
    return playing_;
}

size_t SimulatedSensorBackend::played_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return played_count_;
}

} // namespace homeguardian
