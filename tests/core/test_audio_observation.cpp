// test_audio_observation: the audio half of the platform-neutral sensor event contract.
//
// Fixtures are synthetic PCM, so the ground truth is exact and the tests assert what the contract
// promises: a bounded measurement, a closed vocabulary, provenance carried from the caller, and a JSON
// shape the fusion engine can read without knowing which platform produced the samples.

#include <catch2/catch.hpp>

#include "core/AudioObservation.h"
#include "core/FusionEngine.h"
#include "core/Observation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

// A windowed tone: deterministic, and clearly not silence.
std::vector<int16_t> tone(size_t frames, uint32_t channels, double amplitude, double hz, uint32_t rate) {
    std::vector<int16_t> pcm(frames * channels, 0);
    for (size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(2.0 * 3.14159265358979 * hz * double(i) / double(rate));
        for (uint32_t c = 0; c < channels; ++c) {
            pcm[i * channels + c] = int16_t(v);
        }
    }
    return pcm;
}

struct FakeSource : IAudioObservationSource {
    std::string platform() const override { return "test"; }
    bool open(std::string* error) override { error->clear(); return true; }
    void close() override {}
    bool read_window(std::vector<int16_t>& pcm, size_t* frames, uint32_t* channels,
                     uint32_t* sample_rate) override {
        pcm = tone(960, 1, 8000.0, 440.0, 48000);
        *frames = 960;
        *channels = 1;
        *sample_rate = 48000;
        return true;
    }
};

}  // namespace

TEST_CASE("audio observation: a real window becomes a bounded measurement", "[audio][contract]") {
    const auto pcm = tone(960, 1, 8000.0, 440.0, 48000);
    const auto o = observe_audio_window(pcm.data(), 960, 1, 48000, Provenance::synthetic, "test:tone");

    REQUIRE(o.valid);
    REQUIRE(o.features.rms > 0.1);
    REQUIRE(o.features.peak > 0.1);
    REQUIRE(o.confidence >= 0.0);
    REQUIRE(o.confidence <= 1.0);
    REQUIRE(o.provenance == Provenance::synthetic);          // a fixture is never reported as real
    REQUIRE(o.event.find("sound level") == 0);                // what was measured, not what it was
    REQUIRE(o.uncertainty.find("not a species") != std::string::npos);
    REQUIRE(o.uncertainty.find("not an event classification") != std::string::npos);
}

TEST_CASE("audio observation: silence and bad input are refused, not guessed",
          "[audio][contract][refusal]") {
    // Silence is a measurement (very low level), and the observation says so rather than failing.
    const auto silence = tone(960, 1, 0.0, 440.0, 48000);
    const auto quiet = observe_audio_window(silence.data(), 960, 1, 48000, Provenance::synthetic, "t");
    REQUIRE(quiet.valid);
    REQUIRE(quiet.features.rms < 0.01);

    // Invalid input is refused with a reason.
    const auto refused = observe_audio_window(nullptr, 960, 1, 48000, Provenance::synthetic, "t");
    REQUIRE_FALSE(refused.valid);
    REQUIRE_FALSE(refused.reason.empty());
    REQUIRE(refused.confidence == 0.0);

    // An out-of-range sample rate is refused by the existing extractor contract (its bounds are
    // 8 kHz to 48 kHz, so 12345 Hz would be a valid rate - the first version of this test assumed
    // otherwise and was wrong).
    const auto bad_rate = observe_audio_window(silence.data(), 960, 1, 4000, Provenance::synthetic, "t");
    REQUIRE_FALSE(bad_rate.valid);
    REQUIRE_FALSE(bad_rate.reason.empty());
}

TEST_CASE("audio observation: the vocabulary carries no identification of anything",
          "[audio][contract][refusal]") {
    const auto pcm = tone(960, 2, 6000.0, 220.0, 44100);
    const auto o = observe_audio_window(pcm.data(), 960, 2, 44100, Provenance::synthetic, "test");
    REQUIRE(o.valid);
    // The words are banned as CLAIMS, not as denials: the honesty of the text depends on saying "not
    // speech, not a species". So each occurrence must be negated nearby, and the event description -
    // which asserts what was measured - must not contain them at all.
    // "not a species" and "not speech" are both denials; the bare "not " belongs in this list (an
    // earlier version of this check omitted it and failed on its own honest wording).
    const std::vector<std::string> negations = {"not ", "no ", "never", "nothing", "without"};
    for (const char* forbidden : {"speech", "word", "keyword", "species", "person", "voice",
                                  "recognis", "recogniz", "animal", "alarm"}) {
        INFO("forbidden in the event description: " << forbidden);
        REQUIRE(o.event.find(forbidden) == std::string::npos);
        size_t at = o.uncertainty.find(forbidden);
        while (at != std::string::npos) {
            const size_t from = at > 24 ? at - 24 : 0;
            const std::string before = o.uncertainty.substr(from, at - from);
            const bool negated = std::any_of(negations.begin(), negations.end(),
                                             [&](const std::string& n) {
                                                 return before.find(n) != std::string::npos;
                                             });
            INFO("uncertainty says: " << o.uncertainty << " | context before '" << before << "'");
            REQUIRE(negated);
            at = o.uncertainty.find(forbidden, at + 1);
        }
    }
}

TEST_CASE("audio observation: the JSON is the platform-neutral shape the engine consumes",
          "[audio][contract]") {
    const auto pcm = tone(960, 1, 8000.0, 440.0, 48000);
    const auto o = observe_audio_window(pcm.data(), 960, 1, 48000, Provenance::real, "alsa:default");
    const auto j = audio_observation_to_json(o);

    // The keys any consumer needs, whatever the source platform was.
    for (const char* key : {"type", "schema_version", "source", "provenance", "status", "confidence",
                            "detector_id", "model_id", "event", "uncertainty", "features"}) {
        INFO("key: " << key);
        REQUIRE(j.contains(key));
    }
    REQUIRE(j["provenance"] == "real");            // carried, not inferred
    REQUIRE(j["status"] == "detected");
    REQUIRE(j["features"].contains("rms"));
    REQUIRE(j["features"].contains("zero_crossing_rate"));
}

TEST_CASE("audio observation: any platform can supply samples through the interface",
          "[audio][contract]") {
    FakeSource source;
    std::string error;
    REQUIRE(source.platform() == "test");
    REQUIRE(source.open(&error));

    std::vector<int16_t> pcm;
    size_t frames = 0;
    uint32_t channels = 0, rate = 0;
    REQUIRE(source.read_window(pcm, &frames, &channels, &rate));
    const auto o = observe_audio_window(pcm.data(), frames, channels, rate, Provenance::synthetic,
                                        source.platform());
    REQUIRE(o.valid);
    // The engine consumes it through its own types: the JSON's keys map onto an Observation, which
    // becomes an Event and comes back - the same path a camera observation takes. A platform-neutral
    // contract is only worth anything if the consumer can actually read it.
    const auto j = audio_observation_to_json(o);
    Observation observation;
    observation.observation_id = "audio-contract-1";
    observation.source = j["source"];
    observation.detector_id = j["detector_id"];
    observation.model_id = j["model_id"];
    observation.status = DetectorStatus::detected;
    observation.confidence = j["confidence"];
    observation.provenance = Provenance::real;          // stated by the caller
    observation.features = j["features"];

    const auto round_trip = Observation::from_event(observation.to_event());
    REQUIRE(round_trip.has_value());
    REQUIRE(round_trip->detector_id == "audio-activity");
    REQUIRE(round_trip->status == DetectorStatus::detected);
    REQUIRE(round_trip->features.contains("rms"));
    REQUIRE(round_trip->features.contains("zero_crossing_rate"));

    // And the fusion engine accepts it as evidence for its own verdict, exactly like motion evidence.
    const FusionEngine engine;
    std::vector<Observation> window{*round_trip, *round_trip};
    window[1].observation_id = "audio-contract-2";
    const auto fused = engine.fuse(window);
    REQUIRE(fused.contributing == 2);
    REQUIRE(fused.provenance == EvidenceProvenance::real);
    source.close();
}
