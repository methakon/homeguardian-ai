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

    // 2) Scenario A — normal household activity (no qualifying concern).
    {
        DemoScenario s;
        s.id = "normal";
        s.title = "Normal household activity";
        s.description =
            "A single authorized synthetic observation: routine activity that "
            "does not meet the alert rule.";
        s.cycles = 1;
        s.grant_enabled = true;
        s.grant_assistant = false;   // one device active
        s.run_detection = true;
        s.detection_energies = {0.72, 0.68};
        s.expectation =
            "no alert: one observation is below the >=2-in-60s correlation "
            "threshold, even though activity was detected.";
        v.push_back(std::move(s));
    }

    // 3) Scenario B — unusual activity (traceable alert).
    {
        DemoScenario s;
        s.id = "unusual";
        s.title = "Unusual activity";
        s.description =
            "Repeated authorized synthetic observations within the window "
            "trigger the correlation rule and produce an alert.";
        s.cycles = 5;
        s.grant_enabled = true;
        s.grant_assistant = true;
        s.run_detection = true;
        s.detection_energies = {0.95, 0.90, 0.88, 0.92, 0.85};
        s.expectation =
            "one alert delivered and persisted; repeated duplicates suppressed; "
            "the alert references the observed synthetic pattern.";
        v.push_back(std::move(s));
    }

    // 4) Scenario C — inconclusive detection (ambiguous evidence).
    {
        DemoScenario s;
        s.id = "inconclusive";
        s.title = "Inconclusive detection";
        s.description =
            "Ambiguous synthetic evidence near the detector threshold: the "
            "detector and fusion stages report inconclusive.";
        s.cycles = 1;
        s.grant_enabled = true;
        s.grant_assistant = false;
        s.run_detection = true;
        s.detection_energies = {0.52, 0.49, 0.51};
        s.expectation =
            "no definitive classification and no alert: an uncertain result is "
            "never promoted to a confirmed alert.";
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
