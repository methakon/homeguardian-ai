#include "core/DemoScenarios.h"

namespace homeguardian {

namespace {

// The deterministic catalogue. Order matters only for display.
//
// Energy-sequence notes (SyntheticActivityDetector: threshold=0.5,
// min_quality=0.3; quality = min(1, |energy-0.5|/0.3)):
//   - energy >= 0.59 with quality>=0.3 -> detected
//   - energy <= 0.41 with quality>=0.3 -> not_detected
//   - energy within 0.09 of 0.5       -> inconclusive (low quality)
// These values are chosen so each scenario's detection outcome is stable.
std::vector<DemoScenario> build_catalogue() {
    std::vector<DemoScenario> v;

    // 1) Default — the original, backwards-compatible demo.
    {
        DemoScenario s;
        s.id = "default";
        s.title = "Default (backwards-compatible)";
        s.description =
            "The original demo: repeated authorized synthetic observations "
            "trigger the presence correlation rule.";
        s.cycles = 5;
        s.grant_enabled = true;
        s.grant_assistant = true;
        s.run_detection = true;
        s.detection_energies = {0.8, 0.75, 0.52, 0.82, 0.78};
        s.expectation =
            "activity detected; one presence alert delivered; later repeats "
            "suppressed by the alert policy.";
        v.push_back(std::move(s));
    }

    // 2) Scenario A — normal household activity (no qualifying detection).
    {
        DemoScenario s;
        s.id = "normal";
        s.title = "Normal household activity";
        s.description =
            "Routine synthetic activity BELOW the detection baseline: the "
            "detector runs and the fused verdict is not_detected.";
        s.cycles = 2;
        s.grant_enabled = true;
        s.grant_assistant = true;
        s.run_detection = true;
        // Both energies are below the 0.5 threshold (and clear of the
        // inconclusive band), so both detector observations are not_detected.
        s.detection_energies = {0.24, 0.28};
        s.expectation =
            "no alert: the fused detection verdict is not_detected. Activity was "
            "observed and evaluated, but nothing qualified as a detection.";
        v.push_back(std::move(s));
    }

    // 3) Scenario B — unusual activity (traceable detection-driven alert).
    {
        DemoScenario s;
        s.id = "unusual";
        s.title = "Unusual activity";
        s.description =
            "Sustained synthetic activity above the detection baseline: the "
            "detector and fusion agree, and the alert is raised from that "
            "evidence.";
        s.cycles = 5;
        s.grant_enabled = true;
        s.grant_assistant = true;
        s.run_detection = true;
        s.detection_energies = {0.95, 0.90, 0.88, 0.92, 0.85};
        s.expectation =
            "detection-driven alert: every detector observation is detected, "
            "fusion confidence is 1.00, and the alert severity follows that "
            "confidence; repeats are suppressed by the alert policy.";
        v.push_back(std::move(s));
    }

    // 4) Scenario C — inconclusive detection (ambiguous evidence).
    {
        DemoScenario s;
        s.id = "inconclusive";
        s.title = "Inconclusive detection";
        s.description =
            "Ambiguous synthetic evidence near the detector threshold: every "
            "detector observation is inconclusive and fusion refuses a verdict.";
        s.cycles = 3;
        s.grant_enabled = true;
        s.grant_assistant = false;
        s.run_detection = true;
        s.detection_energies = {0.52, 0.49, 0.51};
        s.expectation =
            "no alert: the detector reported inconclusive for every input and "
            "fusion returned unavailable. An uncertain result is never promoted "
            "to a confirmed alert.";
        v.push_back(std::move(s));
    }

    // 5) Scenario D — consent denied (fail-closed acquisition).
    {
        DemoScenario s;
        s.id = "consent-denied";
        s.title = "Consent denied";
        s.description =
            "The assistant consumer's synthetic consent is withdrawn; its "
            "acquisition is denied by the consent gate while the other "
            "consumer remains authorized.";
        s.cycles = 1;
        s.grant_enabled = true;
        s.grant_assistant = true;
        s.withdraw_assistant = true;
        s.run_detection = false;     // no observation for the denied consumer
        s.detection_energies = {};
        s.expectation =
            "the denied consumer receives no media and produces no event; the "
            "denial is recorded and enforcement_ok stays true.";
        v.push_back(std::move(s));
    }

    return v;
}

}  // namespace

const std::vector<DemoScenario>& demo_scenarios() {
    static const std::vector<DemoScenario> kCatalogue = build_catalogue();
    return kCatalogue;
}

const DemoScenario* find_demo_scenario(std::string_view id) {
    for (const auto& s : demo_scenarios()) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string default_demo_scenario_id() {
    return "default";
}

bool is_valid_scenario_token(std::string_view id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace homeguardian
