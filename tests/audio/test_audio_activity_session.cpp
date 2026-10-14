#include "catch2/catch.hpp"
#include "audio/AudioActivitySession.h"
#include "backend/linux/LocalSpeaker.h"
#include "backend/linux/PwAudioCaptureDevice.h"
#include "core/Logger.h"

#include <cmath>
#include <string>
#include <vector>

using namespace homeguardian;

// These tests are DELIBERATELY SILENT AND DEAF: no test in this file opens the microphone,
// spawns a capture child or plays a sound. The capture device is exercised in `dry_run`
// (which never spawns) or in states where start() must REFUSE, and the speaker is only
// ever asked for things it may refuse or for its command line. The first real capture and
// the first real playback are operator-gated (see src/hg_audio_bridge.cpp).

namespace {

std::vector<int16_t> tone_frame(size_t frames, double amplitude, double frequency = 440.0,
                                uint32_t rate = 48000) {
    std::vector<int16_t> pcm(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        pcm[i] = static_cast<int16_t>(std::sin(2.0 * M_PI * frequency * t) * amplitude * 32767.0);
    }
    return pcm;
}

std::vector<int16_t> quiet_frame(size_t frames) { return std::vector<int16_t>(frames, 0); }

}  // namespace

TEST_CASE("audio session: a quiet room reports silence, not a finding about the world",
          "[audio][session]") {
    Logger::initialize("error");
    int64_t now = 1700000000000;
    AudioActivitySession::Config config;
    config.window_frames = 4800;
    AudioActivitySession session(config, [&now] { return now; });

    // Warm-up first: the session must not claim knowledge before a baseline exists.
    AudioFinding first = session.observe(quiet_frame(4800).data(), 4800, true);
    REQUIRE(first.valid);
    REQUIRE(first.kind == AudioFindingKind::warming_up);
    REQUIRE_FALSE(first.baseline_ready);

    for (int i = 0; i < 10; ++i) {
        now += 100;
        session.observe(quiet_frame(4800).data(), 4800, true);
    }
    now += 100;
    const AudioFinding quiet = session.observe(quiet_frame(4800).data(), 4800, true);
    REQUIRE(quiet.valid);
    REQUIRE(quiet.baseline_ready);
    REQUIRE(quiet.kind == AudioFindingKind::silence);
    REQUIRE(quiet.sentence == "Ambient activity is within the established baseline.");
    REQUIRE(quiet.real_source);  // the caller vouched for real samples
    REQUIRE(quiet.silent_windows > 0);
}

TEST_CASE("audio session: a sustained rise is reported without identifying the source",
          "[audio][session]") {
    Logger::initialize("error");
    int64_t now = 1700000000000;
    AudioActivitySession session(AudioActivitySession::Config{}, [&now] { return now; });

    for (int i = 0; i < 12; ++i) {  // establish a quiet baseline
        now += 100;
        session.observe(quiet_frame(4800).data(), 4800, true);
    }
    AudioFinding rising{};
    bool saw_increase = false;
    for (int i = 0; i < 10; ++i) {  // then a sustained loud period
        now += 100;
        rising = session.observe(tone_frame(4800, 0.5).data(), 4800, true);
        if (rising.kind == AudioFindingKind::sustained_increase) {
            saw_increase = true;
            break;
        }
    }
    REQUIRE(saw_increase);
    REQUIRE(rising.sustained);
    REQUIRE(rising.ratio > 1.0);
    // The explanation must not name a source, and must SAY that it does not.
    REQUIRE(rising.sentence.find("not identified") != std::string::npos);
    REQUIRE(rising.uncertainty.find("not identified") != std::string::npos);
    REQUIRE(rising.confidence > 0.0);
    REQUIRE(rising.confidence < 1.0);  // never certainty
}

TEST_CASE("audio session: a microphone that stops delivering is not a quiet room",
          "[audio][session]") {
    Logger::initialize("error");
    int64_t now = 1700000000000;
    // In production the bound is a TIME decision (roughly ten seconds of silence), so the test
    // pins it low to exercise the mechanism instead of waiting. The shipped default is asserted
    // in its own case below.
    AudioActivitySession::Config config;
    config.stalled_after_empty_windows = 3;
    AudioActivitySession session(config, [&now] { return now; });

    const AudioFinding one = session.observe_no_samples();
    REQUIRE(one.kind == AudioFindingKind::insufficient_evidence);  // one stall is not a verdict
    AudioFinding stalled{};
    for (int i = 0; i < 3; ++i) {
        now += 100;
        stalled = session.observe_no_samples();
    }
    REQUIRE(stalled.kind == AudioFindingKind::microphone_stalled);
    // The wording states the FACT and the bound and names the plausible causes; it must not
    // diagnose the hardware, because this component cannot see the hardware.
    REQUIRE(stalled.sentence == "The microphone has delivered no samples for several seconds.");
    REQUIRE(stalled.reason.find("consecutive windows") != std::string::npos);
    REQUIRE_FALSE(stalled.valid);
    // Nothing was measured, so no provenance may be claimed.
    REQUIRE_FALSE(stalled.real_source);
    REQUIRE(stalled.uncertainty.find("unknown") != std::string::npos);

    // A sample arriving again clears the stall.
    now += 100;
    const AudioFinding recovered = session.observe(quiet_frame(4800).data(), 4800, true);
    REQUIRE(recovered.kind != AudioFindingKind::microphone_stalled);
}

TEST_CASE("audio session: the shipped stall bound is a time bound, not a retry count",
          "[audio][session]") {
    // Roughly ten seconds at the 200 ms read pace. A three-read bound once turned a momentary
    // gap in the sound-server graph into a hardware verdict, and the caller then sent SIGTERM to
    // a healthy capture child. Asserting the shipped default keeps a future edit from quietly
    // undoing that.
    const AudioActivitySession::Config config{};
    REQUIRE(config.stalled_after_empty_windows >= 40);
}

TEST_CASE("audio session: unusable windows are refused with a reason, never scored",
          "[audio][session]") {
    Logger::initialize("error");
    AudioActivitySession session(AudioActivitySession::Config{}, [] { return 1700000000000; });
    const std::vector<int16_t> tiny(4, 100);  // far below the extractor's minimum
    const AudioFinding finding = session.observe(tiny.data(), tiny.size(), true);
    REQUIRE_FALSE(finding.valid);
    REQUIRE_FALSE(finding.reason.empty());
    REQUIRE(finding.confidence == 0.0);
    REQUIRE(finding.kind == AudioFindingKind::insufficient_evidence);
}

TEST_CASE("audio session: provenance is carried from the caller, never inferred",
          "[audio][session]") {
    Logger::initialize("error");
    int64_t now = 1700000000000;
    AudioActivitySession session(AudioActivitySession::Config{}, [&now] { return now; });
    // Identical samples, one declared real and one declared synthetic: the finding must
    // follow the CALLER's statement, because only the caller knows where the bytes came from.
    const auto samples = tone_frame(4800, 0.3);
    const AudioFinding as_synthetic = session.observe(samples.data(), samples.size(), false);
    REQUIRE_FALSE(as_synthetic.real_source);
    session.reset();
    const AudioFinding as_real = session.observe(samples.data(), samples.size(), true);
    REQUIRE(as_real.real_source);
    REQUIRE(as_synthetic.kind == as_real.kind);
}

TEST_CASE("audio session: the explanation vocabulary cannot name a species or an event",
          "[audio][session][vocabulary]") {
    Logger::initialize("error");
    // The project's standing rule: make an unacceptable claim unrepresentable. Every
    // sentence this component can emit is scanned here, so a future edit that adds a
    // species, an emotion or a disaster claim fails this test rather than shipping.
    const char* forbidden[] = {"dog",     "cat",      "rat",       "mouse",   "bird",
                               "cockroach", "intruder", "burglar",  "fall",    "fire",
                               "smoke",   "earthquake", "flood",   "speech",  "voice",
                               "person",  "alarm",    "emergency", "danger",  "weapon"};
    AudioActivitySession::Config config;
    int64_t now = 1700000000000;
    AudioActivitySession session(config, [&now] { return now; });

    std::vector<std::string> sentences;
    for (int i = 0; i < 14; ++i) {
        now += 100;
        sentences.push_back(session.observe(quiet_frame(4800).data(), 4800, true).sentence);
    }
    for (int i = 0; i < 14; ++i) {
        now += 100;
        sentences.push_back(session.observe(tone_frame(4800, 0.6).data(), 4800, true).sentence);
    }
    for (int i = 0; i < 4; ++i) sentences.push_back(session.observe_no_samples().sentence);
    sentences.push_back(session.observe(std::vector<int16_t>(3, 0).data(), 3, true).sentence);

    for (const std::string& sentence : sentences) {
        REQUIRE_FALSE(sentence.empty());
        std::string lowered = sentence;
        for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* word : forbidden) {
            INFO("sentence: " << sentence << " / forbidden: " << word);
            REQUIRE(lowered.find(word) == std::string::npos);
        }
    }
}

TEST_CASE("microphone capture refuses to start without explicit operator authorization",
          "[audio][capture][safety]") {
    Logger::initialize("error");
    PwAudioCaptureDevice::Config config;
    config.dry_run = false;  // the refusal must be tested where a real spawn WOULD happen
    PwAudioCaptureDevice device(config);

    device.initialize();
    // THE SAFETY PROPERTY: constructing and initializing a microphone is not consent, and
    // neither is the OS letting the process reach a sound server. start() must refuse.
    REQUIRE_FALSE(device.capture_authorized());
    REQUIRE_THROWS(device.start());
    REQUIRE(device.state() != DeviceState::Capturing);
    REQUIRE(device.read_frames(nullptr, 0) == 0);

    // The operator grants capture, then withdraws it: revocation is immediate.
    device.authorize_capture(true);
    REQUIRE(device.capture_authorized());
    device.authorize_capture(false);
    REQUIRE_FALSE(device.capture_authorized());
    REQUIRE(device.state() != DeviceState::Capturing);
}

TEST_CASE("microphone capture reports the exact command it would run, and dries out",
          "[audio][capture]") {
    Logger::initialize("error");

    SECTION("the default tool is parec, and the argv matches parec's own interface") {
        PwAudioCaptureDevice::Config config;
        config.device = "alsa_input.pci-0000_00_1b.0.analog-stereo";
        config.sample_rate = 48000;
        config.channels = 1;
        config.dry_run = true;  // never spawn: this test asserts the PLAN, not a capture
        PwAudioCaptureDevice device(config);

        const auto argv = device.capture_command_line();
        REQUIRE(argv.front() == "parec");
        // parec needs --raw; pw-record does NOT accept it (it exits "unrecognized option", and
        // the session then reports a dead microphone - Incident 54). The two tools take
        // different flags, so the builder is asserted for BOTH.
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--raw")) != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--format=s16le")) != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--rate=48000")) != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--channels=1")) != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(),
                          std::string("--device=") + config.device) != argv.end());
        // parec writes to stdout by default; it must NOT receive pw-record's trailing '-'.
        REQUIRE(argv.back() != "-");

        device.authorize_capture(true);
        device.initialize();
        REQUIRE_NOTHROW(device.start());
        REQUIRE(device.state() == DeviceState::Capturing);
        device.stop();
        REQUIRE(device.state() == DeviceState::Stopped);
        device.close();
        REQUIRE(device.state() == DeviceState::Idle);
        REQUIRE(device.stats().frames_captured == 0);  // dry_run captured nothing, and says so
    }

    SECTION("pw-record is supported with ITS flags, not parec's") {
        PwAudioCaptureDevice::Config config;
        config.capture_command = "pw-record";
        config.device = "alsa_input.pci-0000_00_1b.0.analog-stereo";
        config.dry_run = true;
        PwAudioCaptureDevice device(config);
        const auto argv = device.capture_command_line();
        REQUIRE(argv.front() == "pw-record");
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--raw")) == argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(), std::string("--format=s16")) != argv.end());
        REQUIRE(std::find(argv.begin(), argv.end(),
                          std::string("--target=") + config.device) != argv.end());
        REQUIRE(argv.back() == "-");  // pw-record needs the output named
    }

    SECTION("an output MONITOR is refused as a microphone") {
        PwAudioCaptureDevice::Config config;
        // A monitor is a loopback of what the machine is PLAYING, not room sound. Accepting it
        // would report the local speaker as the environment and feed announcements back into
        // detection.
        config.device = "alsa_output.pci-0000_00_1b.0.iec958-stereo.monitor";
        PwAudioCaptureDevice device(config);
        REQUIRE_FALSE(device.refused_reason().empty());
        device.authorize_capture(true);
        device.initialize();
        REQUIRE_THROWS(device.start());
        REQUIRE(device.state() != DeviceState::Capturing);
    }
}

TEST_CASE("speaker refuses to play while the microphone is live, and never claims to be Alexa",
          "[audio][speaker][safety]") {
    Logger::initialize("error");
    LocalSpeaker::Config config;
    config.volume_limit = 1.5;  // must be clamped, not honoured
    LocalSpeaker speaker(config);
    REQUIRE(speaker.volume_limit() == Approx(LocalSpeaker::kMaxVolumeLimit));
    speaker.set_volume_limit(std::nan(""));
    REQUIRE(speaker.volume_limit() == Approx(0.0));
    speaker.set_volume_limit(0.4);

    // Feedback guard: with the microphone live, a request is REFUSED (nothing is played).
    speaker.set_feedback_guard(true);
    REQUIRE_FALSE(speaker.play_tone(440.0, 100));
    REQUIRE_FALSE(speaker.speak("test"));
    REQUIRE(speaker.stats().refused == 2);

    // The engine identification is honest and brand-free.
    for (LocalSpeaker::JobKind kind : {LocalSpeaker::JobKind::tone, LocalSpeaker::JobKind::speech}) {
        const std::string engine = speaker.engine_name(kind);
        std::string lowered = engine;
        for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        REQUIRE(lowered.find("alexa") == std::string::npos);
        REQUIRE(lowered.find("amazon") == std::string::npos);
    }
    REQUIRE(speaker.engine_name(LocalSpeaker::JobKind::tone) == "homeguardian-generated-tone");
    std::string why;
    // spd-say exists on this host but has no voice backend, so the honest answer is not "yes".
    const std::string speech_engine = speaker.engine_available(LocalSpeaker::JobKind::speech, &why);
    if (speech_engine.empty()) REQUIRE_FALSE(why.empty());
}

TEST_CASE("the speaker queue is bounded and drops the oldest, not the newest",
          "[audio][speaker]") {
    Logger::initialize("error");
    LocalSpeaker::Config config;
    config.queue_capacity = 2;
    LocalSpeaker speaker(config);  // NOT started: nothing is played, nothing is spawned
    REQUIRE(speaker.play_tone(440.0, 50));
    REQUIRE(speaker.play_tone(660.0, 50));
    REQUIRE(speaker.play_tone(880.0, 50));
    REQUIRE(speaker.queued() == 2);
    REQUIRE(speaker.stats().dropped == 1);
    speaker.cancel();
    REQUIRE(speaker.queued() == 0);
}
