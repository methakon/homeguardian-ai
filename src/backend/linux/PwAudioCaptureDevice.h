#pragma once

// REAL microphone capture on Ubuntu, behind the existing IMediaDevice contract.
//
// WHY A SUBPROCESS AND NOT libasound
// ----------------------------------
// This host has no ALSA development headers and `sudo` requires a password, so a
// libasound-linked backend cannot be built here at all. PipeWire's own CLI tools
// (`pw-record`, `parec`) are present and go through the very server the desktop uses, so
// the capture path is REAL PCM from the real device - not simulated - with no new
// dependency, no root, and no device node opened directly.
//
// WHAT THIS IS NOT
// ----------------
//   * NOT speech recognition, keyword spotting or transcription.
//   * NOT species, emotion, danger or emergency inference. Energy is energy.
//   * NOT continuous or unattended: start() REFUSES unless the operator authorized
//     capture explicitly (see authorize_capture), so no code path can open the
//     microphone because it was constructed.
//
// SAFETY
// ------
//   * Capture is authorizable, revocable, and auditable: `authorize_capture(false)`
//     stops an active session immediately and releases the child process.
//   * The PCM ring buffer is BOUNDED; samples older than its capacity are dropped rather
//     than queued, so a stalled consumer cannot grow memory.
//   * No raw audio is written to disk by this class - it hands bounded windows to the
//     caller, which computes scalars and discards them.
//   * Device removal is detected as the child process exiting; the device goes to Error
//     and reports why, rather than pretending to capture silence.

#include "core/IMediaDevice.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace homeguardian {

class PwAudioCaptureDevice : public IMediaDevice {
public:
    struct Config {
        std::string device;              // PipeWire/Pulse source name; empty = default source
        uint32_t sample_rate = 48000;
        uint32_t channels = 1;
        // parec's format name. pw-record uses the short form and the ctor normalises it.
        std::string sample_format = "s16le";
        size_t ring_capacity_frames = 48000;  // 1 s at 48 kHz: bounded, oldest dropped
        // The one allowed sample rate set. The adapter asks for `sample_rate` and lets
        // PipeWire convert from whatever the hardware actually runs at.
        // `parec` is the tool PROVEN to deliver PCM on this host. `pw-record` exists too
        // and is supported by the tool-aware argv builder, but it has no --raw option and
        // a wrong flag makes it exit instantly, which presents as a dead microphone.
        std::string capture_command = "parec";
        // A capture tool needs a moment to start delivering: parec takes ~0.2-1 s to produce
        // its first sample. Waiting only milliseconds and then declaring the device dead
        // misreports a normal startup as a hardware fault, which is exactly the kind of
        // misleading "equipment failed" claim this project refuses to make.
        int first_read_grace_ms = 2000;   // the first read after start()
        int read_wait_ms = 200;           // every later read
        bool dry_run = false;            // true = never spawn; report the plan instead
    };

    struct Stats {
        uint64_t windows_read = 0;
        uint64_t frames_captured = 0;
        uint64_t frames_dropped = 0;     // overwritten in the ring because the reader lagged
        uint64_t spawn_failures = 0;
        uint64_t disconnects = 0;
        uint64_t child_stderr_bytes = 0;   // diagnostics seen from the capture child
        int last_exit_code = 0;
    };

    explicit PwAudioCaptureDevice(Config config);
    ~PwAudioCaptureDevice() override;

    // ---- IMediaDevice ----
    DeviceKind kind() const override { return DeviceKind::audio; }
    void initialize() override;          // Idle -> Initialized (opens nothing)
    void start() override;               // Initialized -> Capturing (REFUSES unless authorized)
    void stop() noexcept override;       // Capturing -> Stopped (kills the child, releases fds)
    void close() noexcept override;      // -> Idle
    void fail() noexcept override;       // -> Error
    DeviceState state() const override;
    std::string name() const override;

    // ---- operator control ----
    // Explicit, revocable authorization for physical capture. start() refuses without it,
    // and revoking it while capturing stops the session at once - the OS-level permission
    // is necessary but is not treated as sufficient.
    void authorize_capture(bool authorized);
    bool capture_authorized() const { return capture_authorized_.load(); }

    // Read the next window of PCM from the ring. Returns the frames copied (0 when the
    // buffer is empty). Never throws. `frames_requested` is clamped to the bound.
    size_t read_frames(int16_t* out, size_t frames_requested);

    // The exact command line that WOULD be executed (used by tests and by the operator
    // preview; never a substitute for actually running it).
    std::vector<std::string> capture_command_line() const;
    const std::string& last_error() const { return last_error_; }
    const std::string& refused_reason() const { return capture_refused_reason_; }
    const Stats& stats() const { return stats_; }
    const Config& config() const { return config_; }

    static constexpr size_t kMaxFramesPerRead = 48000;

private:
    void spawn_capture_child(std::string* error);
    void terminate_child() noexcept;
    // A DEDICATED READER THREAD owns the child's pipes and does blocking reads via poll().
    //
    // WHY NOT read from the consumer's thread with O_NONBLOCK: O_NONBLOCK is a property of
    // the OPEN FILE DESCRIPTION, and dup2 makes the child's stdout share that description with
    // this process's read end. Setting it here therefore also made the CHILD's writes
    // non-blocking, so once the pipe filled its next write failed and the capture tool stopped
    // producing - after exactly one pipe buffer (65,536 bytes), with the child alive, no EOF
    // and nothing on stderr. A reader thread with blocking reads and poll() for readiness
    // leaves the child's descriptors completely alone.
    void reader_loop();
    void stop_reader() noexcept;          // signal + join, never while holding the mutex
    void drain_child_stderr();           // ALWAYS drain diagnostics: a full pipe blocks the child
    void set_error(const std::string& why) noexcept;

    Config config_;
    mutable std::mutex mutex_;
    DeviceState state_ = DeviceState::Idle;
    std::atomic<bool> capture_authorized_{false};
    int child_pid_ = -1;
    int child_stdout_ = -1;
    int child_stderr_fd_ = -1;   // the pipe; distinct from the message below
    std::string child_stderr_;   // what the child said, for the operator
    std::string capture_refused_reason_;
    bool first_read_ = true;
    std::string last_error_;
    std::deque<int16_t> ring_;           // interleaved int16 samples
    std::condition_variable ring_cv_;
    std::thread reader_;
    // Atomics because the stopper and the reader race on them by design.
    std::atomic<bool> reader_stop_{false};
    std::atomic<bool> reader_done_{false};
    Stats stats_;
};

}  // namespace homeguardian
