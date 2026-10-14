#include "voice/VoicePhrasebook.h"

#include <array>
#include <cctype>

namespace homeguardian {
namespace {

struct MessageTraits {
    const char* text;
    // State changes may be repeated sooner than observations; a state change is news.
    bool state_change;
};

constexpr std::array<MessageTraits, 11> kMessages = {{
    {"HomeGuardian monitoring is active.", true},
    {"HomeGuardian monitoring has stopped.", true},
    {"Microphone monitoring has stopped.", true},
    {"Bluetooth speaker connected.", true},
    {"An increase in surrounding sound activity was detected.", false},
    {"Sustained sound activity has been detected.", false},
    {"The current sound observation is inconclusive.", false},
    {"Unusual sound activity was detected. The source has not been identified.", false},
    {"Camera one is connected over Wi-Fi.", true},
    {"Camera one has disconnected.", true},
    {"Microphone input is unavailable.", true},
}};

// A word this project must never say from an energy measurement. Kept as data so a test can
// assert the closed vocabulary does not silently grow one.
constexpr std::array<const char*, 22> kForbidden = {{
    "dog", "cat", "bird", "mouse", "rat", "gecko", "animal", "pet", "bark", "meow", "chirp",
    "person", "intruder", "burglar", "human", "speech", "voice", "fall", "fire", "earthquake",
    "flood", "alexa",
}};

int64_t index_of(VoiceMessage message) { return static_cast<int64_t>(message); }

}  // namespace

const char* voice_message_text(VoiceMessage message) {
    const int64_t index = index_of(message);
    if (index < 0 || index >= static_cast<int64_t>(kMessages.size())) {
        return "The current sound observation is inconclusive.";
    }
    return kMessages[static_cast<size_t>(index)].text;
}

bool voice_text_is_forbidden(const std::string& text) {
    // Lowercase copy so the check is not fooled by capitalisation.
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const char* word : kForbidden) {
        if (lowered.find(word) != std::string::npos) return true;
    }
    return false;
}

VoicePhrasebook::VoicePhrasebook(Config config) : config_(config) {
    if (config_.max_utterance_chars == 0) config_.max_utterance_chars = 160;
    if (config_.suppression_window_ms <= 0) config_.suppression_window_ms = 1500;
}

bool VoicePhrasebook::sanitize_utterance(const std::string& text, size_t max_chars,
                                         std::string* out, std::string* why) {
    const auto reject = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (text.empty()) return reject("the text is empty");
    if (text.size() > max_chars) return reject("the text is longer than the utterance bound");
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return reject("the text contains a control character");
        // A conservative allowlist: letters, digits, space and ordinary punctuation. Anything
        // else is refused outright rather than escaped, so no shell could ever see it - the
        // text is also passed as ONE argv element, never through a shell.
        const bool allowed = std::isalnum(u) != 0 || c == ' ' || c == '.' || c == ',' ||
                             c == '\'' || c == '-' || c == '?' || c == '!' || c == ':' ||
                             c == ';' || c == '(' || c == ')';
        if (!allowed) return reject("the text contains a character this interface does not accept");
    }
    if (why) why->clear();
    if (out) *out = text;
    return true;
}

void VoicePhrasebook::note_playback_started(int64_t now_ms) {
    suppression_until_ms_ = now_ms + config_.suppression_window_ms;
}

void VoicePhrasebook::note_playback_finished(int64_t now_ms) {
    // Playback ending never extends the window, and it never disables monitoring: analysis
    // resumes on its own when the window closes. A tail keeps a short reverb ringing out.
    const int64_t tail = now_ms + (config_.suppression_window_ms / 3);
    if (tail < suppression_until_ms_) suppression_until_ms_ = tail;
}

bool VoicePhrasebook::admit(VoiceMessage message, int64_t now_ms, std::string* reason) {
    const int64_t index = index_of(message);
    if (index < 0 || index >= static_cast<int64_t>(kMessages.size())) {
        if (reason) *reason = "unknown message";
        ++counters_.gated;
        return false;
    }
    if (last_announcement_ms_ != 0 && now_ms - last_announcement_ms_ < config_.min_interval_ms) {
        if (reason) *reason = "another announcement was made too recently";
        ++counters_.suppressed;
        return false;
    }
    const int64_t previous = last_spoken_ms_[static_cast<size_t>(index)];
    int64_t cooldown = config_.per_message_cooldown_ms;
    if (kMessages[static_cast<size_t>(index)].state_change) {
        // A state change is news even if it repeats, so it only honours the interval above.
        cooldown = 0;
    }
    if (cooldown > 0 && previous != 0 && now_ms - previous < cooldown) {
        if (reason) *reason = "the same message was spoken inside its cooldown";
        ++counters_.suppressed;
        return false;
    }
    // Deliberately records NOTHING: the caller commits only after a real hand-off.
    return true;
}

void VoicePhrasebook::commit(VoiceMessage message, int64_t now_ms) {
    const int64_t index = index_of(message);
    if (index < 0 || index >= static_cast<int64_t>(kMessages.size())) return;
    last_announcement_ms_ = now_ms;
    last_spoken_ms_[static_cast<size_t>(index)] = now_ms;
    ++counters_.allowed;
}

bool VoicePhrasebook::select_for_finding(const AudioFinding& finding, VoiceMessage* message,
                                         std::string* reason) const {
    switch (finding.kind) {
        case AudioFindingKind::warming_up:
            if (reason) *reason = "the baseline is not established yet";
            return false;
        case AudioFindingKind::quiet_baseline:
            if (reason) *reason = "activity is within the room's own baseline";
            return false;
        case AudioFindingKind::silence:
            if (reason) *reason = "a quiet room is not an announcement";
            return false;
        case AudioFindingKind::sustained_decrease:
            if (reason) *reason = "a decrease in activity is not announced";
            return false;
        case AudioFindingKind::insufficient_evidence:
            if (reason) *reason = "the observation is inconclusive";
            *message = VoiceMessage::sound_inconclusive;
            return true;
        case AudioFindingKind::microphone_stalled:
            // Nothing arrived at all. Say exactly that, and never call it a quiet room.
            *message = VoiceMessage::microphone_stopped;
            return true;
        case AudioFindingKind::sustained_increase:
            // A statistical deviation is a change this room does not normally show, but the
            // SOURCE is still unidentified. Say the honest sentence for unexplained activity.
            if (finding.statistical_deviation) {
                *message = VoiceMessage::unexplained_activity;
            } else {
                *message = VoiceMessage::sound_sustained;
            }
            return true;
    }
    if (reason) *reason = "no message for this finding";
    return false;
}

VoiceDecision VoicePhrasebook::decide(const AudioFinding& finding, int64_t now_ms) {
    VoiceDecision decision;
    VoiceMessage message = VoiceMessage::sound_inconclusive;
    std::string why;
    if (!select_for_finding(finding, &message, &why)) {
        decision.reason = why.empty() ? "nothing to announce" : why;
        ++counters_.no_message;
        return decision;
    }
    // A microphone that delivered NOTHING is reported as invalid evidence (correctly - nothing
    // was measured), and that is exactly the case the operator most needs to hear about. So the
    // validity gate applies to OBSERVATIONS, not to the "I received no samples" statement.
    if (!finding.valid && message != VoiceMessage::microphone_stopped) {
        decision.reason = "the finding is not valid, so it is not spoken";
        ++counters_.gated;
        return decision;
    }
    // An inconclusive window is only spoken when the operator wants it, and it is never
    // presented as a finding.
    if (message == VoiceMessage::sound_inconclusive &&
        !config_.announce_inconclusive) {
        decision.reason = "inconclusive observations are not announced";
        ++counters_.gated;
        return decision;
    }
    if (message != VoiceMessage::sound_inconclusive &&
        message != VoiceMessage::microphone_stopped &&
        finding.confidence < config_.min_confidence) {
        decision.reason = "the confidence is below the announcement threshold";
        ++counters_.gated;
        return decision;
    }
    decision.message = message;
    if (!admit(message, now_ms, &why)) {
        decision.reason = why;
        decision.suppressed = true;
        return decision;
    }
    decision.speak = true;
    return decision;
}

VoiceDecision VoicePhrasebook::decide_system(VoiceMessage message, int64_t now_ms) {
    VoiceDecision decision;
    decision.message = message;
    std::string why;
    if (!admit(message, now_ms, &why)) {
        decision.reason = why;
        decision.suppressed = true;
        return decision;
    }
    decision.speak = true;
    return decision;
}

VoiceDecision VoicePhrasebook::decide_camera(bool authenticated_device_present, bool connected,
                                             int64_t now_ms) {
    VoiceDecision decision;
    decision.message = connected ? VoiceMessage::camera_connected
                                 : VoiceMessage::camera_disconnected;
    if (!authenticated_device_present) {
        // A synthetic event, a test fixture, or a device that never authenticated. Saying a
        // camera is connected here would be a spoken false claim.
        decision.reason = "no authenticated device is connected, so no camera is announced";
        ++counters_.gated;
        return decision;
    }
    std::string why;
    if (!admit(decision.message, now_ms, &why)) {
        decision.reason = why;
        decision.suppressed = true;
        return decision;
    }
    decision.speak = true;
    return decision;
}

}  // namespace homeguardian
