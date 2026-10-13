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
        Config() = default;             // explicit: usable in a default argument
    };

    // Value-init via defaults() — see AnomalyDetector.h note on aggregate-init
    // ordering for default arguments (a nested Config with member initializers
    // cannot be used directly as a default argument).
    static Config defaults() { return Config{}; }

    explicit DetectionFusionRule(std::string name, Config cfg = defaults())
        : name_(std::move(name)), cfg_(cfg), fusion_() {}

    std::optional<Alert> evaluate(const std::vector<Event>& window) override;

    std::string name() const override { return name_; }
    std::string description() const override {
        return "Detection-driven rule: fused detector evidence must agree "
               "before an alert is raised";
    }

private:
    std::string name_;
    Config cfg_;
    FusionEngine fusion_;
};

}  // namespace homeguardian
