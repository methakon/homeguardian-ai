#pragma once

// DetectionFusionRule: a correlation rule that raises an alert ONLY when the
// REAL detection/fusion evidence qualifies.
//
// WHY (L-11): the previous demo raised a generic "presence" alert from raw
// observation counts, completely independently of what the detector/fusion
// stage concluded. That made the alert un-explainable and disconnected from the
// system's own detection. This rule closes that gap: it reconstructs
// Observations from the inference events already in the pipeline window, fuses
// them with FusionEngine, and returns an alert only when the fused verdict is
// `detected` with sufficient confidence.
//
// WHAT IT IS NOT: it is not a safety/medical verdict. "detected" means the
// configured (synthetic) detectors agreed something was present. It makes no
// claim that a fall, illness, emergency or dangerous event occurred.
//
// HARD RULE: inconclusive / unavailable / not_detected evidence NEVER produces
// an alert. An uncertain result is never promoted to a confirmation.

#include "core/ActivityEpisode.h"
#include "core/Alert.h"
#include "core/CorrelationRule.h"
#include "core/FusionEngine.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace homeguardian {

class DetectionFusionRule : public ICorrelationRule {
public:
    struct Config {
        size_t min_usable = 2;          // usable votes required (>= 2)
        double min_confidence = 0.5;    // minimum fused confidence to alert
        double warning_confidence = 0.8; // >= this -> Severity::warning
        size_t max_contributing = 10;   // bound on reported contributing ids
        // Which detectors count as fusion evidence. An EMPTY list means "accept
        // every detector" (backwards compatible). A non-empty list is a strict
        // allowlist: observations from any other detector still flow through the
        // pipeline (validated, counted, auditable) but are NOT votes.
        //
        // L-12: the default is the activity detector only. The statistical
        // anomaly detector (detector id "activity-anomaly") is therefore
        // ADVISORY by default: its result is reported and audited but does not
        // influence the alert. Adding it here makes that influence explicit and
        // is covered by tests.
        std::vector<std::string> evidence_detector_ids = {"activity-energy"};
        // Opt-in: also consider a bounded activity EPISODE (ActivityEpisodeSelector) as qualifying
        // evidence. OFF by default, so the release's decision path is bit-for-bit what it was; a
        // caller that turns it on is choosing to judge a short sustained change on its own evidence
        // instead of on the size of the window it arrived in. No threshold in this class changes.
        bool use_episode_evidence = false;
        ActivityEpisodeSelector::Config episode{};
        Config() = default;             // explicit: usable in a default argument
    };

    // Value-init via defaults() — see AnomalyDetector.h note on aggregate-init
    // ordering for default arguments (a nested Config with member initializers
    // cannot be used directly as a default argument).
    static Config defaults() { return Config{}; }

    explicit DetectionFusionRule(std::string name, Config cfg = defaults())
        : name_(std::move(name)), cfg_(std::move(cfg)), fusion_() {}

    // Replace the evidence allowlist at runtime (demo/test configuration). An
    // empty vector means "accept all detectors". Single-threaded use only (the
    // demo bridge and the tests call this between runs).
    void set_evidence_detector_ids(std::vector<std::string> ids) {
        cfg_.evidence_detector_ids = std::move(ids);
    }
    const std::vector<std::string>& evidence_detector_ids() const {
        return cfg_.evidence_detector_ids;
    }
    // True when `detector_id` may contribute evidence (empty list = all).
    bool accepts_detector(const std::string& detector_id) const {
        if (cfg_.evidence_detector_ids.empty()) return true;
        for (const auto& id : cfg_.evidence_detector_ids) {
            if (id == detector_id) return true;
        }
        return false;
    }

    std::optional<Alert> evaluate(const std::vector<Event>& window) override;

    std::string name() const override { return name_; }
    std::string description() const override {
        return "Detection-driven rule: fused detector evidence must agree "
               "before an alert is raised";
    }

private:
    // One place assembles the alert, whichever evidence path qualified, so the message and the
    // provenance fields cannot drift apart between them.
    std::optional<Alert> build_alert(double confidence, size_t contributing, const std::string& reason,
                                     EvidenceProvenance provenance, size_t real_inputs,
                                     size_t synthetic_inputs, const char* evidence_mode,
                                     const ActivityEpisode* episode, const FusionResult& classic,
                                     const std::vector<std::string>& usable_event_ids,
                                     size_t inconclusive) const;

    std::string name_;
    Config cfg_;
    FusionEngine fusion_;
};

}  // namespace homeguardian
