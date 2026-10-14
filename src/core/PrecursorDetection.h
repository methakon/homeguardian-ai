#pragma once

// The precursor suite: bounded feature -> detector -> DST fusion -> Observation.
//
// This is the piece of `animal-calamity-precursor-detection` that can be built
// honestly today. It produces ONLY the result vocabulary the milestone allows:
//
//   activity_change_detected      sustained activity level change vs baseline
//   audio_activity_deviation      bounded audio energy outside its learned band
//   possible_animal_activity      LOW-CONFIDENCE HYPOTHESIS, explicitly labelled
//   environmental_correlation     an authorized environmental sample moved in
//                                 the same window as a detected change
//   insufficient_evidence         including "the sensors contradict each other"
//
// It CANNOT produce `earthquake_imminent`, `flood_will_occur` or
// `dog_predicts_disaster`: the DST frame of discernment (DempsterShafer.h) has no
// element for them, so such a claim is unrepresentable rather than merely
// discouraged.
//
// WHAT IT DOES NOT DO
//   * No species identification. `possible_animal_activity` is a hypothesis about
//     a MULTI-SENSOR ACTIVITY PATTERN, never "this is a gecko/rodent/dog".
//   * No speech/voice/emotion analysis, and no face or fall detection.
//   * No raw media retention: it consumes already-extracted scalars (or bounded
//     int16 PCM / luma planes passed straight to the extractors).
//
// CATEGORY DISCIPLINE (evidence grading from the skill's own correction ledger):
//   A (established)  -> bounded audio energy, motion change, occupancy/activity
//                       level: measured quantities, no interpretation.
//   B (plausible)    -> environmental correlation via EnvironmentalCorrelation;
//                       reported as a correlation at `info`, never an alert.
//   C (folklore)     -> NOT IMPLEMENTED as a detector. Seismic/calamity
//                       precursors have no code path here.

#include "core/ActivityDetectors.h"
#include "core/AnomalyMonitor.h"
#include "core/BoundedFeatures.h"
#include "core/DempsterShafer.h"
#include "core/Observation.h"

#include <chrono>
#include <string>
#include <vector>

namespace homeguardian {

enum class PrecursorLabel {
    insufficient_evidence,
    activity_change_detected,
    audio_activity_deviation,
    possible_animal_activity,
    environmental_correlation
};

inline const char* precursor_label_to_string(PrecursorLabel l) {
    switch (l) {
        case PrecursorLabel::insufficient_evidence: return "insufficient_evidence";
        case PrecursorLabel::activity_change_detected: return "activity_change_detected";
        case PrecursorLabel::audio_activity_deviation: return "audio_activity_deviation";
        case PrecursorLabel::possible_animal_activity: return "possible_animal_activity";
        case PrecursorLabel::environmental_correlation: return "environmental_correlation";
    }
    return "insufficient_evidence";
}

struct PrecursorFusion {
    PrecursorLabel label = PrecursorLabel::insufficient_evidence;
    std::string hypothesis;         // top hypothesis in the DST frame
    double belief = 0.0;            // belief in the top hypothesis
    double ignorance = 0.0;         // uncommitted mass (Theta)
    double conflict_k = 0.0;        // disagreement between sources
    bool suppressed = false;        // K above threshold: contradictory, not evidence
    size_t confirming_sources = 0;  // sources supporting activity_change
    std::string explanation;        // bounded, human-readable
};

class PrecursorSuite {
public:
    struct Config {
        ActivityChangeDetector::Config motion_activity;
        ActivityChangeDetector::Config audio_activity;
        VectorDriftDetector::Config vector;
        AnomalyMonitor::Config audio_deviation;
        // Acceptance targets, NOT measured performance.
        // An authorized environmental measurement that moved by at least this
        // strength takes precedence over the animal-activity HYPOTHESIS whenever
        // there is a change to explain: the unfalsifiable claim yields to the one
        // with a physical correlate. Both beliefs are still reported.
        double environmental_explains_strength = 0.5;
        double possible_animal_belief = 0.5;         // belief required for the hypothesis label
        size_t possible_animal_min_sources = 2;      // independent confirming sources
        Config() = default;
    };

    static Config defaults();
    // The default config carries the suite's own feature/detector names so the
    // observations are distinguishable from the existing synthetic detectors.
    explicit PrecursorSuite(Config cfg = defaults());

    // ---- ingest (all inputs are AUTHORIZED, already bounded) ----

    // Camera path: changed-fraction between consecutive permitted luma planes.
    ActivityChangeDetector::Result observe_motion(double changed_fraction,
                                                  std::chrono::system_clock::time_point now);

    // Microphone path: extract bounded features from permitted PCM and feed the
    // activity + statistical-deviation detectors. Raw audio is never retained.
    AudioFrameFeatures observe_audio(const int16_t* pcm, size_t frames, uint32_t channels,
                                     uint32_t sample_rate,
                                     std::chrono::system_clock::time_point now);

    // Pre-extracted audio features (for callers that extract elsewhere).
    ActivityChangeDetector::Result observe_audio_features(const AudioFrameFeatures& f,
                                                          std::chrono::system_clock::time_point now);

    // Arbitrary bounded feature vector (e.g. a future approved embedding).
    VectorDriftDetector::Result observe_vector(const std::vector<double>& v,
                                               std::chrono::system_clock::time_point now);

    // An authorized environmental measurement moved by `deviation_strength`
    // (0..1) relative to its own baseline. Comes from EnvironmentalCorrelation.
    void observe_environmental(double deviation_strength, std::chrono::system_clock::time_point now);

    // ---- evaluate ----

    PrecursorFusion fusion() const;

    // Observations for the existing pipeline: one per active detector plus the
    // fusion result. Bounded, provenance-tagged, with explainable features.
    std::vector<Observation> observations(const std::string& source,
                                         Provenance provenance = Provenance::synthetic) const;

    const ActivityChangeDetector::Result& motion() const { return motion_result_; }
    const ActivityChangeDetector::Result& audio() const { return audio_result_; }
    const AnomalyResult& audio_deviation() const { return audio_deviation_result_; }
    const VectorDriftDetector::Result& vector() const { return vector_result_; }

    void reset();
    size_t motion_samples() const { return motion_detector_.sample_count(); }
    size_t audio_samples() const { return audio_detector_.sample_count(); }
    size_t vector_samples() const { return vector_detector_.sample_count(); }

private:
    Config cfg_;
    ActivityChangeDetector motion_detector_;
    ActivityChangeDetector audio_detector_;
    AnomalyMonitor audio_deviation_monitor_;
    VectorDriftDetector vector_detector_;

    ActivityChangeDetector::Result motion_result_{};
    ActivityChangeDetector::Result audio_result_{};
    AnomalyResult audio_deviation_result_{};
    VectorDriftDetector::Result vector_result_{};
    bool has_motion_ = false;
    bool has_audio_ = false;
    bool has_deviation_ = false;
    bool has_vector_ = false;
    bool has_environmental_ = false;
    double environmental_strength_ = 0.0;
};

}  // namespace homeguardian
