#pragma once

// REAL speaker output on Ubuntu, with a bounded queue, a volume limit and a cancel path.
//
// WHAT IT PLAYS
// -------------
//   * A locally GENERATED tone (a short WAV written to the scratch directory). This is the
//     default and needs no dependency, no download and no licence review, which is why the
//     operator's first output test uses it.
//   * SPOKEN TEXT through the smallest offline engine actually installed, in preference order
//     `espeak-ng` -> `espeak` -> `spd-say` (see `choose_speech_engine`). Speech Dispatcher
//     without an output module ACCEPTS a request and produces silence, so this class reports an
//     engine as available only when something can really synthesize.
//
// WHAT IT IS NOT: this is NOT Amazon Alexa and never claims to be. `engine_name()` returns
// the real engine, and the code contains no Alexa branding. A genuine Alexa integration
// would be a separate authenticated runtime behind the same seam.
//
// SAFETY
// ------
//   * Volume is limited in software (`volume_limit`, default 0.5) - the class never asks
//     the system for more, and clamps whatever it is given.
//   * The queue is BOUNDED: the oldest job is dropped and counted, so a chatty producer cannot
//     grow memory.
//   * cancel() kills an in-flight playback and clears the queue.
//   * Feedback avoidance: the caller suppresses output while capture is active
//     (see `set_feedback_guard`), and this class refuses a request outright when the guard
//     says the microphone is live.
//   * NO SHELL, EVER: every child is fork+execvp with a validated argv, and spoken text
//     travels as exactly ONE argv element, so no detector string can reach a shell.
//   * Mute is explicit and independent of microphone consent.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "voice/IVoiceOutput.h"

namespace homeguardian {

class LocalSpeaker : public IVoiceOutput {
public:
    enum class JobKind { tone, speech };

    struct Job {
        JobKind kind = JobKind::tone;
        double frequency_hz = 440.0;   // tone only
        uint32_t duration_ms = 300;    // tone only
        std::string text;              // speech only
        int rate = 160;                // speech only: words per minute
        std::string voice;             // speech only: engine voice; empty = engine default
    };

    struct Config {
        std::string device;              // PipeWire sink name; empty = default sink
        std::string player_command = "pw-cat";  // or "aplay"
        std::string scratch_dir = "/tmp";
        double volume_limit = 0.5;       // hard software cap, 0.0 - 1.0
        size_t queue_capacity = 8;       // bounded
        std::string voice;               // preferred voice for speech jobs
        int rate = 160;                  // preferred words per minute
    };

    struct Stats {
        uint64_t played = 0;
        uint64_t dropped = 0;            // oldest-dropped because the queue was full
        uint64_t refused = 0;            // refused by the feedback guard, mute, or a failed engine
        uint64_t cancelled = 0;
        int last_exit_code = 0;
    };

    // What the operator can see about the output path. Every field is measured or explicitly
    // unknown - nothing here is inferred.
    struct OutputStatus {
        bool speech_available = false;
        std::string speech_engine;       // "" when nothing usable is installed
        std::string speech_why;          // why not, when unavailable
        bool tone_available = false;
        std::string tone_engine;
        std::string device;              // the sink asked for ("" = the system default)
        bool device_present = false;     // verified by a real query, not assumed
        std::string device_why;          // why the sink could not be confirmed
        bool muted = false;
        bool busy = false;
        size_t queued = 0;
        double volume_limit = 0.0;
        std::string last_failure;
        uint64_t played = 0;
        uint64_t dropped = 0;
        uint64_t refused = 0;
    };

    // The offline speech engine chosen from what is installed.
    struct SpeechEngine {
        std::string program;             // "" when none is usable
        bool available = false;
        bool direct = false;             // true: the synthesizer speaks by itself (espeak-ng)
        std::string why;                 // why it is unavailable, when it is
    };

    explicit LocalSpeaker(Config config);
    ~LocalSpeaker();

    // The engine choice as a PURE function of what exists, so the preference order and the
    // Speech-Dispatcher-without-a-voice case are testable without installing anything.
    static SpeechEngine choose_speech_engine(const std::function<bool(const std::string&)>& exists);

    // Build the speech argv for an ALREADY CHOSEN engine. Split out so the property that
    // matters most - the text is exactly ONE argv element, never a shell string - is testable
    // on a host where no engine happens to be installed.
    static std::vector<std::string> build_speech_argv(const SpeechEngine& engine, const Job& job,
                                                      int volume_percent,
                                                      const std::string& voice, int rate);

    // Worker lifecycle. start() spawns the drain thread; stop() cancels and joins it.
    void start();
    void stop() noexcept;

    // Queue one job. Returns false when refused (feedback guard active, muted, or the queue is
    // full and the job could not be admitted).
    bool enqueue(const Job& job);
    bool play_tone(double frequency_hz, uint32_t duration_ms);
    bool speak(const std::string& text);

    // Queue an utterance with an explicit rate/voice. The text is taken as ONE argv element and
    // never interpreted.
    bool speak_with_options(const std::string& text, int rate, const std::string& voice);

    // Interrupt: kill the current playback, clear the queue.
    void cancel() noexcept;

    // Explicit mute, independent of microphone consent.
    void set_muted(bool muted) { muted_.store(muted); }
    bool muted() const { return muted_.load(); }

    // While the microphone is live, output is suppressed unless the operator explicitly
    // allows it - a speaker and a microphone in the same room is a feedback loop, and a
    // "device that talks while it listens" is also a privacy question.
    void set_feedback_guard(bool microphone_active) { microphone_active_.store(microphone_active); }
    bool feedback_guard() const { return microphone_active_.load(); }

    void set_volume_limit(double limit);
    double volume_limit() const { return volume_limit_.load(); }

    // The REAL engine name for the requested job kind, or why it cannot run. Never "Alexa".
    std::string engine_name(JobKind kind) const;
    std::string engine_available(JobKind kind, std::string* why) const;

    // Ask the sound server whether the requested sink is really there. A Bluetooth speaker that
    // went away is REPORTED, never assumed and never fatal.
    bool output_present(std::string* why) const;

    OutputStatus get_output_status(bool probe_device) const;

    const Stats& stats() const { return stats_; }
    size_t queued() const;
    bool busy() const { return playing_.load(); }

    // The exact argv that would be executed for a job (tests inspect it; nothing claims a
    // result from a preview).
    std::vector<std::string> command_line_for(const Job& job) const;

    // ---- IVoiceOutput ------------------------------------------------------------------
    bool speak_text(const std::string& text, int rate) override { return speak_with_options(text, rate, std::string()); }
    bool speech_available(std::string* why) const override;
    void stop_speaking() override { cancel(); }
    std::string engine_label() const override { return engine_name(JobKind::speech); }
    std::string output_device() const override { return config_.device; }

    static constexpr double kMaxVolumeLimit = 1.0;

private:
    void worker_loop();
    bool run_job(const Job& job, std::string* error);
    std::string write_tone_wav(double frequency_hz, uint32_t duration_ms) const;
    void note_failure(const std::string& failure);
    // Run a probe command WITHOUT a shell, reading at most `max_bytes` of its stdout.
    static std::string run_probe(const std::vector<std::string>& argv, size_t max_bytes,
                                 int timeout_ms, int* exit_code);

    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> microphone_active_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> playing_{false};
    std::atomic<double> volume_limit_{0.5};
    std::atomic<int> current_child_{-1};
    mutable std::mutex failure_mutex_;   // written by the worker, read by status()
    std::string last_failure_;
    Stats stats_;
};

}  // namespace homeguardian
