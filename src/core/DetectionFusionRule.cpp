#include "core/DetectionFusionRule.h"

#include "core/Observation.h"
#include "core/UUID.h"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace homeguardian {

std::optional<Alert> DetectionFusionRule::evaluate(const std::vector<Event>& window) {
    // Rebuild the detection evidence from the inference events the REAL pipeline
    // carried. Nothing is invented here: if the detection stage never ran (e.g.
    // capture disabled, or consent withdrawn so no acquisition occurred), there
    // are no inference events and this rule cannot fire.
    std::vector<Observation> observations;
    std::vector<std::string> usable_event_ids;
    size_t inconclusive = 0;
    for (const auto& ev : window) {
        const auto o = Observation::from_event(ev);
        if (!o) continue;
        // Evidence allowlist (L-12): observations from detectors that are not
        // accepted are NOT votes and do not even count as unusable — they are
        // simply not evidence for this rule. They still flowed through the
        // pipeline and remain in history/audit.
        if (!accepts_detector(o->detector_id)) continue;
        if (o->status == DetectorStatus::inconclusive ||
            o->status == DetectorStatus::unavailable) {
            ++inconclusive;
        }
        if (o->status == DetectorStatus::detected ||
            o->status == DetectorStatus::not_detected) {
            if (usable_event_ids.size() < cfg_.max_contributing) {
                usable_event_ids.push_back(ev.get_event_id());
            }
        }
        observations.push_back(*o);
    }
    if (observations.empty()) return std::nullopt;

    const FusionResult fr = fusion_.fuse(observations);

    // --- evidence path 2 (opt-in): a bounded activity episode ----------------------------------
    // Judged on its own evidence, so a short sustained change is not outvoted by the quiet window it
    // arrived in. Nothing here lowers a threshold, and the episode selector refuses quiet scenes,
    // one-off spikes and runs of confirmed FALLS (see ActivityEpisode.h for why each refusal exists).
    if (cfg_.use_episode_evidence) {
        const ActivityEpisodeSelector selector(cfg_.episode);
        if (const auto episode = selector.best(observations)) {
            return build_alert(episode->mean_confidence, episode->length, episode->reason,
                               episode->provenance, episode->real_inputs, episode->synthetic_inputs,
                               "episode", &*episode, fr, usable_event_ids, inconclusive);
        }
        // No episode: fall through to the classic decision, which refuses exactly as it does today.
    }

    // --- evidence path 1 (default): the existing fused-evidence decision ------------------------
    // Only a positively DETECTED, sufficiently confident fusion result may raise an alert.
    // inconclusive / unavailable / not_detected -> no alert.
    if (fr.verdict != FusionVerdict::detected) return std::nullopt;
    if (fr.confidence < cfg_.min_confidence) return std::nullopt;
    if (fr.contributing < cfg_.min_usable) return std::nullopt;

    return build_alert(fr.confidence, fr.contributing, fr.reason, fr.provenance, fr.real_inputs,
                       fr.synthetic_inputs, "fusion", nullptr, fr, usable_event_ids, inconclusive);
}

std::optional<Alert> DetectionFusionRule::build_alert(
    double confidence, size_t contributing, const std::string& reason, EvidenceProvenance provenance,
    size_t real_inputs, size_t synthetic_inputs, const char* evidence_mode,
    const ActivityEpisode* episode, const FusionResult& classic,
    const std::vector<std::string>& usable_event_ids, size_t inconclusive) const {
    // Severity is derived from the actual evidence, from whichever path qualified.
    const Severity severity =
        confidence >= cfg_.warning_confidence ? Severity::warning : Severity::info;

    // The message states the provenance of the evidence that produced it. The all-synthetic wording
    // is unchanged, so existing documentation and assertions stay true; what changes is that a real
    // or mixed window can no longer be described as synthetic. No path here can upgrade anything to
    // `real`: the label comes from derive_evidence_provenance over the contributing observations.
    const char* subject = "activity detected under uncertain provenance";
    switch (provenance) {
        case EvidenceProvenance::synthetic: subject = "synthetic activity detected"; break;
        case EvidenceProvenance::real:      subject = "real-sensor activity detected"; break;
        case EvidenceProvenance::mixed:     subject = "mixed-provenance activity detected"; break;
        case EvidenceProvenance::uncertain: subject = "activity detected under uncertain provenance"; break;
    }

    // "fusion confidence" is the wording the default (all-synthetic) path has always used, so every
    // existing log line, document quote and assertion stays true. The episode path names itself.
    std::ostringstream msg;
    msg << subject << ": " << (episode != nullptr ? "episode" : "fusion") << " confidence "
        << std::fixed << std::setprecision(2) << confidence
        << " from " << contributing << " contributing detector observation(s)";

    // Distinct contributing detector ids (bounded, non-sensitive).
    std::set<std::string> uniq(classic.sources.begin(), classic.sources.end());
    nlohmann::json detector_ids = nlohmann::json::array();
    for (const auto& d : uniq) {
        if (detector_ids.size() >= cfg_.max_contributing) break;
        detector_ids.push_back(d);
    }

    nlohmann::json provenance_sources = {
        {"real", real_inputs},
        {"synthetic", synthetic_inputs},
    };

    nlohmann::json details = {
        {"fusion_verdict", fusion_verdict_to_string(classic.verdict)},
        {"fusion_confidence", classic.confidence},
        {"contributing", contributing},
        {"inconclusive", inconclusive},
        // Derived from the contributing observations, never asserted. "real" is reachable only from
        // real evidence; "mixed" is stated as mixed; an empty evidence set is "uncertain".
        {"provenance", evidence_provenance_to_string(provenance)},
        {"provenance_sources", provenance_sources},
        {"provenance_derived", true},
        {"evidence_mode", evidence_mode},
        {"detector_ids", detector_ids},
        {"reason", reason},
    };
    if (episode != nullptr) {
        details["episode"] = {
            {"length", episode->length},
            {"direction", episode->direction},
            {"mean_confidence", episode->mean_confidence},
            {"weakest_confidence", episode->weakest_confidence},
            {"provenance", evidence_provenance_to_string(episode->provenance)},
        };
    }

    return Alert::create("alert-" + UUID::generate(), severity, msg.str(), usable_event_ids,
                         confidence, name_, details);
}

}  // namespace homeguardian
