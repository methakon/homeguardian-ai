// hg_audio_bridge - the host-side, permission-gated audio service.
//
// WHY THIS PROCESS EXISTS (milestone section E)
// ---------------------------------------------
// Audio hardware is reached from a SMALL HOST-SIDE PROCESS, never from a container. The
// Docker hub therefore keeps zero audio privileges: no `--privileged`, no `/dev` mount, no
// Docker socket, no PipeWire/PulseAudio socket inside the container, no host home directory.
// The hub receives only audio-DERIVED observations. The alternatives were rejected:
//   * mounting /dev/snd into the hub      -> gives the network-facing process raw device access
//   * mounting the PulseAudio socket      -> the container could capture anything, any time
//   * --privileged                        -> strictly worse than either
// This process holds the microphone permission, computes bounded scalars, and hands the
// scalars - never raw audio - onward.
//
// OPERATOR CONTROL (milestone section C/J)
// ----------------------------------------
//   * capture REQUIRES the explicit `--start` flag AND the confirmation string
//     `--i-understand-this-opens-the-microphone`. Construction, discovery and `--probe` never
//     open the microphone.
//   * Ctrl-C (or `--seconds N`) stops capture and releases the device.
//   * A visible, continuously printed capture indicator shows that capture is active.
//   * No raw audio is written to disk. No transcription. No cloud transmission.
//
// USAGE
//   hg_audio_bridge --probe                     # enumerate devices, capture nothing
//   hg_audio_bridge --test-tone                 # play a short generated tone (output only)
//   hg_audio_bridge --start --i-understand-this-opens-the-microphone [--seconds N] [--speak]
//
// NOT AMAZON ALEXA. This program speaks through the local engine it names on stdout
// (`homeguardian-generated-tone` or `speech-dispatcher(spd-say)`) and contains no Alexa
// branding. Genuine Alexa would be a separate authenticated runtime.

#include "audio/AudioActivitySession.h"
#include "backend/linux/LocalSpeaker.h"
#include "voice/VoiceResponseController.h"
#include "backend/linux/PwAudioCaptureDevice.h"
#include "backend/linux/SensorDiscovery.h"
#include "core/Logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace homeguardian;

namespace {

std::atomic<bool> g_stop_requested{false};

void on_signal(int) { g_stop_requested.store(true); }

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void print_devices() {
    LinuxSensorDiscovery discovery;
    const auto microphones = discovery.list_microphones();
    const auto speakers = discovery.list_speakers();
    std::cout << "microphones (" << microphones.size() << "):\n";
    for (const auto& device : microphones) {
        std::cout << "  " << device.id << "  [" << device.driver << "] " << device.name << "\n";
    }
    std::cout << "speakers (" << speakers.size() << "):\n";
    for (const auto& device : speakers) {
        std::cout << "  " << device.id << "  [" << device.driver << "] " << device.name << "\n";
    }
}

// One finding as a single JSON line: the interface the hub path consumes. Scalars only -
// no audio, no transcript, no identification.
std::string finding_json(const AudioFinding& finding, const std::string& device_name) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << "{\"kind\":\"" << audio_finding_kind_to_string(finding.kind) << "\""
        << ",\"valid\":" << (finding.valid ? "true" : "false")
        << ",\"real_source\":" << (finding.real_source ? "true" : "false")
        << ",\"provenance\":\"" << (finding.real_source ? "real" : "synthetic") << "\""
        << ",\"confidence\":" << finding.confidence
        << ",\"sentence\":\"" << json_escape(finding.sentence) << "\""
        << ",\"uncertainty\":\"" << json_escape(finding.uncertainty) << "\""
        << ",\"rms\":" << finding.features.rms
        << ",\"peak\":" << finding.features.peak
        << ",\"zero_crossing_rate\":" << finding.features.zero_crossing_rate
        << ",\"baseline_ready\":" << (finding.baseline_ready ? "true" : "false")
        << ",\"baseline_level\":" << finding.baseline_level
        << ",\"recent_level\":" << finding.recent_level
        << ",\"ratio\":" << finding.ratio
        << ",\"sustained\":" << (finding.sustained ? "true" : "false")
        << ",\"statistical_deviation\":" << (finding.statistical_deviation ? "true" : "false")
        << ",\"windows_observed\":" << finding.windows_observed
        << ",\"silent_windows\":" << finding.silent_windows
        << ",\"reason\":\"" << json_escape(finding.reason) << "\""
        << ",\"device\":\"" << json_escape(device_name) << "\""
        << ",\"not_transcribed\":true"
        << ",\"source_identified\":false"
        << "}";
    return out.str();
}

void print_usage(const char* argv0) {
    std::cout << "usage:\n"
              << "  " << argv0 << " --probe                 enumerate devices (captures nothing)\n"
              << "  " << argv0 << " --test-tone             play a short generated tone\n"
              << "  " << argv0
              << " --start --i-understand-this-opens-the-microphone [--seconds N] [--speak]\n"
              << "  " << argv0 << " --help\n\n"
              << "Capture REQUIRES the confirmation flag: this program will not open a\n"
              << "microphone because it was asked to run.\n";
}

}  // namespace

// The bounded wait for a spoken sentence to complete. Long enough for a real synthesizer on a
// Bluetooth sink to finish a short sentence, short enough that a hung engine cannot hang the
// caller forever.
constexpr int kSpeechWaitMs = 20000;

int main(int argc, char** argv) {
    Logger::initialize("warn");

    bool probe = false;
    bool test_tone = false;
    bool start = false;
    bool confirmed = false;
    bool speak = false;
    bool voice = false;          // announce findings while capturing (opt-in)
    bool no_voice = false;       // explicit: capture without announcements
    bool mute = false;
    bool unmute = false;
    bool speak_test = false;
    bool voice_status = false;
    int voice_rate = 0;          // 0 = the adapter's default
    double voice_volume = -1.0;  // < 0 = the adapter's default
    int seconds = 0;  // 0 = run until interrupted
    std::string device_name;     // the microphone SOURCE
    std::string sink_name;       // the OUTPUT sink
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--probe") {
            probe = true;
        } else if (arg == "--test-tone") {
            test_tone = true;
        } else if (arg == "--start") {
            start = true;
        } else if (arg == "--i-understand-this-opens-the-microphone") {
            confirmed = true;
        } else if (arg == "--speak") {
            speak = true;
        } else if (arg == "--voice") {
            voice = true;
        } else if (arg == "--no-voice") {
            no_voice = true;
        } else if (arg == "--mute") {
            mute = true;
        } else if (arg == "--unmute") {
            unmute = true;
        } else if (arg == "--speak-test") {
            speak_test = true;
        } else if (arg == "--voice-status") {
            voice_status = true;
        } else if (arg == "--voice-rate" && i + 1 < argc) {
            voice_rate = std::atoi(argv[++i]);
        } else if (arg == "--voice-volume" && i + 1 < argc) {
            voice_volume = std::atof(argv[++i]);
        } else if (arg == "--sink" && i + 1 < argc) {
            sink_name = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            device_name = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (no_voice) voice = false;
    if (voice) speak = true;  // announcing while listening implies speaking at all

    if (!probe && !test_tone && !start && !speak_test && !voice_status) {
        print_usage(argv[0]);
        return 2;
    }

    if (probe) {
        print_devices();
        std::cout << "probe complete: no microphone was opened and nothing was captured.\n";
        return 0;
    }

    LocalSpeaker::Config speaker_config;
    speaker_config.volume_limit = 0.4;  // modest by default; the class clamps regardless
    if (voice_volume >= 0.0) speaker_config.volume_limit = voice_volume;
    if (voice_rate > 0) speaker_config.rate = voice_rate;
    if (!sink_name.empty()) speaker_config.device = sink_name;  // the OUTPUT device
    LocalSpeaker speaker(speaker_config);

    VoiceResponseController::Config voice_config;
    voice_config.enabled = voice;  // no --voice means announcements are OFF, and status says so
    if (voice_rate > 0) voice_config.rate = voice_rate;
    VoiceResponseController voice_control(voice_config, &speaker);

    // --- operator-visible voice status ---------------------------------------------------
    if (voice_status) {
        const auto status = voice_control.status(0);
        std::string why;
        const bool available = voice_control.speech_available(&why);
        // Report the engine that can ACTUALLY run, or "(none available)": naming the
        // last-resort program while it is unusable would read as a working engine.
        std::string engine_why;
        const std::string real_engine = speaker.engine_available(LocalSpeaker::JobKind::speech,
                                                                 &engine_why);
        std::cout << "{\"voice_enabled\":" << (status.enabled ? "true" : "false")
                  << ",\"engine\":\""
                  << json_escape(real_engine.empty() ? std::string("(none available)") : real_engine)
                  << "\""
                  << ",\"speech_available\":" << (available ? "true" : "false")
                  << ",\"engine_why\":\"" << json_escape(status.engine_why) << "\""
                  << ",\"output_device\":\""
                  << json_escape(status.device.empty() ? "(system default)" : status.device)
                  << "\""
                  << ",\"device_present\":" << (speaker.output_present(nullptr) ? "true" : "false")
                  << ",\"muted\":" << (speaker.muted() ? "true" : "false")
                  << ",\"volume_limit\":" << speaker.volume_limit()
                  << ",\"last_spoken\":\"" << json_escape(status.last_message) << "\""
                  << ",\"last_failure\":\"" << json_escape(status.last_failure) << "\""
                  << ",\"spoken\":" << status.spoken
                  << ",\"suppressed\":" << status.suppressed
                  << ",\"refused\":" << status.refused
                  << ",\"not_alexa\":true}\n";
        return 0;
    }

    // --- the operator's explicit speech test ---------------------------------------------
    if (speak_test) {
        const std::string sentence =
            "HomeGuardian local voice feedback test is successful.";
        std::cout << "engine: " << speaker.engine_name(LocalSpeaker::JobKind::speech)
                  << "\noutput: "
                  << (sink_name.empty() ? std::string("(system default sink)") : sink_name)
                  << "\n";
        std::string why;
        if (!speaker.output_present(&why)) {
            std::cerr << "the output device could not be confirmed: " << why << "\n";
        }
        if (!voice_control.speech_available(&why)) {
            // Refuse honestly: no engine, no sentence. An exit code of zero here would be a lie.
            std::cerr << "speech is NOT available: " << why << "\n"
                      << "nothing was played. Install a synthesizer (e.g. espeak-ng) and re-run.\n";
            return 1;
        }
        speaker.start();
        // COMPLETION IS TRACKED, NOT GUESSED. `busy()` is false between the enqueue and the
        // worker taking the job, so waiting on it returned instantly and the old code then
        // stopped the speaker - cancelling a sentence that had not started. Wait for the
        // monotone completion index instead, with a bounded deadline.
        const uint64_t marker = speaker.completion_marker();
        const uint64_t played_before = speaker.stats().played;
        int64_t now = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (!voice_control.speak_operator_text(sentence, now, &why)) {
            std::cerr << "the sentence was refused: " << why << "\n";
            speaker.stop();
            return 1;
        }
        const bool completed = speaker.wait_for_completion(marker + 1, kSpeechWaitMs);
        const uint64_t played_now = speaker.stats().played;
        const uint64_t refused_now = speaker.stats().refused;
        const int last_exit = speaker.stats().last_exit_code;
        speaker.stop();  // joins the worker; nothing is left running
        std::cout << "engine: " << speaker.engine_name(LocalSpeaker::JobKind::speech) << "\n"
                  << "sentence: \"" << sentence << "\"\n"
                  << "completed_within_wait=" << (completed ? "true" : "false")
                  << " playback_jobs=" << played_now
                  << " (before=" << played_before << ")"
                  << " refused=" << refused_now
                  << " last_exit=" << last_exit << "\n";
        if (!completed) {
            std::cerr << "the sentence did not finish within " << kSpeechWaitMs
                      << " ms; nothing is claimed about it.\n";
            return 1;
        }
        if (played_now <= played_before) {
            // ZERO PLAYBACK IS NEVER SUCCESS, whatever the exit status of anything involved.
            std::cerr << "no playback job completed: the sentence was NOT played (refused="
                      << refused_now << ", last_exit=" << last_exit << ")\n";
            return 1;
        }
        std::cout << "OPERATOR: this exit status is NOT evidence of audibility - please confirm\n"
                  << "whether you heard the sentence, and report it as confirmed or not.\n";
        return 0;
    }

    if (test_tone) {
        // An OUTPUT-only test the operator explicitly initiates. No capture is involved.
        std::cout << "engine: " << speaker.engine_name(LocalSpeaker::JobKind::tone) << "\n";
        std::cout << "playing a 440 Hz tone for 400 ms at volume limit "
                  << speaker.volume_limit() << " (safe, short, generated locally)\n";
        speaker.start();
        if (!speaker.play_tone(440.0, 400)) {
            std::cerr << "the tone was refused\n";
            speaker.stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        speaker.stop();
        std::cout << "tone played=" << speaker.stats().played
                  << " refused=" << speaker.stats().refused << "\n";
        return speaker.stats().played > 0 ? 0 : 1;
    }

    if (mute) speaker.set_muted(true);
    if (unmute) speaker.set_muted(false);

    if (!confirmed) {
        std::cerr << "refusing to open the microphone: --start requires the explicit "
                     "confirmation flag --i-understand-this-opens-the-microphone.\n"
                     "Run --probe first to see the devices, then re-run with the flag when you\n"
                     "are ready for capture to begin. Nothing was opened.\n";
        return 3;
    }

    PwAudioCaptureDevice::Config capture_config;
    capture_config.sample_rate = 48000;
    capture_config.channels = 1;
    capture_config.device = device_name;
    PwAudioCaptureDevice microphone(capture_config);

    std::cout << "device: " << microphone.name() << "\n";
    microphone.authorize_capture(true);  // the operator's explicit action, above
    microphone.initialize();
    try {
        microphone.start();
    } catch (const std::exception& e) {
        std::cerr << "capture did not start: " << e.what() << "\n";
        return 4;
    }

    AudioActivitySession session(AudioActivitySession::Config{},
                                 [] { return static_cast<int64_t>(
                                          std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count()); });

    // FEEDBACK CONTROL. Without --voice the old conservative default stands: nothing is played
    // while the microphone is live (the speaker refuses outright). WITH --voice the operator has
    // asked for spoken findings while listening, so playback is allowed and the feedback is
    // handled by a BOUNDED suppression window instead: our own announcement is not fed back
    // into the activity model as if it were independent evidence. This is suppression, not
    // acoustic echo cancellation - see docs/LOCAL_VOICE_FEEDBACK_H02A.md section 6.
    if (!voice) speaker.set_feedback_guard(true);
    speaker.start();

    std::cout << "*** CAPTURE ACTIVE: the microphone is open and delivering samples. ***\n"
              << "*** Press Ctrl-C (or wait for --seconds) to stop. Raw audio is not saved. ***\n";

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const auto started = std::chrono::steady_clock::now();
    std::vector<int16_t> buffer(4800);
    std::string last_sentence;
    AudioFinding last_interesting;
    bool have_interesting = false;
    uint64_t windows = 0;
    uint64_t suppressed_windows = 0;  // skipped because our own announcement was playing

    while (!g_stop_requested.load()) {
        if (seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (elapsed >= seconds) break;
        }
        const int64_t now_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (voice && voice_control.input_suppressed(now_ms)) {
            // The speaker is (probably) still talking. Do NOT feed our own sentence into the
            // activity model: that would let an announcement look like independent evidence.
            // Reported separately from a stalled microphone, and it ends with the window.
            ++suppressed_windows;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const size_t frames = microphone.read_frames(buffer.data(), buffer.size());
        AudioFinding finding;
        if (frames == 0) {
            finding = session.observe_no_samples();
        } else {
            finding = session.observe(buffer.data(), frames, /*real_source=*/true);
        }
        ++windows;
        if (finding.sentence != last_sentence) {
            std::cout << finding_json(finding, microphone.name()) << "\n" << std::flush;
            last_sentence = finding.sentence;
        }
        if (voice) {
            // The REAL finding decides the sentence (closed vocabulary), and the policy decides
            // whether now is the moment. The output only ENQUEUES, so the detection loop is
            // never blocked by playback.
            const std::string before_sentence = voice_control.status(now_ms).last_message;
            if (voice_control.announce_finding(finding, now_ms)) {
                // Evidence, not inference: say WHICH approved sentence went to the speaker, so a
                // transcript can be checked against the finding that produced it. This is the
                // phrasebook's wording, NOT the dashboard sentence in the JSON above.
                std::cout << "voice_spoke: \""
                          << voice_control.status(now_ms).last_message << "\"\n" << std::flush;
            } else if (voice_control.status(now_ms).last_message != before_sentence) {
                std::cout << "voice_spoke: \""
                          << voice_control.status(now_ms).last_message << "\"\n" << std::flush;
            }
        }
        if (finding.kind == AudioFindingKind::sustained_increase ||
            finding.kind == AudioFindingKind::microphone_stalled) {
            last_interesting = finding;
            have_interesting = true;
        }
        if (finding.kind == AudioFindingKind::microphone_stalled) {
            // Report WHAT the capture child actually did before giving up. A stall message
            // without the child's own words is the kind of vague equipment claim that costs a
            // debugging cycle (see the pw-record --raw incident).
            std::cerr << "capture device stopped delivering samples; stopping.\n"
                      << "  last_error: "
                      << (microphone.last_error().empty() ? "(none reported)"
                                                          : microphone.last_error())
                      << "\n"
                      << "  child exit status: " << microphone.stats().last_exit_code
                      << ", frames=" << microphone.stats().frames_captured
                      << ", disconnects=" << microphone.stats().disconnects << "\n";
            break;
        }
    }

    // Stop capture FIRST, then allow speech: the guard is released only once the microphone
    // is closed, so the speaker can never talk into a live microphone.
    microphone.authorize_capture(false);
    microphone.close();
    std::cout << "\ncapture stopped: windows=" << windows
              << " frames=" << microphone.stats().frames_captured
              << " dropped=" << microphone.stats().frames_dropped
              << " disconnects=" << microphone.stats().disconnects << "\n";
    std::cout << "*** CAPTURE INACTIVE: the microphone is closed and no further samples "
                 "are being read. ***\n";

    // Shutdown path: stop any playback and never leave a child running.
    voice_control.stop();
    if (voice) {
        const auto status = voice_control.status(0);
        // Name only an engine that can REALLY run, exactly as --voice-status does.
        std::string summary_why;
        const std::string usable_engine =
            speaker.engine_available(LocalSpeaker::JobKind::speech, &summary_why);
        std::cout << "voice: spoken=" << status.spoken << " suppressed=" << status.suppressed
                  << " refused=" << status.refused
                  << " suppressed_input_windows=" << suppressed_windows
                  << " engine=" << (usable_engine.empty() ? std::string("(none available)")
                                                          : usable_engine)
                  << "\n";
    }

    if (speak && have_interesting) {
        speaker.set_feedback_guard(false);
        const std::string spoken = last_interesting.sentence + " " + last_interesting.uncertainty;
        std::string why;
        const std::string engine = speaker.engine_available(LocalSpeaker::JobKind::speech, &why);
        if (engine.empty()) {
            std::cerr << "spoken feedback unavailable: " << why << "\n";
        } else {
            std::cout << "speaking via " << speaker.engine_name(LocalSpeaker::JobKind::speech)
                      << ": " << spoken << "\n";
            speaker.speak(spoken);
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        }
    }
    speaker.stop();
    return 0;
}
