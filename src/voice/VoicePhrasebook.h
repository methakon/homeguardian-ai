#pragma once

// The CLOSED spoken vocabulary for HomeGuardian's local voice feedback.
//
// WHAT THIS IS
// ------------
//   A pure, testable policy: given a REAL AudioFinding (or a system state change) it decides
//   whether anything should be said, and which APPROVED sentence to say. It performs no I/O,
//   spawns nothing and holds no audio resources.
//
// WHY IT IS CLOSED
// ----------------
//   A spoken sentence is a claim made out loud, so the set of sentences is fixed at compile
//   time and nothing derived from a payload can become speech. There is deliberately NO
//   message for a species, an animal, a person, a fall, a fire or an earthquake: the audio
//   features this project computes measure energy, not sources, and a sentence that named one
//   would be a false verification spoken aloud. `voice_text_is_forbidden()` exists so a test
//   can assert the absence rather than trust the author.
//
//   Camera statements require an EXPLICIT authenticated-device argument: a synthetic test
//   event cannot make HomeGuardian say a camera is connected.
//
// WHAT IT IS NOT: not Amazon Alexa, not a text-to-speech engine. It chooses text; the output
//   adapter speaks it.

#include <cstdint>
#include <string>

#include "audio/AudioActivitySession.h"

namespace homeguardian {

enum class VoiceMessage {
    // System status
    monitoring_active,
    monitoring_stopped,
    microphone_stopped,
    speaker_connected,
    // Sound observations
    sound_increase,
    sound_sustained,
    sound_inconclusive,
    unexplained_activity,
    // Device status
    camera_connected,
    camera_disconnected,
    microphone_unavailable
};

// The exact approved sentence for a message. Never nullptr.
const char* voice_message_text(VoiceMessage message);

// True when the text contains a word this project must never say from audio energy alone
// (a species, a person, an animal, or a disaster). Used by tests to assert the absence.
bool voice_text_is_forbidden(const std::string& text);

struct VoiceDecision {
    bool speak = false;              // policy allows this to be spoken now
    bool suppressed = false;         // a real message existed but policy held it back
    VoiceMessage message = VoiceMessage::sound_inconclusive;
    std::string reason;              // why it will not be spoken (empty when it will)
};

class VoicePhrasebook {
public:
    struct Config {
        // Nothing is announced below this confidence. An inconclusive window has no business
        // being spoken as if it were a finding.
        double min_confidence = 0.5;
        // Minimum spacing between ANY two announcements (anti-chatter).
        int64_t min_interval_ms = 4000;
        // The same sentence is not repeated inside this window, so a sustained condition does
        // not become a monologue.
        int64_t per_message_cooldown_ms = 45000;
        // A spoken sentence is bounded, always.
        size_t max_utterance_chars = 160;
        // How long the input is treated as "possibly our own voice" after playback starts.
        // This is FEEDBACK SUPPRESSION, not acoustic echo cancellation - see docs.
        int64_t suppression_window_ms = 1500;
        // Whether an inconclusive observation may be spoken at all.
        bool announce_inconclusive = true;
    };

    // A nested Config's default member initializers are not usable in the ENCLOSING class's
    // own default argument on GCC (they are not complete yet), so the default is produced by a
    // member function evaluated at call time.
    static Config defaults() { return Config{}; }
    explicit VoicePhrasebook(Config config = defaults());

    // Decide for a real finding. Returns speak=false with a reason for a quiet room, a
    // warming-up window, a decrease, or anything below the confidence gate.
    //
    // A DECISION IS NOT A COMMITMENT. These methods change no state, because the delivery can
    // still fail (muted output, no engine installed, a full queue) and a sentence that was
    // never spoken must not consume its announcement slot or start its cooldown - the same
    // mistake as recording durable work before the work succeeded.
    VoiceDecision decide(const AudioFinding& finding, int64_t now_ms);

    // Decide for an explicit system state change (monitoring started/stopped, device events).
    VoiceDecision decide_system(VoiceMessage message, int64_t now_ms);

    // A camera statement is emitted ONLY when it is about a real authenticated device.
    // `authenticated_device_present` is the caller's assertion of that fact; with it false the
    // decision never speaks, whatever the event was.
    VoiceDecision decide_camera(bool authenticated_device_present, bool connected, int64_t now_ms);

    // Record that a sentence WAS actually handed to the output. Only this consumes the
    // interval and the per-message cooldown.
    void commit(VoiceMessage message, int64_t now_ms);

    // ---- feedback-loop support (the microphone can hear our own speaker) ----------------
    // Playback opens a BOUNDED window during which input is treated as possibly our own voice.
    void note_playback_started(int64_t now_ms);
    void note_playback_finished(int64_t now_ms);
    int64_t suppression_until_ms() const { return suppression_until_ms_; }
    bool input_suppressed(int64_t now_ms) const { return now_ms < suppression_until_ms_; }

    // Validate operator-supplied test text (never detector text): bounded length, printable
    // characters only, no control characters and no shell metacharacters. A rejected string is
    // never spoken.
    static bool sanitize_utterance(const std::string& text, size_t max_chars, std::string* out,
                                   std::string* why);

    struct Counters {
        uint64_t allowed = 0;     // decisions that may be spoken
        uint64_t gated = 0;       // below the confidence gate / no message
        uint64_t suppressed = 0;  // a message existed but cooldown/interval held it back
        uint64_t no_message = 0;  // the finding maps to nothing to say
    };
    const Counters& counters() const { return counters_; }

    const Config& config() const { return config_; }

private:
    bool admit(VoiceMessage message, int64_t now_ms, std::string* reason);
    bool select_for_finding(const AudioFinding& finding, VoiceMessage* message,
                            std::string* reason) const;

    Config config_;
    int64_t last_announcement_ms_ = 0;
    int64_t suppression_until_ms_ = 0;
    // Per-message last-spoken time, indexed by the enum's integer value (fixed, small set).
    int64_t last_spoken_ms_[11] = {};
    Counters counters_{};
};

}  // namespace homeguardian
