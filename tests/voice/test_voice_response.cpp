// Tests for the local voice-feedback layer. DELIBERATELY SILENT: every case drives the
// IVoiceOutput seam with a fake, so no test process ever opens a speaker, and no test needs a
// speech engine to be installed. The one thing asserted about the REAL output is its argv,
// which is a pure function.

#include "voice/VoicePhrasebook.h"
#include "voice/VoiceResponseController.h"
#include "backend/linux/LocalSpeaker.h"
#include "catch2/catch.hpp"

namespace {

using namespace homeguardian;

AudioFinding finding_of(AudioFindingKind kind, bool valid, double confidence) {
    AudioFinding finding;
    finding.kind = kind;
    finding.valid = valid;
    finding.confidence = confidence;
    finding.real_source = true;
    finding.sentence = "test fixture";  // never spoken: the phrasebook owns the wording
    return finding;
}

// A fake output: records what would be spoken and can refuse on demand.
class FakeOutput : public IVoiceOutput {
public:
    bool available = true;
    std::string why = "the fake engine is unavailable";
    bool is_muted = false;
    bool is_busy = false;
    size_t refuse_after = 100000;
    std::vector<std::pair<std::string, int>> spoken;

    bool speak_text(const std::string& text, int rate) override {
        if (is_muted) return false;
        if (spoken.size() >= refuse_after) return false;
        spoken.emplace_back(text, rate);
        is_busy = true;
        return true;
    }
    bool speech_available(std::string* out_why) const override {
        if (!available && out_why != nullptr) *out_why = why;
        return available;
    }
    bool busy() const override { return is_busy; }
    bool muted() const override { return is_muted; }
    void set_muted(bool muted) override { is_muted = muted; }
    void stop_speaking() override { is_busy = false; }
    std::string engine_label() const override { return "fake-engine"; }
    std::string output_device() const override { return "fake-sink"; }
};

}  // namespace

TEST_CASE("voice: the spoken vocabulary is closed and claims nothing the features cannot",
          "[voice][phrasebook]") {
    // The set is fixed. Adding a member without handling it here fails this assertion.
    REQUIRE(static_cast<int>(VoiceMessage::microphone_unavailable) == 10);
    const VoiceMessage all[] = {
        VoiceMessage::monitoring_active,   VoiceMessage::monitoring_stopped,
        VoiceMessage::microphone_stopped,  VoiceMessage::speaker_connected,
        VoiceMessage::sound_increase,      VoiceMessage::sound_sustained,
        VoiceMessage::sound_inconclusive,  VoiceMessage::unexplained_activity,
        VoiceMessage::camera_connected,    VoiceMessage::camera_disconnected,
        VoiceMessage::microphone_unavailable};
    for (const VoiceMessage message : all) {
        const std::string text = voice_message_text(message);
        REQUIRE_FALSE(text.empty());
        // No species, no person, no animal, no disaster, no "alexa" - assert the absence
        // rather than trusting the author.
        REQUIRE_FALSE(voice_text_is_forbidden(text));
    }
    // The forbidden check has to actually work, or the assertion above proves nothing.
    REQUIRE(voice_text_is_forbidden("a dog was barking"));
    REQUIRE(voice_text_is_forbidden("earthquake warning"));
    REQUIRE(voice_text_is_forbidden("Alexa is listening"));
}

TEST_CASE("voice: a quiet room, a warming-up window and a drop are not announcements",
          "[voice][phrasebook]") {
    VoicePhrasebook book;
    REQUIRE_FALSE(book.decide(finding_of(AudioFindingKind::warming_up, true, 0.9), 1000).speak);
    REQUIRE_FALSE(book.decide(finding_of(AudioFindingKind::quiet_baseline, true, 0.9), 1000).speak);
    REQUIRE_FALSE(book.decide(finding_of(AudioFindingKind::silence, true, 0.9), 1000).speak);
    REQUIRE_FALSE(
        book.decide(finding_of(AudioFindingKind::sustained_decrease, true, 0.9), 1000).speak);
    REQUIRE(book.counters().no_message == 4);
}

TEST_CASE("voice: an unexplained sustained increase is spoken as exactly that",
          "[voice][phrasebook]") {
    VoicePhrasebook book;
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    const VoiceDecision decision = book.decide(finding, 1000);
    REQUIRE(decision.speak);
    REQUIRE(decision.message == VoiceMessage::unexplained_activity);
    // The sentence must state that the source is NOT identified, and must not name one.
    const std::string text = voice_message_text(decision.message);
    REQUIRE(text.find("not been identified") != std::string::npos);
    REQUIRE_FALSE(voice_text_is_forbidden(text));
}

TEST_CASE("voice: confidence below the threshold is gated, not announced", "[voice][phrasebook]") {
    VoicePhrasebook book;
    const VoiceDecision decision =
        book.decide(finding_of(AudioFindingKind::sustained_increase, true, 0.2), 1000);
    REQUIRE_FALSE(decision.speak);
    REQUIRE(decision.reason.find("confidence") != std::string::npos);
    REQUIRE(book.counters().gated == 1);
}

TEST_CASE("voice: a stalled microphone is announced as a FACT even though the finding is invalid",
          "[voice][phrasebook]") {
    VoicePhrasebook book;
    // The session marks "no samples arrived" as invalid evidence (nothing was measured), and it
    // is precisely the case the operator must hear about.
    const VoiceDecision decision =
        book.decide(finding_of(AudioFindingKind::microphone_stalled, false, 0.0), 1000);
    REQUIRE(decision.speak);
    REQUIRE(decision.message == VoiceMessage::microphone_stopped);
    const std::string text = voice_message_text(decision.message);
    REQUIRE(text.find("Microphone") != std::string::npos);
    REQUIRE_FALSE(voice_text_is_forbidden(text));
}

TEST_CASE("voice: repeating the same sentence inside its cooldown is suppressed",
          "[voice][phrasebook]") {
    VoicePhrasebook::Config config;
    VoicePhrasebook book(config);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    const VoiceDecision first = book.decide(finding, 1000);
    REQUIRE(first.speak);
    book.commit(first.message, 1000);  // it was spoken, so the slot is consumed
    REQUIRE(book.counters().allowed == 1);
    // Past the interval but inside the per-message cooldown: a sustained condition must not
    // become a monologue.
    const VoiceDecision second = book.decide(finding, 1000 + config.min_interval_ms + 100);
    REQUIRE_FALSE(second.speak);
    REQUIRE(second.suppressed);
    REQUIRE(book.counters().suppressed == 1);
    // After the cooldown it may be said again.
    const VoiceDecision third = book.decide(finding, 1000 + config.per_message_cooldown_ms + 200);
    REQUIRE(third.speak);
    book.commit(third.message, 1000 + config.per_message_cooldown_ms + 200);
    REQUIRE(book.counters().allowed == 2);
}

TEST_CASE("voice: a camera is never announced without an authenticated device",
          "[voice][phrasebook]") {
    VoicePhrasebook book;
    const VoiceDecision synthetic = book.decide_camera(false, true, 1000);
    REQUIRE_FALSE(synthetic.speak);
    REQUIRE(synthetic.reason.find("authenticated") != std::string::npos);
    // A real device, and enough time later, is announced.
    const VoiceDecision real = book.decide_camera(true, true, 5000);
    REQUIRE(real.speak);
    REQUIRE(real.message == VoiceMessage::camera_connected);
}

TEST_CASE("voice: operator test text is sanitized, and a rejected string is never spoken",
          "[voice][phrasebook]") {
    std::string out;
    std::string why;
    REQUIRE(VoicePhrasebook::sanitize_utterance("HomeGuardian local voice feedback test.",
                                                160, &out, &why));
    REQUIRE(out == "HomeGuardian local voice feedback test.");
    // Control characters, over-long text and non-printable bytes are refused outright.
    REQUIRE_FALSE(VoicePhrasebook::sanitize_utterance(std::string("bad\nline"), 160, &out, &why));
    REQUIRE_FALSE(VoicePhrasebook::sanitize_utterance(std::string("tab\there"), 160, &out, &why));
    REQUIRE_FALSE(VoicePhrasebook::sanitize_utterance(std::string(400, 'x'), 160, &out, &why));
    REQUIRE_FALSE(VoicePhrasebook::sanitize_utterance(std::string("bell\x07"), 160, &out, &why));
    REQUIRE_FALSE(why.empty());
}

TEST_CASE("voice: the engine preference order is a pure function of what exists",
          "[voice][engine]") {
    const auto exists_only = [](std::initializer_list<const char*> present) {
        return [present](const std::string& name) {
            for (const char* candidate : present) {
                if (name == candidate) return true;
            }
            return false;
        };
    };
    const LocalSpeaker::SpeechEngine preferred =
        LocalSpeaker::choose_speech_engine(exists_only({"spd-say", "espeak", "espeak-ng"}));
    REQUIRE(preferred.available);
    REQUIRE(preferred.program == "espeak-ng");  // smallest direct engine, chosen first
    REQUIRE(preferred.direct);

    const LocalSpeaker::SpeechEngine fallback = LocalSpeaker::choose_speech_engine(exists_only({"espeak", "spd-say"}));
    REQUIRE(fallback.available);
    REQUIRE(fallback.program == "espeak");

    // Speech Dispatcher alone would accept the request and produce SILENCE while exiting 0.
    const LocalSpeaker::SpeechEngine dispatcher = LocalSpeaker::choose_speech_engine(exists_only({"spd-say"}));
    REQUIRE_FALSE(dispatcher.available);
    REQUIRE(dispatcher.why.find("synthesizer backend") != std::string::npos);

    const LocalSpeaker::SpeechEngine none = LocalSpeaker::choose_speech_engine(exists_only({}));
    REQUIRE_FALSE(none.available);
    REQUIRE_FALSE(none.why.empty());
}

TEST_CASE("voice: spoken text is ONE argv element and never a shell string", "[voice][output]") {
    LocalSpeaker::Job job;
    job.kind = LocalSpeaker::JobKind::speech;
    // A hostile payload: if any path interpreted this, the test's own assertion would show it.
    job.text = "hello; rm -rf /tmp/x $(id) `whoami` | tee /tmp/y && echo done";
    LocalSpeaker::SpeechEngine engine;
    engine.program = "espeak-ng";
    engine.available = true;
    engine.direct = true;
    const std::vector<std::string> argv =
        LocalSpeaker::build_speech_argv(engine, job, 40, "", 160);
    REQUIRE(argv.size() == 6);
    REQUIRE(argv[0] == "espeak-ng");
    REQUIRE(argv[1] == "-s");
    REQUIRE(argv[2] == "160");
    REQUIRE(argv[3] == "-a");
    REQUIRE(argv[4] == "40");  // the software volume cap, not the system's
    REQUIRE(argv.back() == job.text);  // byte-for-byte, as a single element
    // No shell anywhere in the argv, and no element that could be read as a command.
    for (const std::string& element : argv) {
        REQUIRE(element.find("/bin/sh") == std::string::npos);
        REQUIRE(element != "-c");
    }
}

TEST_CASE("voice: the app's real output never answers to Alexa's name", "[voice][output]") {
    LocalSpeaker::Config config;
    config.device = "fake_sink_for_status_only";
    LocalSpeaker speaker(config);
    // Nothing is started, so no worker thread runs and no sound is played.
    REQUIRE(speaker.engine_label().find("Alexa") == std::string::npos);
    REQUIRE(speaker.engine_label().find("alexa") == std::string::npos);
    std::string why;
    const bool available = speaker.speech_available(&why);
    if (!available) {
        // Honest reporting beats a silent no-op: there must be a reason.
        REQUIRE_FALSE(why.empty());
    } else {
        REQUIRE_FALSE(speaker.engine_label().empty());
    }
    const LocalSpeaker::OutputStatus status = speaker.get_output_status(false);
    REQUIRE(status.muted == false);
    REQUIRE(status.volume_limit > 0.0);
    REQUIRE(status.volume_limit <= LocalSpeaker::kMaxVolumeLimit);
}

TEST_CASE("voice: mute and the microphone feedback guard are separate controls",
          "[voice][output]") {
    LocalSpeaker::Config config;
    LocalSpeaker speaker(config);  // never started: nothing is played by this test
    REQUIRE(speaker.play_tone(440.0, 100));          // queued, not played
    REQUIRE(speaker.stats().refused == 0);
    speaker.set_muted(true);
    REQUIRE_FALSE(speaker.play_tone(440.0, 100));
    REQUIRE(speaker.stats().refused == 1);
    speaker.set_muted(false);
    speaker.set_feedback_guard(true);
    REQUIRE_FALSE(speaker.play_tone(440.0, 100));    // would squeal into a live microphone
    REQUIRE(speaker.stats().refused == 2);
    speaker.set_feedback_guard(false);
    REQUIRE(speaker.muted() == false);
    REQUIRE(speaker.feedback_guard() == false);
}

TEST_CASE("voice: the spoken sentence corresponds to the real finding", "[voice][control]") {
    FakeOutput output;
    VoiceResponseController::Config config;
    VoiceResponseController controller(config, &output);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    REQUIRE(controller.announce_finding(finding, 1000));
    REQUIRE(output.spoken.size() == 1);
    // Exactly the approved sentence for that finding - not a recording, not a generic line.
    REQUIRE(output.spoken[0].first ==
            "Unusual sound activity was detected. The source has not been identified.");
    REQUIRE(output.spoken[0].second == config.rate);
    // And a finding that maps to nothing says nothing.
    REQUIRE_FALSE(controller.announce_finding(finding_of(AudioFindingKind::quiet_baseline, true, 0.9),
                                              9000));
    REQUIRE(output.spoken.size() == 1);
}

TEST_CASE("voice: disabled, muted and unavailable outputs are counted, never silent", "[voice][control]") {
    FakeOutput output;
    VoiceResponseController::Config config;
    VoiceResponseController controller(config, &output);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;

    controller.set_enabled(false);
    REQUIRE_FALSE(controller.announce_finding(finding, 1000));
    controller.set_enabled(true);

    REQUIRE(controller.set_muted(true));
    REQUIRE_FALSE(controller.announce_finding(finding, 2000));
    REQUIRE(controller.status(2000).refused == 1);
    REQUIRE(controller.set_muted(false));

    output.available = false;
    REQUIRE_FALSE(controller.announce_finding(finding, 3000));
    REQUIRE(controller.status(3000).engine_available == false);
    REQUIRE_FALSE(controller.status(3000).engine_why.empty());

    output.available = true;
    REQUIRE(controller.announce_finding(finding, 4000));
    REQUIRE(controller.status(4000).spoken == 1);
}

TEST_CASE("voice: a full queue is refused and counted, and never blocks the caller",
          "[voice][control]") {
    FakeOutput output;
    output.refuse_after = 1;
    VoiceResponseController::Config config;
    config.phrasebook.min_interval_ms = 0;
    config.phrasebook.per_message_cooldown_ms = 0;
    VoiceResponseController controller(config, &output);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    REQUIRE(controller.announce_finding(finding, 1000));
    REQUIRE_FALSE(controller.announce_finding(finding, 2000));  // saturated
    REQUIRE(controller.status(2000).refused == 1);
    REQUIRE(output.spoken.size() == 1);
}

TEST_CASE("voice: a REFUSED delivery does not consume the announcement slot",
          "[voice][control]") {
    // The bug this guards: if the policy recorded the sentence before the output accepted it,
    // a muted or engine-less attempt would silently burn the slot and the next real finding
    // would be suppressed as "too soon" - a message that was never spoken stealing the turn.
    FakeOutput output;
    VoiceResponseController::Config config;
    VoiceResponseController controller(config, &output);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    output.available = false;  // no engine: the sentence cannot be delivered
    REQUIRE_FALSE(controller.announce_finding(finding, 1000));
    REQUIRE(controller.status(1000).refused == 1);
    REQUIRE(controller.status(1000).spoken == 0);
    output.available = true;
    // Well inside what WOULD have been the interval: it must still be able to speak, because
    // nothing was spoken before.
    REQUIRE(controller.announce_finding(finding, 1500));
    REQUIRE(output.spoken.size() == 1);
}

TEST_CASE("voice: our own announcement opens a bounded window and monitoring resumes after it",
          "[voice][control]") {
    FakeOutput output;
    VoiceResponseController::Config config;
    VoiceResponseController controller(config, &output);
    AudioFinding finding = finding_of(AudioFindingKind::sustained_increase, true, 0.9);
    finding.statistical_deviation = true;
    REQUIRE_FALSE(controller.input_suppressed(1000));
    REQUIRE(controller.announce_finding(finding, 1000));
    // The input is now treated as possibly our own voice...
    REQUIRE(controller.input_suppressed(1100));
    // ...but ONLY for the bounded window: suppression must not disable environmental
    // monitoring, and must not be confused with a failed microphone.
    const int64_t window = config.phrasebook.suppression_window_ms;
    REQUIRE_FALSE(controller.input_suppressed(1000 + window + 1));
    // Analysis resumes on its own: a later finding is announced again after its cooldown.
    REQUIRE(controller.announce_finding(finding, 1000 + config.phrasebook.per_message_cooldown_ms + 2000));
    REQUIRE(output.spoken.size() == 2);
}
