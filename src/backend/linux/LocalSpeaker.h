#pragma once

// REAL speaker output on Ubuntu, with a bounded queue, a volume limit and a cancel path.
//
// WHAT IT PLAYS
// -------------
//   * A locally GENERATED tone (a short WAV written to the scratch directory). This is the
//     default and needs no dependency, no download and no licence review, which is why the
//     operator's first output test uses it.
//   * Optionally `spd-say` (Speech Dispatcher) if a voice is actually installed - the
//     engine reports honestly when it is not.
//
// WHAT IT IS NOT: this is NOT Amazon Alexa and never claims to be. `engine_name()` returns
// the real engine, and the code contains no Alexa branding. A genuine Alexa integration
// would be a separate authenticated runtime behind the same `speak()` seam.
//
// SAFETY
// ------
//   * Volume is limited in software (`volume_limit`, default 0.5) - the class never asks
//     the system for more, and clamps whatever it is given.
//   * The queue is BOUNDED: the newest request replaces nothing and the oldest is dropped
//     and counted, so a chatty producer cannot grow memory.
//   * cancel() kills an in-flight playback and clears the queue.
//   * Feedback avoidance: the caller suppresses output while capture is active
//     (see `set_feedback_guard`), and this class refuses a request outright when the guard
//     says the microphone is live.

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

class LocalSpeaker {
public:
    enum class JobKind { tone, speech };

    struct Job {
        JobKind kind = JobKind::tone;
        double frequency_hz = 440.0;   // tone only
        uint32_t duration_ms = 300;    // tone only
        std::string text;              // speech only
    };

    struct Config {
        std::string device;              // PipeWire sink name; empty = default sink
        std::string player_command = "pw-cat";  // or "aplay"
        std::string scratch_dir = "/tmp";
        double volume_limit = 0.5;       // hard software cap, 0.0 - 1.0
        size_t queue_capacity = 8;       // bounded
    };

    struct Stats {
        uint64_t played = 0;
        uint64_t dropped = 0;            // oldest-dropped because the queue was full
        uint64_t refused = 0;            // refused by the feedback guard or a failed engine
        uint64_t cancelled = 0;
        int last_exit_code = 0;
    };

    explicit LocalSpeaker(Config config);
    ~LocalSpeaker();

    // Worker lifecycle. start() spawns the drain thread; stop() cancels and joins it.
    void start();
    void stop() noexcept;

    // Queue one job. Returns false when refused (feedback guard active, or the queue is
    // full and the job could not be admitted).
    bool enqueue(const Job& job);
    bool play_tone(double frequency_hz, uint32_t duration_ms);
    bool speak(const std::string& text);

    // Interrupt: kill the current playback, clear the queue.
    void cancel();

    // While the microphone is live, output is suppressed unless the operator explicitly
    // allows it - a speaker and a microphone in the same room is a feedback loop, and a
    // "device that talks while it listens" is also a privacy question.
    void set_feedback_guard(bool microphone_active) { microphone_active_.store(microphone_active); }

    void set_volume_limit(double limit);
    double volume_limit() const { return volume_limit_.load(); }

    // The REAL engine name for the requested job kind, or why it cannot run. Never "Alexa".
    std::string engine_name(JobKind kind) const;
    std::string engine_available(JobKind kind, std::string* why) const;

    const Stats& stats() const { return stats_; }
    size_t queued() const;
    bool busy() const { return playing_.load(); }

    // The exact argv that would be executed for a job (tests inspect it; nothing claims a
    // result from a preview).
    std::vector<std::string> command_line_for(const Job& job) const;

    static constexpr double kMaxVolumeLimit = 1.0;

private:
    void worker_loop();
    bool run_job(const Job& job, std::string* error);
    std::string write_tone_wav(double frequency_hz, uint32_t duration_ms) const;

    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> microphone_active_{false};
    std::atomic<bool> playing_{false};
    std::atomic<double> volume_limit_{0.5};
    std::atomic<int> current_child_{-1};
    Stats stats_;
};

}  // namespace homeguardian
