#pragma once

// AlsaCommandAudioSource: the Linux microphone adapter, implemented against the platform-neutral
// IAudioObservationSource so the fusion engine never learns what a microphone is.
//
// WHY A COMMAND AND NOT libasound
//   The camera path deliberately talks to the kernel directly and links no media library. There is no
//   equivalent dependency-free path to ALSA capture: the raw interface is libasound's headers. `arecord`
//   is the distribution's own ALSA front end and is already required by any machine that can capture, so
//   the adapter drives it and reads PCM from a PIPE. No file path is ever passed, so no audio is stored;
//   the window is reduced to bounded scalars and dropped.
//
// THE WINDOW IS THE UNIT
//   Each read_window() is one bounded capture process. There is no long-lived device handle, which means
//   a device that disappears between windows is contained to that window instead of wedging a session,
//   and a device that appears is picked up without a restart. The cost is process startup per window,
//   which is recorded rather than hidden.
//
// WHAT THE RUNNER IS FOR
//   The command is injected (CommandRunner) so the tests exercise every path - missing tool, missing
//   device, a short read, an odd byte count, an endless stream - without a microphone. The real runner
//   is fork/exec with a pipe, and it is bounded: a command that streams forever is cut off rather than
//   allowed to grow the process.

#include "core/AudioObservation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace homeguardian {

struct CommandResult {
    int exit_code = -1;
    std::string output;
    std::string error;
    bool truncated = false;      // the output hit the byte bound and was cut off
};

using CommandRunner = std::function<CommandResult(const std::vector<std::string>& argv, size_t max_bytes)>;

// The default runner: fork/execvp with a pipe to stdout, bounded by max_bytes.
CommandResult run_command_piped(const std::vector<std::string>& argv, size_t max_bytes);

class AlsaCommandAudioSource : public IAudioObservationSource {
public:
    struct Config {
        std::string device = "default";     // an ALSA device name, never a file
        uint32_t sample_rate = 48000;
        uint32_t channels = 1;
        // BOUNDED, and the bound is not arbitrary: extract_audio_features refuses more than
        // FeatureBounds::kMaxAudioFrames (48000) frames per call, so one window is at most 1 s at
        // 48 kHz. A longer measurement is repeated windows, which keeps memory per window bounded and
        // makes the window length an explicit choice instead of an off-by-one nobody notices.
        uint32_t seconds = 1;
        std::string command = "arecord";
        size_t max_bytes = 8u * 1024u * 1024u;
        CommandRunner runner = run_command_piped;
    };

    explicit AlsaCommandAudioSource(Config cfg);

    std::string platform() const override { return "alsa-command"; }

    // Verifies that the tool runs and that the machine reports capture hardware. It does NOT try to
    // match the configured device name against the listing: ALSA aliases ("default", "plughw:1,0") do
    // not appear there literally, so a name check would refuse valid devices. A wrong device fails at
    // capture time instead, which surfaces as "no window" rather than as a plausible empty one.
    bool open(std::string* error) override;
    void close() override { opened_ = false; }

    // One bounded window. Returns false when no window could be read, which the caller must treat as
    // "no evidence" - never as a quiet scene.
    bool read_window(std::vector<int16_t>& pcm, size_t* frames, uint32_t* channels,
                     uint32_t* sample_rate) override;

    const Config& config() const { return cfg_; }
    // The argv actually used for a capture, so a test can prove no file is written.
    std::vector<std::string> capture_argv() const;
    // Whether the last read returned fewer frames than the device was asked for.
    bool last_read_was_short() const { return last_read_short_; }

private:
    Config cfg_{};
    bool opened_ = false;
    bool last_read_short_ = false;
};

}  // namespace homeguardian
