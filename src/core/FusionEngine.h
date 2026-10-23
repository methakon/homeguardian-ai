#pragma once

// FusionEngine: combines multiple timestamped Observations into a single,
// explainable fused result. Pure C++ standard library — no new dependency.
//
// WHY: multimodal fusion is where a home-safety system either earns trust or
// loses it. This engine fuses detector outputs conservatively and EXPLAINABLY:
//   - Confidence propagation: the fused confidence is derived from the inputs
//     (agreement raises it, conflict lowers it), never invented.
//   - Conflict handling: when detectors disagree, the result is marked
//     `inconclusive` rather than silently picking a winner.
//   - Missing-modality handling: absent inputs do not fabricate a result; the
//     fusion reports which modalities were present.
//   - Time window: only observations within `window` of the newest input are
//     fused, so stale inputs do not contaminate a fresh decision.
//   - Unavailable handling: an `unavailable` observation never counts as
//     evidence for "detected"; it only lowers confidence / marks inconclusive.
//
// WHAT IT IS NOT:
//  - NOT a safety, medical, or behavioural verdict. A fused "detected" means
//    "the configured detectors agreed something was present" — the caller maps
//    that to an alert via the EXISTING AlertPolicy. This class makes no claim
//    that a fall, illness, emergency, or dangerous event occurred.
//  - NOT an unrestricted behavioural profiler and NOT an authorization learner.

#include "core/Observation.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace homeguardian {

enum class FusionVerdict {
    detected,       // detectors agreed something is present
    not_detected,   // detectors agreed nothing is present
    inconclusive,   // detectors conflicted, or too few reliable inputs
    unavailable     // no usable detector output at all
};

inline const char* fusion_verdict_to_string(FusionVerdict v) {
    switch (v) {
        case FusionVerdict::detected: return "detected";
        case FusionVerdict::not_detected: return "not_detected";
        case FusionVerdict::inconclusive: return "inconclusive";
        case FusionVerdict::unavailable: return "unavailable";
    }
    return "unknown";
}

// The provenance of the EVIDENCE that produced a fused result - not of the engine, the build or the
// caller. A caller that is about to publish a claim must be able to state this truthfully, so it is
// derived here from the votes that were actually fused and cannot be set by anyone else.
//
//   real       every contributing observation came from a physical device frame/sample
//   synthetic  every contributing observation came from a fixture or the simulator
//   mixed      both kinds contributed - stated as mixed, never promoted to either extreme
//   uncertain  nothing contributed, so nothing can be claimed about the evidence
//
// `uncertain` is the default for a default-constructed result: the fail-safe direction is to claim
// less, so a result that was never computed cannot be read as real evidence.
enum class EvidenceProvenance {
    real,
    synthetic,
    mixed,
    uncertain
};

inline const char* evidence_provenance_to_string(EvidenceProvenance p) {
    switch (p) {
        case EvidenceProvenance::real:      return "real";
        case EvidenceProvenance::synthetic: return "synthetic";
        case EvidenceProvenance::mixed:     return "mixed";
        case EvidenceProvenance::uncertain: return "uncertain";
    }
    return "uncertain";
}

// The one place the rule is written down: a mix is `mixed`, an empty set is `uncertain`, and a
// single kind is itself. There is deliberately no path from a synthetic input to a `real` verdict.
inline EvidenceProvenance derive_evidence_provenance(size_t real_inputs, size_t synthetic_inputs) {
    if (real_inputs > 0 && synthetic_inputs > 0) return EvidenceProvenance::mixed;
    if (real_inputs > 0) return EvidenceProvenance::real;
    if (synthetic_inputs > 0) return EvidenceProvenance::synthetic;
    return EvidenceProvenance::uncertain;
}

struct FusionResult {
    FusionVerdict verdict = FusionVerdict::unavailable;
    double confidence = 0.0;                 // [0,1], derived from inputs
    size_t contributing = 0;                 // observations actually used
    std::vector<std::string> sources;        // which detector_ids contributed
    std::string reason;                      // bounded human-readable explanation
    EvidenceProvenance provenance = EvidenceProvenance::uncertain;  // derived from the votes
    size_t real_inputs = 0;                  // contributing observations that were physical
    size_t synthetic_inputs = 0;             // contributing observations that were fixtures
};

class FusionEngine {
public:
    struct Config {
        std::chrono::milliseconds window{60000};  // fuse inputs within this of newest
        size_t min_contributing = 2;              // need >= this many usable inputs
        double agreement_threshold = 0.6;         // fraction that must agree
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    // Value-init via defaults() — see AnomalyDetector.h note on aggregate-init
    // ordering for default arguments.
    explicit FusionEngine(Config cfg = defaults()) : cfg_(cfg) {}

    // Fuse a set of observations. The engine does not mutate the inputs.
    FusionResult fuse(const std::vector<Observation>& obs) const;

private:
    Config cfg_;
};

}  // namespace homeguardian
