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
