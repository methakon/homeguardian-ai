#include "voice/VoiceResponseController.h"

namespace homeguardian {

VoiceResponseController::VoiceResponseController(Config config, IVoiceOutput* output)
    : config_(config), phrasebook_(config.phrasebook), output_(output) {}

bool VoiceResponseController::deliver(VoiceMessage message, int64_t now_ms, std::string* why) {
    const std::string text = voice_message_text(message);
    if (output_ == nullptr) {
        if (why) *why = "no output is attached";
        ++refused_;
        return false;
    }
    std::string engine_why;
    if (!output_->speech_available(&engine_why)) {
        // No engine, no sentence. Report it instead of pretending to have spoken.
        if (why) *why = engine_why.empty() ? std::string("no speech engine is available") : engine_why;
        last_failure_ = why ? *why : std::string("speech unavailable");
        ++refused_;
        return false;
    }
    // Enqueue only: the output has its own worker, so the caller (the detection loop) is never
    // blocked by playback.
    if (!output_->speak_text(text, config_.rate)) {
        if (why) *why = "the output refused the request (muted, feedback guard, or a full queue)";
        last_failure_ = "the output refused the request";
        ++refused_;
        return false;
    }
    last_message_ = text;
    // The sentence was really handed over, so NOW the slot is consumed and the cooldown starts.
    phrasebook_.commit(message, now_ms);
    // Open the bounded "this might be our own voice" window at hand-off. With a queue this
    // starts slightly early, which suppresses MORE input, never less - the conservative side.
    phrasebook_.note_playback_started(now_ms);
    return true;
}

bool VoiceResponseController::announce_finding(const AudioFinding& finding, int64_t now_ms) {
    if (!config_.enabled) return false;  // no state is consumed while speech is off
    const VoiceDecision decision = phrasebook_.decide(finding, now_ms);
    if (!decision.speak) return false;
    std::string why;
    if (!deliver(decision.message, now_ms, &why)) return false;
    return true;
}

bool VoiceResponseController::announce_system(VoiceMessage message, int64_t now_ms) {
    if (!config_.enabled) return false;  // no state is consumed while speech is off
    const VoiceDecision decision = phrasebook_.decide_system(message, now_ms);
    if (!decision.speak) return false;
    std::string why;
    return deliver(decision.message, now_ms, &why);
}

bool VoiceResponseController::speak_operator_text(const std::string& text, int64_t now_ms,
                                                  std::string* why) {
    std::string clean;
    if (!VoicePhrasebook::sanitize_utterance(text, config_.phrasebook.max_utterance_chars, &clean,
                                            why)) {
        ++refused_;
        last_failure_ = why ? *why : std::string("the text was rejected");
        return false;
    }
    if (output_ == nullptr) {
        if (why) *why = "no output is attached";
        ++refused_;
        return false;
    }
    std::string engine_why;
    if (!output_->speech_available(&engine_why)) {
        if (why) *why = engine_why.empty() ? std::string("no speech engine is available") : engine_why;
        last_failure_ = why ? *why : std::string("speech unavailable");
        ++refused_;
        return false;
    }
    if (!output_->speak_text(clean, config_.rate)) {
        if (why) *why = "the output refused the request (muted, feedback guard, or a full queue)";
        ++refused_;
        return false;
    }
    last_message_ = clean;
    phrasebook_.note_playback_started(now_ms);
    return true;
}

bool VoiceResponseController::set_muted(bool muted) {
    if (output_ == nullptr) return false;
    output_->set_muted(muted);
    return output_->muted() == muted;
}

bool VoiceResponseController::muted() const { return output_ != nullptr && output_->muted(); }

bool VoiceResponseController::speech_available(std::string* why) const {
    if (output_ == nullptr) {
        if (why) *why = "no output is attached";
        return false;
    }
    return output_->speech_available(why);
}

void VoiceResponseController::stop() noexcept {
    if (output_ != nullptr) output_->stop_speaking();
}

VoiceResponseController::Status VoiceResponseController::status(int64_t now_ms) const {
    Status status;
    status.enabled = config_.enabled;
    status.muted = muted();
    std::string why;
    status.engine_available = speech_available(&why);
    status.engine_why = why;
    if (output_ != nullptr) {
        status.engine = output_->engine_label();
        status.device = output_->output_device();
    }
    status.last_message = last_message_;
    status.last_failure = last_failure_;
    status.suppressed = phrasebook_.counters().suppressed;
    status.gated = phrasebook_.counters().gated + phrasebook_.counters().no_message;
    status.spoken = phrasebook_.counters().allowed;
    status.refused = refused_;
    status.suppression_until_ms = phrasebook_.suppression_until_ms();
    status.input_suppressed = phrasebook_.input_suppressed(now_ms);
    return status;
}

}  // namespace homeguardian
