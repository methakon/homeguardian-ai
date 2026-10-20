// hg_audio_probe: one bounded, locally processed microphone window, printed as bounded scalars.
//
// WHAT IT DOES NOT DO: it does not record. There is no file argument in the capture command it builds,
// no audio is retained, and what it prints is the closed-form feature set (RMS, peak, zero-crossing
// rate) plus an observation carrying real provenance. A microphone test you cannot audit is not
// evidence, so the command it actually ran is printed too.
//
// Usage: hg_audio_probe [--device NAME] [--seconds 1] [--check-refusal]

#include "backend/linux/AlsaCommandAudioSource.h"
#include "core/AudioObservation.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace homeguardian;

int main(int argc, char** argv) {
    AlsaCommandAudioSource::Config cfg;
    bool check_refusal = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            cfg.device = argv[++i];
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            cfg.seconds = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--check-refusal") == 0) {
            check_refusal = true;
        }
    }

    AlsaCommandAudioSource source(cfg);
    std::printf("source      : %s\n", source.platform().c_str());
    std::printf("device      : %s (%u Hz, %u ch, %u s -> %zu frames)\n", cfg.device.c_str(),
                cfg.sample_rate, cfg.channels, cfg.seconds,
                static_cast<size_t>(cfg.seconds) * cfg.sample_rate);

    std::string error;
    if (!source.open(&error)) {
        std::printf("REFUSED     : %s\n", error.c_str());
        return 2;
    }

    std::printf("capture cmd :");
    for (const auto& a : source.capture_argv()) std::printf(" %s", a.c_str());
    std::printf("\n              (no file argument: raw PCM arrives on the pipe)\n");

    std::vector<int16_t> pcm;
    size_t frames = 0;
    uint32_t channels = 0, rate = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = source.read_window(pcm, &frames, &channels, &rate);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    if (!ok) {
        std::printf("NO WINDOW   : the capture produced nothing usable (not a quiet room)\n");
        return 3;
    }
    std::printf("window      : %zu frames, %u ch, %u Hz, %lld ms wall%s\n", frames, channels, rate,
                static_cast<long long>(elapsed_ms), source.last_read_was_short() ? " (SHORT READ)" : "");

    const auto observation =
        observe_audio_window(pcm.data(), frames, channels, rate, Provenance::real, source.platform());
    if (!observation.valid) {
        std::printf("REFUSED     : the window could not be reduced: %s\n", observation.reason.c_str());
        return 4;
    }
    std::printf("features    : rms=%.4f peak=%.4f zcr=%.4f\n", observation.features.rms,
                observation.features.peak, observation.features.zero_crossing_rate);
    std::printf("event       : %s\n", observation.event.c_str());
    std::printf("uncertainty : %s\n", observation.uncertainty.c_str());
    std::printf("provenance  : %s\n", provenance_to_string(observation.provenance));
    std::printf("json        : %s\n", audio_observation_to_json(observation).dump().c_str());

    if (check_refusal) {
        // A device that does not exist must be refused, not silently replaced by the default one. This is
        // the same machine, the same code path, and it proves the refusal on real hardware.
        AlsaCommandAudioSource::Config bad;
        bad.device = "hw:99,99";
        AlsaCommandAudioSource bad_source(bad);
        std::vector<int16_t> other;
        size_t other_frames = 0;
        uint32_t other_channels = 0, other_rate = 0;
        std::string bad_error;
        const bool opened = bad_source.open(&bad_error);
        const bool read = opened && bad_source.read_window(other, &other_frames, &other_channels,
                                                           &other_rate);
        std::printf("refusal test: open=%s read=%s -> %s\n", opened ? "ok" : "refused",
                    read ? "a window" : "NO WINDOW",
                    read ? "FAILED: a nonexistent device produced audio"
                         : "PASS: a nonexistent device produced no window");
        if (read) return 5;
    }
    return 0;
}
