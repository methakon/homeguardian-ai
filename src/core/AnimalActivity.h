#pragma once

// AnimalActivity: a LAYERED representation of animal-related activity for a household sensor, designed
// so that the thing everyone wants (a species) is the one thing the type system will not produce.
//
// WHY THE SEPARATION IS IN THE TYPES, NOT IN THE DOCS
// ---------------------------------------------------
// A pixel-change proxy reports that something moved. A microphone reports that something made a sound.
// Neither is an animal, and neither is a species. Systems that blur that line end up shipping
// "animal detected" from motion alone, and the first field report ("it said animal; it was a curtain")
// destroys the credibility of every other claim. So the layers are separate types here:
//
//   1. ActivityEvidence      - one bounded measurement: motion, audio, environmental, schedule.
//   2. motion_only           - "something moved in view". No animal claim of any kind.
//   3. candidate_animal_activity - a HYPOTHESIS that a multi-source pattern is consistent with a small
//                              body moving. It says so in its own sentence and cannot be promoted.
//   4. ConfirmedSpeciesIdentity - NOT CONSTRUCTIBLE in this build (no classifier, no annotated
//                              dataset, no model runtime). The class deletes its constructors and
//                              reports feature_available() == false, so no code path can emit one.
//
// PROVENANCE is derived from the contributing evidence by the same rule the fusion engine uses, so a
// replay or a fixture can never be reported as a physical measurement.
//
// WHAT IT REFUSES TO SAY: no species, no individual, no identification, no behaviour interpretation, no
// pest/health claim, and nothing that implies a calamity. The vocabulary is closed and a test scans
// every sentence this component can emit for species and identification words.

#include "core/FusionEngine.h"   // EvidenceProvenance + derive_evidence_provenance
#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace homeguardian {

// Layer 1: one bounded measurement from one channel. Deliberately NOT an animal claim.
enum class ActivityEvidenceKind {
    motion,        // changed-pixel fraction from the camera path
    audio,         // bounded audio descriptors (reserved: no microphone path is enabled today)
    environmental, // pressure/temperature/humidity correlation, when such a sensor exists
    schedule       // time-of-day context, never evidence on its own
};

inline const char* activity_evidence_kind_to_string(ActivityEvidenceKind k) {
    switch (k) {
        case ActivityEvidenceKind::motion:        return "motion";
        case ActivityEvidenceKind::audio:         return "audio";
        case ActivityEvidenceKind::environmental: return "environmental";
        case ActivityEvidenceKind::schedule:      return "schedule";
    }
    return "unknown";
}

struct ActivityEvidence {
    ActivityEvidenceKind kind = ActivityEvidenceKind::motion;
    std::string detector_id;
    std::string model_id;
    double confidence = 0.0;                 // [0,1]; the detector's own strength, not a probability of an animal
    Provenance provenance = Provenance::synthetic;
    std::string direction;                   // "rising" / "falling" when the detector models one
    bool sustained_change_confirmed = false; // the detector confirmed a sustained change, not a spike
    std::chrono::system_clock::time_point observed_at = std::chrono::system_clock::now();
    nlohmann::json features = nlohmann::json::object();
};

// Layer 2 & 3: what the system may say. There is no "identified" member and no species member.
enum class AnimalActivityClass {
    no_evidence,                // quiet
    motion_only,                // something moved; that is the whole claim
    candidate_animal_activity,  // HYPOTHESIS: a multi-source pattern consistent with small-body motion
    insufficient_evidence       // not enough, or the layer is disabled
};

inline const char* animal_activity_class_to_string(AnimalActivityClass c) {
    switch (c) {
        case AnimalActivityClass::no_evidence:               return "no_evidence";
        case AnimalActivityClass::motion_only:               return "motion_only";
        case AnimalActivityClass::candidate_animal_activity: return "candidate_animal_activity";
        case AnimalActivityClass::insufficient_evidence:     return "insufficient_evidence";
    }
    return "insufficient_evidence";
}

struct AnimalActivityFinding {
    AnimalActivityClass classification = AnimalActivityClass::insufficient_evidence;
    double confidence = 0.0;                       // the contributing evidence's own confidence
    EvidenceProvenance provenance = EvidenceProvenance::uncertain;
    size_t motion_sources = 0;
    size_t audio_sources = 0;
    std::string sentence;      // closed vocabulary: what is claimed
    std::string uncertainty;   // what this explicitly is NOT
    std::string reason;        // bounded, non-sensitive explanation
};

// Layer 4: a confirmed species identity. It cannot exist in this build, by construction.
//
// Deliberately: every constructor is deleted and the only factory reports that the feature is absent.
// A future classifier would have to REPLACE this type (and bring an approved, licensed annotated
// dataset and a measured false-positive rate), rather than quietly construct one from motion.
class ConfirmedSpeciesIdentity {
public:
    ConfirmedSpeciesIdentity() = delete;
    ConfirmedSpeciesIdentity(const ConfirmedSpeciesIdentity&) = delete;
    ConfirmedSpeciesIdentity& operator=(const ConfirmedSpeciesIdentity&) = delete;

    // No classifier, no annotated dataset, no model runtime ships with this project.
    static bool feature_available() { return false; }
    static const char* unavailable_reason() {
        return "no classifier, no approved annotated dataset and no model runtime in this build";
    }
};

struct AnimalActivityPolicy {
    // The layer is OPT-IN and off by default: nothing in the release's decision path changes because
    // this file exists.
    bool enabled = false;
    // A candidate hypothesis needs corroboration: at least this many independent sustained-change
    // sources. One motion source alone is `motion_only`, never a candidate.
    size_t min_sustained_sources = 2;
    // The same confidence gate used everywhere else in this project. Not tuned here.
    double min_confidence = 0.5;
    // Reserved for the future audio channel: a candidate would then require corroboration, not just
    // more of the same kind of evidence.
    bool require_audio_corroboration = false;

    static AnimalActivityPolicy defaults() { return AnimalActivityPolicy{}; }
};

class AnimalActivityAssessor {
public:
    AnimalActivityAssessor() = default;
    explicit AnimalActivityAssessor(AnimalActivityPolicy policy) : policy_(policy) {}

    AnimalActivityFinding assess(const std::vector<ActivityEvidence>& evidence) const;

    bool enabled() const { return policy_.enabled; }
    const AnimalActivityPolicy& policy() const { return policy_; }

private:
    AnimalActivityPolicy policy_{};
};

}  // namespace homeguardian
