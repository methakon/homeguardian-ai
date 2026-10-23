#pragma once

// Ties the spoken-decision policy to the one real audio output, without ever blocking the
// detection pipeline: `announce_*` only ENQUEUES (the output has its own worker thread), and
// every refusal is counted with its reason rather than silently dropped.
//
// Two independent controls, which the operator asked for explicitly:
//   * `enabled`  - may HomeGuardian speak at all?
//   * `muted`    - the output's own mute (and the microphone's consent is a THIRD, separate
//                  control: an operator can stop listening while keeping speech enabled, or
//                  mute speech while leaving the microphone authorized).
//
// It also owns the feedback-loop window: when a sentence is handed to the player, the input is
// treated as "possibly our own voice" for a bounded time, so HomeGuardian does not hear its own
// announcement and report it as an independent finding. That is FEEDBACK SUPPRESSION, not
// acoustic echo cancellation, and the limitation is documented.

#include <cstdint>
#include <string>

#include "audio/AudioActivitySession.h"
#include "voice/IVoiceOutput.h"
#include "voice/VoicePhrasebook.h"

namespace homeguardian {

class VoiceResponseController {
public:
    struct Config {
        VoicePhrasebook::Config phrasebook;
        bool enabled = true;
        int rate = 160;
    };

    struct Status {
        bool enabled = false;
        bool muted = false;
        bool engine_available = false;
        bool input_suppressed = false;
        std::string engine;
        std::string engine_why;
        std::string device;
        std::string last_message;     // the last sentence handed to the output
        std::string last_failure;     // the last playback failure, or empty
        uint64_t spoken = 0;
        uint64_t suppressed = 0;      // a real message existed, policy held it back
        uint64_t gated = 0;           // nothing to say / below the confidence gate
        uint64_t refused = 0;         // the output refused the request
        int64_t suppression_until_ms = 0;
    };

    // `output` is NOT owned: this process has exactly one audio-output subsystem.
    VoiceResponseController(Config config, IVoiceOutput* output);

    // Announce a REAL finding. Returns true when a sentence was actually queued.
    bool announce_finding(const AudioFinding& finding, int64_t now_ms);
    bool announce_system(VoiceMessage message, int64_t now_ms);

    // Operator-requested test speech. The text is SANITIZED first (bounded, printable, no
    // control characters), and a rejected string is never spoken.
    bool speak_operator_text(const std::string& text, int64_t now_ms, std::string* why);

    void set_enabled(bool enabled) { config_.enabled = enabled; }
    bool enabled() const { return config_.enabled; }
    bool set_muted(bool muted);
    bool muted() const;
    bool speech_available(std::string* why) const;

    // True while the input should be treated as possibly our own announcement.
    bool input_suppressed(int64_t now_ms) const { return phrasebook_.input_suppressed(now_ms); }
    void note_playback_finished(int64_t now_ms) { phrasebook_.note_playback_finished(now_ms); }

    void stop() noexcept;   // cancel playback as the process exits; never leaves a child running

    Status status(int64_t now_ms) const;
    const VoicePhrasebook& phrasebook() const { return phrasebook_; }

private:
    bool deliver(VoiceMessage message, int64_t now_ms, std::string* why);

    Config config_;
    VoicePhrasebook phrasebook_;
    IVoiceOutput* output_ = nullptr;
    std::string last_message_;
    std::string last_failure_;
    uint64_t refused_ = 0;
};

}  // namespace homeguardian
