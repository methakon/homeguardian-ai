#pragma once

// Observation: the detection-framework data model for HomeGuardian.
//
// WHY: the existing Event model already carries type, schema version,
// timestamps, source, confidence, and a bounded payload. What the detection
// layer additionally needs — and which Event does not express — is:
//   - provenance: is this observation SYNTHETIC (from the in-process simulator
//     / offline fixture) or REAL (from a physical sensor)?
//   - detector identity: which detector produced it, and which model/version?
//   - an explicit result status: detected / not_detected / unavailable /
//     inconclusive (so "we could not run the detector" is never confused with
//     "the detector ran and found nothing").
//
// An Observation is a thin, validated wrapper that CONVERTS to a standard
// Event via to_event(), so it flows through the EXISTING event pipeline,
// correlation rules, AlertPolicy, and persistence. It does NOT create a second
// event-processing engine.
//
// PRIVACY: an Observation carries only bounded, validated feature data (counts,
// booleans, small numeric features, quality scores). It NEVER carries raw
// audio, raw video, facial embeddings, biometric templates, or transcripts.

#include "core/Event.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

// How the observation was produced. Synthetic is the default and the only kind
// this build produces — no physical sensor is ever activated.
enum class Provenance {
    synthetic,   // in-process simulator or offline test fixture
    real         // a physical sensor (NOT used in this build; reserved)
};

inline const char* provenance_to_string(Provenance p) {
    return p == Provenance::synthetic ? "synthetic" : "real";
}

// Explicit detector result status. "unavailable" (detector/model not present or
// failed) is deliberately distinct from "not_detected" (ran, found nothing) and
// "inconclusive" (ran, but confidence/quality too low to decide).
enum class DetectorStatus {
    detected,       // the target was found with sufficient confidence
    not_detected,   // the detector ran and did not find the target
    unavailable,    // the detector or model could not run
    inconclusive    // the detector ran but the result is not reliable
};

inline const char* detector_status_to_string(DetectorStatus s) {
    switch (s) {
        case DetectorStatus::detected: return "detected";
        case DetectorStatus::not_detected: return "not_detected";
        case DetectorStatus::unavailable: return "unavailable";
        case DetectorStatus::inconclusive: return "inconclusive";
    }
    return "unknown";
}

struct Observation {
    // Identity / provenance.
    std::string observation_id;             // non-empty
    std::string source;                     // non-empty (e.g. device/sensor id)
    Provenance provenance = Provenance::synthetic;
    std::string detector_id;                // e.g. "motion-energy" (non-empty)
    std::string model_id;                   // e.g. "synthetic-v1" (may be empty)
    uint32_t schema_version = 1;

    // Timing.
    std::chrono::system_clock::time_point observation_timestamp{};

    // Result.
    DetectorStatus status = DetectorStatus::not_detected;
    std::optional<double> confidence;       // [0,1] when present
    std::optional<double> quality;          // [0,1] input quality, when present

    // Bounded, validated feature data. Keys/values are small and non-sensitive.
    nlohmann::json features = nlohmann::json::object();

    // Optional free-text note for explainability (e.g. why inconclusive). Must
    // not contain sensitive data. Bounded in length.
    std::string note;

    // ---- validation + conversion ----

    // Validate fields; throws std::invalid_argument on any violation. Keeps
    // the same fail-fast contract as Event.
    void validate() const;

    // Convert to a standard Event (type = inference) so it flows through the
    // existing pipeline. The provenance/detector/status are carried in the
    // payload and processing_metadata for explainability and audit, while the
    // confidence is mapped onto the Event's confidence field when present.
    Event to_event() const;

    // ---- bounded feature helpers ----

    // Hard bounds on feature data so a malformed detector cannot bloat an
    // observation. Enforced by validate().
    static constexpr size_t kMaxFeatureKeys = 32;
    static constexpr size_t kMaxNoteChars = 256;
    static constexpr size_t kMaxFeatureJsonBytes = 2048;
};

}  // namespace homeguardian
