// test_audio_adapter: the Linux microphone adapter, driven by an injected command runner.
//
// No microphone is opened here, and that is the point: every path that matters - the tool missing, no
// capture hardware, an unbounded window, a short read, an odd byte count, a stream that never ends - is
// reachable with a fake runner, so the suite proves the REFUSALS without depending on this machine
// having a working microphone. One test does drive the real fork/exec runner, with `true` and a command
// that cannot exist, because that is the part a fake cannot exercise.

#include <catch2/catch.hpp>

#include "backend/linux/AlsaCommandAudioSource.h"
#include "core/AudioObservation.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

// Deterministic S16_LE bytes: a 440 Hz tone at 48 kHz mono.
std::string pcm_bytes(size_t frames, uint32_t channels = 1, double amplitude = 8000.0) {
    std::string out;
    out.reserve(frames * channels * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(2.0 * 3.14159265358979 * 440.0 * double(i) / 48000.0);
        const int16_t sample = int16_t(v);
        for (uint32_t c = 0; c < channels; ++c) {
            out.push_back(char(sample & 0xFF));
            out.push_back(char((sample >> 8) & 0xFF));
        }
    }
    return out;
}

struct FakeRunner {
    CommandResult next{};
    std::vector<std::vector<std::string>> calls;
    size_t max_bytes_seen = 0;

    CommandRunner fn() {
        return [this](const std::vector<std::string>& argv, size_t max_bytes) {
            calls.push_back(argv);
            max_bytes_seen = max_bytes;
            return next;
        };
    }
};

std::string device_listing() {
    return "**** List of CAPTURE Hardware Devices ****\n"
           "card 0: PCH [HDA Intel PCH], device 0: ALC897 Analog [ALC897 Analog]\n";
}

}  // namespace

TEST_CASE("adapter: open refuses what it cannot honestly capture", "[audio][adapter]") {
    SECTION("the tool is missing") {
        auto fake = FakeRunner{};
        fake.next = CommandResult{127, "", "", false};
        AlsaCommandAudioSource source({.runner = fake.fn()});
        std::string error;
        REQUIRE_FALSE(source.open(&error));
        REQUIRE(error.find("unavailable") != std::string::npos);
    }
    SECTION("there is no capture hardware") {
        auto fake = FakeRunner{};
        fake.next = CommandResult{0, "**** List of CAPTURE Hardware Devices ****\n", "", false};
        AlsaCommandAudioSource source({.runner = fake.fn()});
        std::string error;
        REQUIRE_FALSE(source.open(&error));
        REQUIRE(error.find("no capture hardware") != std::string::npos);
    }
    SECTION("the window is longer than the extractor accepts") {
        // The adapter's first version defaulted to 2 s and produced frames the extractor refuses, so the
        // window length is validated here, at open, with the bound named.
        auto fake = FakeRunner{};
        fake.next = CommandResult{0, device_listing(), "", false};
        auto cfg = AlsaCommandAudioSource::Config{};
        cfg.runner = fake.fn();
        cfg.seconds = 2;
        AlsaCommandAudioSource source(cfg);
        std::string error;
        REQUIRE_FALSE(source.open(&error));
        REQUIRE(error.find("48000") != std::string::npos);
        REQUIRE(error.find("repeated windows") != std::string::npos);
    }
    SECTION("the window is unbounded") {
        auto fake = FakeRunner{};
        fake.next = CommandResult{0, device_listing(), "", false};
        auto cfg = AlsaCommandAudioSource::Config{};
        cfg.runner = fake.fn();
        cfg.seconds = 0;
        AlsaCommandAudioSource source(cfg);
        std::string error;
        REQUIRE_FALSE(source.open(&error));
        REQUIRE(error.find("unbounded") != std::string::npos);
    }
    SECTION("a healthy machine opens") {
        auto fake = FakeRunner{};
        fake.next = CommandResult{0, device_listing(), "", false};
        AlsaCommandAudioSource source({.runner = fake.fn()});
        std::string error;
        REQUIRE(source.open(&error));
        REQUIRE(error.empty());
        REQUIRE(source.platform() == "alsa-command");
    }
}

TEST_CASE("adapter: the window is bounded and nothing is written to disk", "[audio][adapter]") {
    auto fake = FakeRunner{};
    fake.next = CommandResult{0, device_listing(), "", false};
    AlsaCommandAudioSource source({.runner = fake.fn()});
    std::string error;
    REQUIRE(source.open(&error));

    const auto argv = source.capture_argv();
    // No file argument anywhere: raw PCM goes to the pipe, which is why no audio is stored.
    REQUIRE(argv.back() == "-");
    bool saw_raw = false;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == "-t" && argv[i + 1] == "raw") saw_raw = true;
    }
    REQUIRE(saw_raw);
    for (const auto& a : argv) {
        REQUIRE(a.find(".raw") == std::string::npos);
        REQUIRE(a.find(".wav") == std::string::npos);
    }
    // The capture is a fixed duration, not an open-ended stream.
    bool saw_duration = false;
    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        if (argv[i] == "-d") saw_duration = true;
    }
    REQUIRE(saw_duration);
}

TEST_CASE("adapter: a good window yields bounded features", "[audio][adapter]") {
    auto fake = FakeRunner{};
    fake.next = CommandResult{0, device_listing(), "", false};
    AlsaCommandAudioSource source({.runner = fake.fn()});
    std::string error;
    REQUIRE(source.open(&error));

    fake.next = CommandResult{0, pcm_bytes(48000), "", false};      // exactly 1 s at 48 kHz mono
    std::vector<int16_t> pcm;
    size_t frames = 0;
    uint32_t channels = 0, rate = 0;
    REQUIRE(source.read_window(pcm, &frames, &channels, &rate));
    REQUIRE(frames == 48000);
    REQUIRE(channels == 1);
    REQUIRE(rate == 48000);
    REQUIRE_FALSE(source.last_read_was_short());

    const auto observation =
        observe_audio_window(pcm.data(), frames, channels, rate, Provenance::real, source.platform());
    REQUIRE(observation.valid);
    REQUIRE(observation.features.rms > 0.1);
    REQUIRE(observation.provenance == Provenance::real);
    REQUIRE(observation.source == "alsa-command");
}

TEST_CASE("adapter: malformed and over-long windows are refused, not averaged", "[audio][adapter]") {
    auto fake = FakeRunner{};
    fake.next = CommandResult{0, device_listing(), "", false};
    AlsaCommandAudioSource source({.runner = fake.fn()});
    std::string error;
    REQUIRE(source.open(&error));
    std::vector<int16_t> pcm;
    size_t frames = 0;
    uint32_t channels = 0, rate = 0;

    SECTION("an odd byte count cannot be S16_LE") {
        fake.next = CommandResult{0, std::string(4001, 'x'), "", false};
        REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));
    }
    SECTION("an empty capture is not a quiet room") {
        fake.next = CommandResult{0, "", "", false};
        REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));
    }
    SECTION("a stream that hit the byte bound is refused rather than silently shortened") {
        fake.next = CommandResult{0, pcm_bytes(48000), "", true};
        REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));
    }
    SECTION("a failed capture process is no window") {
        fake.next = CommandResult{1, "", "device busy", false};
        REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));
    }
    SECTION("a device that disappears mid-session is contained to one window") {
        // 0.5 s of a 1 s window: the first version of this fixture used 48000 frames, which IS the whole
        // window and therefore not short at all - the adapter was right and the test was not.
        fake.next = CommandResult{0, pcm_bytes(24000), "", false};
        REQUIRE(source.read_window(pcm, &frames, &channels, &rate));
        REQUIRE(frames == 24000);
        REQUIRE(source.last_read_was_short());
        fake.next = CommandResult{1, "", "device disappeared", false};  // and now it is gone
        REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));
    }
    SECTION("reading before open is refused") {
        AlsaCommandAudioSource unopened({.runner = fake.fn()});
        REQUIRE_FALSE(unopened.read_window(pcm, &frames, &channels, &rate));
    }
}

TEST_CASE("adapter: the real runner is bounded and reports what a shell would",
          "[audio][adapter][real]") {
    // The fake cannot exercise fork/exec, so the real runner is used directly - with commands that
    // cannot touch a microphone.
    const auto ok = run_command_piped({"/bin/true"}, 1024);
    REQUIRE(ok.exit_code == 0);
    REQUIRE(ok.output.empty());

    const auto missing = run_command_piped({"hg-no-such-command-exists"}, 1024);
    REQUIRE(missing.exit_code == 127);        // "command not found", as a shell reports it

    const auto bounded = run_command_piped({"/bin/sh", "-c", "yes bounded | head -c 100000"}, 4096);
    REQUIRE(bounded.truncated);               // a stream that keeps coming is cut off
    REQUIRE(bounded.output.size() <= 70000);  // and never unbounded
}
