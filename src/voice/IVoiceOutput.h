#pragma once

// The seam between the DECISION layer (what should be said) and the OUTPUT layer (how it is
// played). Exactly ONE real implementation exists in this project - `LocalSpeaker`, which is
// also what plays the generated tone - so this is deliberately not a second audio subsystem.
//
// It exists so the response policy can be tested without a sound card: every test drives a fake
// and NO test process ever opens a speaker.

#include <string>

namespace homeguardian {

class IVoiceOutput {
public:
    virtual ~IVoiceOutput() = default;

    // Queue an utterance. Must not block the caller: the implementation returns as soon as the
    // request is queued, and returns false when it was refused (muted, feedback guard, or the
    // queue is full).
    virtual bool speak_text(const std::string& text, int rate) = 0;

    // Is spoken output actually possible right now? `why` receives the reason when it is not.
    virtual bool speech_available(std::string* why) const = 0;

    virtual bool busy() const = 0;
    virtual bool muted() const = 0;
    virtual void set_muted(bool muted) = 0;
    virtual void stop_speaking() = 0;
    virtual std::string engine_label() const = 0;
    virtual std::string output_device() const = 0;
};

}  // namespace homeguardian
