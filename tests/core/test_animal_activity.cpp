// test_animal_activity: the layered animal-activity representation, and its refusals.
//
// The behaviour that matters here is what the component will NOT say: no species, no identification,
// no behaviour reading, and no candidate hypothesis from a single source or from generic motion alone.

#include <catch2/catch.hpp>

#include "core/AnimalActivity.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

ActivityEvidence evidence(ActivityEvidenceKind kind, double confidence, bool sustained,
                          Provenance provenance = Provenance::synthetic, const char* direction = "rising") {
    ActivityEvidence e;
    e.kind = kind;
    e.detector_id = kind == ActivityEvidenceKind::motion ? "camera-motion-activity" : "audio-activity";
    e.model_id = "ema-baseline-v1";
    e.confidence = confidence;
    e.provenance = provenance;
    e.direction = direction;
    e.sustained_change_confirmed = sustained;
    return e;
}

AnimalActivityPolicy enabled_policy() {
    AnimalActivityPolicy p;
    p.enabled = true;
    return p;
}

// Every sentence this component can emit, scanned for a claim it must never make.
//
// Two classes of word, because the honest denial of a claim must itself use the word: a sentence may
// say "not a species identification" and that is the wording we WANT. So species names and
// identification verbs are banned outright, while "species"/"animal" are allowed only in a negated
// form (or as the layer's own name, "animal-activity layer").
const std::vector<std::string> kNeverAllowed = {
    "ant", "ants", "rodent", "mouse", "mice", "rat", "bird", "cockroach", "lizard", "gecko",
    "dog", "cat", "spider", "snake", "identified", "recognised", "recognized", "recognition",
    "earthquake", "flood", "calamity", "disaster",
};
const std::vector<std::string> kOnlyIfNegated = {"species", "animal"};
const std::vector<std::string> kNegations = {"not a", "not an", "not the", "no ", "never", "cannot",
                                             "without", "nothing"};

// Whole-word search: "identification" contains "cat" as a substring, and a naive find() would flag
// every honest denial. Boundaries are checked on both sides.
bool contains_word(const std::string& haystack, const std::string& word) {
    size_t at = haystack.find(word);
    while (at != std::string::npos) {
        const bool left_ok = at == 0 || !std::isalnum(static_cast<unsigned char>(haystack[at - 1]));
        const size_t end = at + word.size();
        const bool right_ok = end >= haystack.size() ||
                              !std::isalnum(static_cast<unsigned char>(haystack[end]));
        if (left_ok && right_ok) return true;
        at = haystack.find(word, at + 1);
    }
    return false;
}

std::string lower_of(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return lower;
}

void assert_no_forbidden_claim(const AnimalActivityFinding& f) {
    for (const std::string& text : {f.sentence, f.uncertainty, f.reason}) {
        const std::string lower = lower_of(text);
        for (const auto& bad : kNeverAllowed) {
            INFO("emitted text: " << text << " | never allowed: " << bad);
            REQUIRE_FALSE(contains_word(lower, bad));
        }
        for (const auto& word : kOnlyIfNegated) {
            size_t at = lower.find(word);
            while (at != std::string::npos) {
                if (!contains_word(lower, word)) break;
                // The layer's own name is not a claim about an animal.
                const bool is_layer_name = word == "animal" && at + word.size() < lower.size() &&
                                           lower[at + word.size()] == '-';
                if (!is_layer_name) {
                    const size_t from = at > 40 ? at - 40 : 0;
                    const std::string before = lower.substr(from, at - from);
                    const bool negated = std::any_of(kNegations.begin(), kNegations.end(),
                                                     [&](const std::string& n) {
                                                         return before.find(n) != std::string::npos;
                                                     });
                    INFO("emitted text: " << text << " | '" << word << "' at " << at
                                          << " must be negated (context: '" << before << "')");
                    REQUIRE(negated);
                }
                at = lower.find(word, at + 1);
            }
        }
    }
}

}  // namespace

TEST_CASE("animal activity: species identity cannot exist in this build", "[animal][refusal]") {
    REQUIRE(ConfirmedSpeciesIdentity::feature_available() == false);
    REQUIRE(std::string(ConfirmedSpeciesIdentity::unavailable_reason()).find("no classifier")
            != std::string::npos);
    // The type deletes its constructors, so this is a compile-time fact rather than a runtime hope:
    //   ConfirmedSpeciesIdentity id;                  // does not compile
    //   ConfirmedSpeciesIdentity id{};                // does not compile
    REQUIRE(true);
}

TEST_CASE("animal activity: a disabled layer says so instead of staying silent", "[animal]") {
    AnimalActivityAssessor assessor;   // default policy: enabled = false
    REQUIRE_FALSE(assessor.enabled());
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.9, true),
                                          evidence(ActivityEvidenceKind::motion, 0.8, true)});
    REQUIRE(finding.classification == AnimalActivityClass::insufficient_evidence);
    REQUIRE(finding.sentence.find("disabled") != std::string::npos);
    REQUIRE(finding.confidence == 0.0);
    REQUIRE(finding.provenance == EvidenceProvenance::uncertain);
    assert_no_forbidden_claim(finding);
}

TEST_CASE("animal activity: one sustained motion source is motion, never an animal",
          "[animal][refusal]") {
    AnimalActivityAssessor assessor(enabled_policy());
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.9, true)});
    REQUIRE(finding.classification == AnimalActivityClass::motion_only);
    REQUIRE(finding.motion_sources == 1);
    REQUIRE(finding.sentence == "a sustained change in motion in view");
    REQUIRE(finding.uncertainty.find("not an animal identification") != std::string::npos);
    REQUIRE(finding.provenance == EvidenceProvenance::synthetic);   // a fixture is not a measurement
    assert_no_forbidden_claim(finding);
}

TEST_CASE("animal activity: a spike is not evidence, however strong", "[animal][refusal]") {
    AnimalActivityAssessor assessor(enabled_policy());
    // Two sources, both strong, both spikes: no sustained confirmation anywhere.
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.99, false),
                                          evidence(ActivityEvidenceKind::motion, 0.99, false)});
    REQUIRE(finding.classification == AnimalActivityClass::no_evidence);
    REQUIRE(finding.confidence == 0.0);
    assert_no_forbidden_claim(finding);

    // And confidence below the gate is not evidence either.
    const auto weak = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.2, true),
                                       evidence(ActivityEvidenceKind::motion, 0.3, true)});
    REQUIRE(weak.classification == AnimalActivityClass::no_evidence);
    assert_no_forbidden_claim(weak);
}

TEST_CASE("animal activity: two corroborating sources produce a labelled HYPOTHESIS", "[animal]") {
    AnimalActivityAssessor assessor(enabled_policy());
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.8, true),
                                          evidence(ActivityEvidenceKind::motion, 0.6, true)});
    REQUIRE(finding.classification == AnimalActivityClass::candidate_animal_activity);
    REQUIRE(finding.motion_sources == 2);
    REQUIRE(finding.confidence == Approx(0.7));
    REQUIRE(finding.uncertainty.find("HYPOTHESIS ONLY") == 0);
    REQUIRE(finding.uncertainty.find("not a species identification") != std::string::npos);
    REQUIRE(finding.uncertainty.find("curtain") != std::string::npos);   // names the confounders
    REQUIRE(finding.reason.find("no audio channel is available") != std::string::npos);
    assert_no_forbidden_claim(finding);
}

TEST_CASE("animal activity: mixed evidence is stated as mixed", "[animal][provenance]") {
    AnimalActivityAssessor assessor(enabled_policy());
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.8, true,
                                                   Provenance::real),
                                          evidence(ActivityEvidenceKind::motion, 0.8, true,
                                                   Provenance::synthetic)});
    REQUIRE(finding.provenance == EvidenceProvenance::mixed);

    const auto all_real = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.8, true,
                                                    Provenance::real),
                                           evidence(ActivityEvidenceKind::motion, 0.7, true,
                                                    Provenance::real)});
    REQUIRE(all_real.provenance == EvidenceProvenance::real);
}

TEST_CASE("animal activity: an audio-only change is not an animal either", "[animal][refusal]") {
    AnimalActivityAssessor assessor(enabled_policy());
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::audio, 0.9, true),
                                          evidence(ActivityEvidenceKind::audio, 0.8, true)});
    REQUIRE(finding.classification == AnimalActivityClass::insufficient_evidence);
    REQUIRE(finding.motion_sources == 0);
    REQUIRE(finding.audio_sources == 2);
    REQUIRE(finding.sentence.find("non-motion") != std::string::npos);
    assert_no_forbidden_claim(finding);
}

TEST_CASE("animal activity: requiring audio corroboration with no audio channel yields motion_only",
          "[animal]") {
    AnimalActivityPolicy p = enabled_policy();
    p.require_audio_corroboration = true;
    AnimalActivityAssessor assessor(p);
    const auto finding = assessor.assess({evidence(ActivityEvidenceKind::motion, 0.9, true),
                                          evidence(ActivityEvidenceKind::motion, 0.85, true)});
    // The policy refuses to promote a hypothesis it cannot corroborate, and says which source is missing.
    REQUIRE(finding.classification == AnimalActivityClass::motion_only);
    assert_no_forbidden_claim(finding);
}

TEST_CASE("animal activity: the classification vocabulary is closed", "[animal][refusal]") {
    // Every class the enum can take, as strings, must be in the closed set.
    const std::vector<std::string> closed = {"no_evidence", "motion_only", "candidate_animal_activity",
                                             "insufficient_evidence"};
    for (AnimalActivityClass c : {AnimalActivityClass::no_evidence, AnimalActivityClass::motion_only,
                                  AnimalActivityClass::candidate_animal_activity,
                                  AnimalActivityClass::insufficient_evidence}) {
        const std::string s = animal_activity_class_to_string(c);
        REQUIRE(std::find(closed.begin(), closed.end(), s) != closed.end());
    }
    REQUIRE(std::string(animal_activity_class_to_string(
                static_cast<AnimalActivityClass>(99))) == "insufficient_evidence");
}

TEST_CASE("animal activity: assessment is deterministic", "[animal]") {
    AnimalActivityAssessor assessor(enabled_policy());
    const std::vector<ActivityEvidence> ev = {evidence(ActivityEvidenceKind::motion, 0.8, true),
                                              evidence(ActivityEvidenceKind::motion, 0.6, true)};
    const auto a = assessor.assess(ev);
    const auto b = assessor.assess(ev);
    REQUIRE(a.classification == b.classification);
    REQUIRE(a.confidence == b.confidence);
    REQUIRE(a.reason == b.reason);
}
