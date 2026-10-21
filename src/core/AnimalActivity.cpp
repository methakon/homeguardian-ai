#include "core/AnimalActivity.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace homeguardian {

AnimalActivityFinding AnimalActivityAssessor::assess(
    const std::vector<ActivityEvidence>& evidence) const {
    AnimalActivityFinding finding;

    // Disabled layer: say so plainly. The single worst outcome for a component like this is silence
    // that a caller reads as "nothing there", so the disabled state is an explicit finding.
    if (!policy_.enabled) {
        finding.classification = AnimalActivityClass::insufficient_evidence;
        finding.sentence = "the animal-activity layer is disabled";
        finding.uncertainty = "nothing is claimed about any animal";
        finding.reason = "enabled=false; this layer is opt-in and off by default";
        return finding;
    }

    // Count SUSTAINED, confirmed changes only. A spike is explicitly not evidence here: it is the same
    // rule the activity detector uses ("one anomalous frame is a door slamming").
    size_t motion = 0;
    size_t audio = 0;
    double confidence_sum = 0.0;
    size_t contributors = 0;
    size_t real_inputs = 0;
    size_t synthetic_inputs = 0;

    for (const auto& e : evidence) {
        if (!e.sustained_change_confirmed) continue;
        if (e.confidence < policy_.min_confidence) continue;
        ++contributors;
        confidence_sum += e.confidence;
        if (e.provenance == Provenance::real) {
            ++real_inputs;
        } else {
            ++synthetic_inputs;
        }
        if (e.kind == ActivityEvidenceKind::motion) ++motion;
        if (e.kind == ActivityEvidenceKind::audio) ++audio;
    }

    finding.motion_sources = motion;
    finding.audio_sources = audio;
    finding.provenance = derive_evidence_provenance(real_inputs, synthetic_inputs);
    finding.confidence = contributors == 0 ? 0.0 : confidence_sum / double(contributors);

    if (contributors == 0) {
        finding.classification = AnimalActivityClass::no_evidence;
        finding.sentence = "no sustained activity change in view";
        finding.uncertainty = "a quiet window is not evidence of absence: nothing was measured";
        finding.reason = "no sustained, sufficiently confident evidence contributed";
        return finding;
    }

    if (motion == 0) {
        // Audio or environmental evidence with no motion: still not an animal claim.
        finding.classification = AnimalActivityClass::insufficient_evidence;
        finding.sentence = "a sustained non-motion change was measured";
        finding.uncertainty = "this is not an animal identification and no species is implied";
        finding.reason = "no motion source contributed";
        return finding;
    }

    // One sustained motion source: motion, and only motion.
    const bool corroborated = (motion + audio) >= policy_.min_sustained_sources;
    const bool audio_ok = !policy_.require_audio_corroboration || audio > 0;
    if (!corroborated || !audio_ok) {
        finding.classification = AnimalActivityClass::motion_only;
        finding.sentence = "a sustained change in motion in view";
        finding.uncertainty =
            "this is not an animal identification, no species is implied, and no individual is tracked";
        std::ostringstream os;
        os << "one sustained motion source at confidence " << std::fixed << std::setprecision(2)
           << finding.confidence << "; a candidate hypothesis needs "
           << policy_.min_sustained_sources << " corroborating sustained sources";
        finding.reason = os.str();
        return finding;
    }

    // Layer 3: a hypothesis. It says the word hypothesis, it names what it is not, and it can never
    // become a species claim - there is no type for one in this build.
    finding.classification = AnimalActivityClass::candidate_animal_activity;
    finding.sentence =
        "a multi-source sustained activity pattern consistent with a small body moving in view";
    finding.uncertainty =
        "HYPOTHESIS ONLY - this is not a species identification, not an individual identification, and "
        "not a behaviour interpretation; a curtain, a shadow or a person produces the same evidence";
    std::ostringstream os;
    os << "sustained sources: motion=" << motion << " audio=" << audio << ", mean confidence "
       << std::fixed << std::setprecision(2) << finding.confidence << ", provenance "
       << evidence_provenance_to_string(finding.provenance);
    if (audio == 0) {
        os << "; no audio channel is available in this build, so corroboration is motion-only";
    }
    finding.reason = os.str();
    return finding;
}

}  // namespace homeguardian
