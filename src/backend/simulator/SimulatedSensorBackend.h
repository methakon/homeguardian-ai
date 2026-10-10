#pragma once

// Deterministic simulated media backend for HomeGuardian AI.
//
// SIMULATOR-FIRST DEVELOPMENT: this backend generates synthetic, fully
// deterministic camera frames and microphone samples in-process, so the entire
// consent, lifecycle, delivery-boundary, bounded-buffer, and shutdown machinery
// can be exercised WITHOUT any real sensor. It sits behind the SAME IMediaDevice
// contract and the SAME ConsentGuardedDevice gate as the Android and Ubuntu
// backends, so tests here validate the shared safety core on real code paths.
//
// What this validates (host, deterministic):
//  - Device lifecycle (initialize/start/stop/close, idempotency).
//  - Consent denial / withdrawal / expiry preventing delivery.
//  - Delivery-boundary authorization (per frame/sample via authorize_delivery).
//  - Permission revocation stopping delivery.
//  - Bounded buffers (ring capacity; overflow drops oldest, never unbounded).
//  - Error injection and recovery; resource cleanup / shutdown.
//
// What this does NOT validate (requires Docker or real hardware):
//  - Actual V4L2 ioctl/format negotiation and real frame timing.
//  - Actual ALSA/PipeWire PCM streams, sample rates, latency.
//  - Real device enumeration via /dev/video* or a running sound server.
//  - Real OS permission revocation semantics.
// The SensorDiscovery enumeration + a real device open are the only parts that
// need hardware (or a loopback kernel module) — see docs for the matrix.
//
// SIMULATION SAFETY: this backend opens NO device file, makes NO syscalls to
// capture hardware, and produces NO media files. Frames are in-memory byte
// patterns; samples are in-memory PCM. Nothing is persisted or uploaded.

#include "core/IMediaDevice.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace homeguardian {

// A bounded FIFO of fixed-size payloads. When full, the oldest entry is
// dropped (overwrite-oldest). This models a bounded acquisition buffer and is
// the invariant under test: memory can never grow without bound.
class BoundedRing {
public:
    explicit BoundedRing(size_t capacity) : capacity_(capacity) {}

    void push(std::vector<uint8_t> payload) {
        std::lock_guard<std::mutex> lk(m_);
        while (buf_.size() >= capacity_) {
            buf_.erase(buf_.begin());  // drop oldest -> bounded
            dropped_++;
        }
        buf_.push_back(std::move(payload));
    }

    size_t size() const { std::lock_guard<std::mutex> lk(m_); return buf_.size(); }
    size_t dropped() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    size_t capacity() const { return capacity_; }

    // Clear on shutdown.
    void clear() { std::lock_guard<std::mutex> lk(m_); buf_.clear(); }

private:
    size_t capacity_;
    mutable std::mutex m_;
    std::vector<std::vector<uint8_t>> buf_;
    size_t dropped_ = 0;
};

class SimulatedSensorBackend : public IMediaDevice {
public:
    // Test-signal shapes for simulated microphone capture. `noise` is the
    // existing pseudo-random PCM; the others are deterministic waveforms so
    // tests can assert exact, known content (e.g. a pure tone).
    enum class TestSignal {
        noise,       // pseudo-random PCM (existing behavior)
        sine,        // deterministic sine tone
        square,      // deterministic square wave
        silence      // all-zero samples
    };

    // Deterministic configuration. Grouped so adding options never reorders a
    // positional constructor and breaks callers.
    struct Config {
        // Camera
        int width = 64;
        int height = 48;
        double frame_rate = 0.0;   // frames/sec; 0 = unpaced (fastest)
        // Audio
        int sample_rate = 16000;
        int samples_per_buffer = 320;
        int channels = 1;          // 1 = mono, 2 = stereo
        TestSignal signal = TestSignal::noise;
        double tone_hz = 440.0;    // used by sine/square
        // Buffer
        size_t ring_capacity = 256;
        // PRNG seed (fixed => reproducible)
        uint64_t seed = 0x5EED;
    };

    // Convenience constructors. The Config-based one is canonical; the
    // positional overload preserves the existing call sites unchanged.
    explicit SimulatedSensorBackend(DeviceKind kind, std::string name, Config cfg);
    SimulatedSensorBackend(DeviceKind kind,
                           std::string name,
                           int width = 64,
                           int height = 48,
                           int sample_rate = 16000,
                           int samples_per_buffer = 320,
                           uint64_t seed = 0x5EED);

    DeviceKind kind() const override { return kind_; }
    std::string name() const override { return name_; }
    DeviceState state() const override;

    // --- IMediaDevice lifecycle ---
    void initialize() override;
    void start() override;
    void stop() noexcept override;
    void close() noexcept override;
    void fail() noexcept override;  // IMediaDevice error injection

    // --- Simulation-specific controls (host tests only) ---

    // Generate the next synthetic payload WITHOUT delivering it. Returns the
    // payload bytes. Deterministic for a given call sequence. Honors the
    // configured frame_rate: if > 0, this call blocks (wall-clock) so the
    // simulated device produces frames at the requested rate. Tests that want
    // raw speed leave frame_rate at 0.
    std::vector<uint8_t> generate_frame();

    // "Deliver" a payload into the bounded buffer. Callers must have already
    // passed authorize_delivery() at the real delivery boundary; this method
    // just records the in-memory payload (it never touches disk or hardware).
    void record_delivered(std::vector<uint8_t> payload);

    // Error/recovery injection for tests.
    void inject_error();
    void recover();
    bool has_error() const;

    // --- Speaker (audio-output) simulation ---
    // Playback of an already-generated payload. The speaker does not capture,
    // so there is no consent delivery gate on play; it models the output path
    // only. Returns true on success. If a playback error has been injected
    // (or the device is not initialized), returns false and records the error.
    bool play(const std::vector<uint8_t>& payload);
    void inject_playback_error();
    bool playing() const;
    size_t played_count() const;

    // Introspection for assertions.
    int init_count() const { return init_count_; }
    int start_count() const { return start_count_; }
    int stop_count() const { return stop_count_; }
    int close_count() const { return close_count_; }
    bool released() const { return released_; }
    size_t buffer_size() const { return ring_.size(); }
    size_t buffer_dropped() const { return ring_.dropped(); }
    size_t buffer_capacity() const { return ring_.capacity(); }
    uint64_t sequence() const { return sequence_; }
    // Audio payload size in bytes for the configured channels/buffer.
    size_t audio_frame_bytes() const;

    ~SimulatedSensorBackend() override { close(); }

private:
    uint64_t next_rand();  // deterministic LCG
    void fill_camera_frame(std::vector<uint8_t>& out);
    void fill_audio_buffer(std::vector<uint8_t>& out);
    void pace_frame();     // enforce frame_rate via wall clock

    DeviceKind kind_;
    std::string name_;
    int width_;
    int height_;
    double frame_rate_;
    int sample_rate_;
    int samples_per_buffer_;
    int channels_;
    TestSignal signal_;
    double tone_hz_;
    uint64_t rng_state_;

    mutable std::mutex m_;
    DeviceState state_ = DeviceState::Idle;
    bool has_error_ = false;
    bool released_ = false;

    // Speaker simulation state (only meaningful for DeviceKind::audio output).
    bool playing_ = false;
    bool playback_error_ = false;
    size_t played_count_ = 0;

    int init_count_ = 0;
    int start_count_ = 0;
    int stop_count_ = 0;
    int close_count_ = 0;
    uint64_t sequence_ = 0;

    // Bounded acquisition buffer (camera frames / mic samples). Capacity is
    // configurable; overwrite-oldest keeps memory bounded.
    BoundedRing ring_;
};

} // namespace homeguardian
