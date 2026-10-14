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

int main(int argc, char** argv) {
    Logger::initialize("warn");

    bool probe = false;
    bool test_tone = false;
    bool start = false;
    bool confirmed = false;
    bool speak = false;
    int seconds = 0;  // 0 = run until interrupted
    std::string device_name;
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

    if (!probe && !test_tone && !start) {
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
    LocalSpeaker speaker(speaker_config);

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

    // Output while the microphone is live would be a feedback loop, so it is suppressed by
    // the speaker's own guard and only utterances that MATTER are queued after capture stops.
    speaker.set_feedback_guard(true);
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

    while (!g_stop_requested.load()) {
        if (seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (elapsed >= seconds) break;
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
        if (finding.kind == AudioFindingKind::sustained_increase ||
            finding.kind == AudioFindingKind::microphone_stalled) {
            last_interesting = finding;
            have_interesting = true;
        }
        if (finding.kind == AudioFindingKind::microphone_stalled) {
            std::cerr << "capture device stopped delivering samples; stopping.\n";
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
