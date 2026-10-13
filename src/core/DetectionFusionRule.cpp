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

    // Only a positively DETECTED, sufficiently confident fusion result may
    // raise an alert. inconclusive / unavailable / not_detected -> no alert.
    if (fr.verdict != FusionVerdict::detected) return std::nullopt;
    if (fr.confidence < cfg_.min_confidence) return std::nullopt;
    if (fr.contributing < cfg_.min_usable) return std::nullopt;

    // Severity is derived from the actual evidence (fused confidence).
    const Severity severity = fr.confidence >= cfg_.warning_confidence
                                  ? Severity::warning
                                  : Severity::info;

    std::ostringstream msg;
    msg << "synthetic activity detected: fusion confidence "
        << std::fixed << std::setprecision(2) << fr.confidence << " from "
        << fr.contributing << " contributing detector observation(s)";

    // Distinct contributing detector ids (bounded, non-sensitive).
    std::set<std::string> uniq(fr.sources.begin(), fr.sources.end());
    nlohmann::json detector_ids = nlohmann::json::array();
    for (const auto& d : uniq) {
        if (detector_ids.size() >= cfg_.max_contributing) break;
        detector_ids.push_back(d);
    }

    nlohmann::json details = {
        {"fusion_verdict", fusion_verdict_to_string(fr.verdict)},
        {"fusion_confidence", fr.confidence},
        {"contributing", fr.contributing},
        {"inconclusive", inconclusive},
        {"provenance", "synthetic"},   // this build is simulator-only
        {"detector_ids", detector_ids},
        {"reason", fr.reason},
    };

    return Alert::create("alert-" + UUID::generate(), severity, msg.str(),
                         usable_event_ids, fr.confidence, name_, details);
}

}  // namespace homeguardian
