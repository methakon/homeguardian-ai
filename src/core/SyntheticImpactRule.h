#pragma once

// SyntheticImpactRule: the correlation rule that turns a CONFIRMED synthetic
// impact pattern into a critical-severity alert (roadmap H-01s).
//
// WHY A SEPARATE RULE: severity prioritisation must be genuine and auditable, not
// a relabelled activity alert. This rule reads only the impact classifier's
// observations (detector id "impact-signal") and fires only when the pattern is
// CONFIRMED by more than one observation:
//   - at least one observation with phase "confirmed" (the completed transient +
//     stillness pattern), AND
//   - at least `min_supporting` (2) impact observations in the window
//     (the transient precursor counts as support).
//
// A lone high-intensity sample is NOT enough: the classifier reports it as
// inconclusive and this rule stays silent. Insufficient or ambiguous evidence
// therefore never becomes a critical classification.
//
// SEVERITY: a confirmed pattern maps to Severity::critical. That mapping is
// explicit here, is the ONLY source of critical alerts in the build, and is
// covered by tests.
//
// IT IS NOT FALL DETECTION. H-01 (real pose-based fall detection) is not
// implemented; the alert message and evidence say so explicitly, and no
// emergency service, notification channel or external system is involved.

#include "core/Alert.h"
#include "core/CorrelationRule.h"

#include <cstddef>
#include <string>
#include <vector>

namespace homeguardian {

class SyntheticImpactRule : public ICorrelationRule {
public:
    struct Config {
        std::string detector_id = "impact-signal";
        size_t min_supporting = 2;    // impact observations required in the window
        size_t max_contributing = 10; // bound on reported contributing ids
        Config() = default;
    };

    static Config defaults() { return Config{}; }

    explicit SyntheticImpactRule(std::string name, Config cfg = defaults())
        : name_(std::move(name)), cfg_(std::move(cfg)) {}

    std::optional<Alert> evaluate(const std::vector<Event>& window) override;

    std::string name() const override { return name_; }
    std::string description() const override {
        return "Synthetic impact rule: a confirmed transient-then-stillness pattern "
               "raises a critical alert (simulation only; not fall detection)";
    }

    const Config& config() const { return cfg_; }

private:
    std::string name_;
    Config cfg_;
};

}  // namespace homeguardian
