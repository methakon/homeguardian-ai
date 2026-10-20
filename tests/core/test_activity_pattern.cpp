// test_activity_pattern: the temporal shape of the camera's bounded scalars, and the edges of its claim.

#include <catch2/catch.hpp>

#include "core/ActivityPattern.h"

#include <cctype>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

ActivityPatternConfig enabled() {
    ActivityPatternConfig cfg;
    cfg.enabled = true;
    return cfg;
}

// A small compact travelling change: ~1% of the frame, well inside the compact radius.
ActivitySample small_moving(double fraction = 0.010) { return {fraction, 0.08, 0.05}; }

}  // namespace

TEST_CASE("pattern: a disabled analysis says so rather than staying silent", "[pattern][opt-in]") {
    const auto finding = characterise_activity_pattern({small_moving(), small_moving(), small_moving()});
    REQUIRE_FALSE(finding.valid);
    REQUIRE(finding.reason.find("opt-in") != std::string::npos);
    REQUIRE(finding.uncertainty.empty() == false);
}

TEST_CASE("pattern: nothing above the floor is quiet", "[pattern]") {
    std::vector<ActivitySample> quiet(20, ActivitySample{0.004, 0.30, 0.0});
    const auto finding = characterise_activity_pattern(quiet, enabled());
    INFO(finding.description);
    REQUIRE(finding.valid);
    REQUIRE(finding.pattern == ActivityPattern::quiet);
    REQUIRE(finding.episodes == 0);
    REQUIRE(finding.confidence > 0.5);
}

TEST_CASE("pattern: one change is isolated, not a repetition", "[pattern]") {
    std::vector<ActivitySample> samples(10, ActivitySample{0.004, 0.30, 0.0});
    samples[4] = small_moving();
    const auto finding = characterise_activity_pattern(samples, enabled());
    INFO(finding.description);
    REQUIRE(finding.pattern == ActivityPattern::isolated_change);
    REQUIRE(finding.episodes == 1);
    REQUIRE(finding.repeats == 0);
}

TEST_CASE("pattern: repeated small travelling changes are recognised as such", "[pattern]") {
    std::vector<ActivitySample> samples;
    for (int i = 0; i < 6; ++i) {
        samples.push_back(ActivitySample{0.004, 0.30, 0.0});      // a quiet gap
        samples.push_back(small_moving(0.010 + 0.001 * i));       // and a small moving change
    }
    const auto finding = characterise_activity_pattern(samples, enabled());
    INFO(finding.description << " | " << finding.uncertainty);
    REQUIRE(finding.valid);
    REQUIRE(finding.pattern == ActivityPattern::repeated_localized);
    REQUIRE(finding.repeats >= 3);
    REQUIRE(finding.confidence > 0.5);
    // and it does NOT claim what it cannot know
    REQUIRE(finding.uncertainty.find("not identify a species") != std::string::npos);
    REQUIRE(finding.uncertainty.find("equally consistent") != std::string::npos);
}

TEST_CASE("pattern: a repetition requires all four conditions", "[pattern][refusal]") {
    auto run = [](std::vector<ActivitySample> samples) {
        for (int i = 0; i < 5; ++i) samples.push_back(samples.empty() ? samples[0] : samples[0]);
        return characterise_activity_pattern(samples, enabled());
    };
    SECTION("a large change is not a small body") {
        std::vector<ActivitySample> samples(3, {0.20, 0.08, 0.05});     // compact and travelling, but big
        samples.push_back({0.004, 0.3, 0.0});
        samples.push_back({0.20, 0.08, 0.05});
        samples.push_back({0.004, 0.3, 0.0});
        samples.push_back({0.20, 0.08, 0.05});
        const auto finding = characterise_activity_pattern(samples, enabled());
        REQUIRE(finding.pattern != ActivityPattern::repeated_localized);
    }
    SECTION("a stationary flicker is not a repetition") {
        std::vector<ActivitySample> samples;
        for (int i = 0; i < 5; ++i) {
            samples.push_back({0.004, 0.3, 0.0});
            samples.push_back({0.010, 0.08, 0.0});      // compact and small, but it does not travel
        }
        const auto finding = characterise_activity_pattern(samples, enabled());
        INFO(finding.description);
        REQUIRE(finding.repeats == 0);
        REQUIRE(finding.pattern != ActivityPattern::repeated_localized);
    }
    SECTION("a spread change across the frame is not a small body") {
        std::vector<ActivitySample> samples;
        for (int i = 0; i < 5; ++i) {
            samples.push_back({0.004, 0.3, 0.0});
            samples.push_back({0.010, 0.35, 0.05});     // travelling and small, but not compact
        }
        const auto finding = characterise_activity_pattern(samples, enabled());
        REQUIRE(finding.repeats == 0);
    }
    SECTION("wildly different amplitudes are erratic, not repeated") {
        std::vector<ActivitySample> samples;
        for (int i = 0; i < 6; ++i) {
            samples.push_back({0.004, 0.3, 0.0});
            samples.push_back({0.006 + 0.014 * i, 0.08, 0.05});
        }
        const auto finding = characterise_activity_pattern(samples, enabled());
        INFO(finding.description);
        REQUIRE(finding.pattern == ActivityPattern::erratic);
    }
}

TEST_CASE("pattern: a large sustained change is stated as sustained", "[pattern]") {
    std::vector<ActivitySample> samples;
    for (int i = 0; i < 12; ++i) samples.push_back({0.45, 0.30, 0.02});
    const auto finding = characterise_activity_pattern(samples, enabled());
    REQUIRE(finding.pattern == ActivityPattern::sustained_activity);
}

TEST_CASE("pattern: the analysis is bounded and deterministic", "[pattern][bounds]") {
    std::vector<ActivitySample> many;
    for (int i = 0; i < 500; ++i) many.push_back(i % 3 == 0 ? small_moving() : ActivitySample{0.004, 0.3, 0.0});
    const auto first = characterise_activity_pattern(many, enabled());
    REQUIRE(first.samples_used <= ActivityPatternConfig{}.max_samples);
    REQUIRE(first.valid);

    const auto second = characterise_activity_pattern(many, enabled());
    REQUIRE(second.pattern == first.pattern);
    REQUIRE(second.repeats == first.repeats);
    REQUIRE(second.confidence == Approx(first.confidence));
}

TEST_CASE("pattern: the vocabulary is closed", "[pattern][refusal]") {
    const ActivityPattern all[] = {ActivityPattern::quiet, ActivityPattern::isolated_change,
                                  ActivityPattern::repeated_localized,
                                  ActivityPattern::sustained_activity, ActivityPattern::erratic};
    // Whole words, not substrings: "erratic" contains "rat", which is exactly the kind of false positive
    // a naive scan produces. The claim being tested is that no pattern NAME is a species claim.
    const auto contains_word = [](const std::string& text, const std::string& word) {
        size_t at = text.find(word);
        while (at != std::string::npos) {
            const bool left_ok = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
            const size_t end = at + word.size();
            const bool right_ok = end >= text.size() || !std::isalpha(static_cast<unsigned char>(text[end]));
            if (left_ok && right_ok) return true;
            at = text.find(word, at + 1);
        }
        return false;
    };
    for (const auto p : all) {
        const std::string name = activity_pattern_to_string(p);
        for (const char* forbidden : {"species", "rat", "mouse", "insect", "bird", "cockroach", "lizard",
                                      "animal"}) {
            INFO(name << " must not say " << forbidden);
            REQUIRE_FALSE(contains_word(name, forbidden));
        }
    }
    REQUIRE(characterise_activity_pattern({}, enabled()).reason.find("nothing to characterise") !=
            std::string::npos);
}

// ---------------------------------------------------------------------------------------------------
// The audio half: the same discipline on the microphone path's bounded features. A level and repetition
// description, never a classification of what made the sound.

namespace {

AudioPatternConfig audio_enabled() {
    AudioPatternConfig cfg;
    cfg.enabled = true;
    return cfg;
}

std::vector<AudioActivitySample> audio_stream(double floor, size_t n) {
    return std::vector<AudioActivitySample>(n, AudioActivitySample{floor, 0.01});
}

}  // namespace

TEST_CASE("audio pattern: opt-in, and it says so when disabled", "[pattern][audio]") {
    const auto finding = characterise_audio_activity(audio_stream(0.3, 8));
    REQUIRE_FALSE(finding.valid);
    REQUIRE(finding.reason.find("opt-in") != std::string::npos);
    REQUIRE(characterise_audio_activity({}, audio_enabled()).reason.find("nothing to characterise") !=
            std::string::npos);
}

TEST_CASE("audio pattern: quiet, isolated, repeated, sustained and varying", "[pattern][audio]") {
    SECTION("a floor below the audible threshold is quiet") {
        const auto finding = characterise_audio_activity(audio_stream(0.005, 20), audio_enabled());
        INFO(finding.description);
        REQUIRE(finding.pattern == AudioActivityPattern::quiet);
        REQUIRE(finding.events == 0);
    }
    SECTION("one loud window is isolated, not a repetition") {
        auto samples = audio_stream(0.005, 20);
        samples[7] = {0.30, 0.02};
        const auto finding = characterise_audio_activity(samples, audio_enabled());
        REQUIRE(finding.pattern == AudioActivityPattern::isolated_event);
        REQUIRE(finding.events == 1);
        REQUIRE(finding.repeats == 0);
    }
    SECTION("repeated similar sounds are recognised as repeated") {
        auto samples = audio_stream(0.005, 20);
        for (size_t i = 2; i < 20; i += 4) samples[i] = {0.28 + 0.01 * double(i % 3), 0.02};
        const auto finding = characterise_audio_activity(samples, audio_enabled());
        INFO(finding.description);
        REQUIRE(finding.pattern == AudioActivityPattern::repeated_events);
        REQUIRE(finding.repeats >= 3);
    }
    SECTION("a level that stays up is sustained, not a series of events") {
        const auto finding = characterise_audio_activity(audio_stream(0.20, 20), audio_enabled());
        REQUIRE(finding.pattern == AudioActivityPattern::sustained_level);
        REQUIRE(finding.occupancy >= 0.6);
    }
    SECTION("wildly different levels are varying, not repeated") {
        // Two clusters fifteen times apart. The first version of the rule called this a repetition because
        // half the events sat near the median; the dispersion test is what now rejects it.
        std::vector<AudioActivitySample> samples;
        for (size_t i = 0; i < 8; ++i) {
            samples.push_back({0.005, 0.01});
            samples.push_back({(i % 2 == 0) ? 0.06 : 0.90, 0.02});
        }
        const auto finding = characterise_audio_activity(samples, audio_enabled());
        INFO(finding.description);
        REQUIRE(finding.pattern == AudioActivityPattern::varying);
        REQUIRE(finding.description.find("dispersion") != std::string::npos);
    }
    SECTION("a tight run of similar bursts stays a repetition, so the gate is not a blanket refusal") {
        std::vector<AudioActivitySample> samples;
        for (size_t i = 0; i < 8; ++i) {
            samples.push_back({0.005, 0.01});
            samples.push_back({0.28 + 0.01 * double(i % 3), 0.02});
        }
        const auto finding = characterise_audio_activity(samples, audio_enabled());
        INFO(finding.description);
        REQUIRE(finding.pattern == AudioActivityPattern::repeated_events);
    }
}

TEST_CASE("audio pattern: the vocabulary names no sound, and the analysis is bounded and deterministic",
          "[pattern][audio][refusal]") {
    const auto contains_word = [](const std::string& text, const std::string& word) {
        size_t at = text.find(word);
        while (at != std::string::npos) {
            const bool left_ok = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
            const size_t end = at + word.size();
            const bool right_ok =
                end >= text.size() || !std::isalpha(static_cast<unsigned char>(text[end]));
            if (left_ok && right_ok) return true;
            at = text.find(word, at + 1);
        }
        return false;
    };
    for (const auto p : {AudioActivityPattern::quiet, AudioActivityPattern::isolated_event,
                         AudioActivityPattern::repeated_events, AudioActivityPattern::sustained_level,
                         AudioActivityPattern::varying}) {
        const std::string name = audio_activity_pattern_to_string(p);
        for (const char* forbidden : {"speech", "word", "voice", "species", "animal", "bark", "alarm"}) {
            INFO(name << " must not say " << forbidden);
            REQUIRE_FALSE(contains_word(name, forbidden));
        }
    }
    // And a finding never carries a sound class in its text.
    auto samples = audio_stream(0.005, 20);
    samples[3] = {0.4, 0.02};
    const auto finding = characterise_audio_activity(samples, audio_enabled());
    REQUIRE(finding.uncertainty.find("not a sound classification") != std::string::npos);
    REQUIRE(finding.description.find("speech") == std::string::npos);

    // Bounded: a long stream is truncated to the configured window, and the result is reproducible.
    std::vector<AudioActivitySample> many;
    for (size_t i = 0; i < 500; ++i) many.push_back(i % 4 == 0 ? AudioActivitySample{0.3, 0.02}
                                                              : AudioActivitySample{0.005, 0.01});
    const auto first = characterise_audio_activity(many, audio_enabled());
    const auto second = characterise_audio_activity(many, audio_enabled());
    REQUIRE(first.samples_used <= AudioPatternConfig{}.max_samples);
    REQUIRE(first.pattern == second.pattern);
    REQUIRE(first.repeats == second.repeats);
}
