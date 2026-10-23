#pragma once

// DemoScenarios: a small, DETERMINISTIC catalogue of synthetic household-safety
// scenarios for the demo. Each entry is a plain data fixture (id + bounded
// knobs); the behaviour is produced by running the spec through the REAL
// Application pipeline (see DemoScenarioRunner.h). The catalogue deliberately
// contains NO pre-baked alert outcomes — only the inputs a scenario applies, so
// every resulting alert is caused by actual synthetic input and the existing
// correlation/alert policy.
//
// All scenarios are SIMULATOR-ONLY: no physical sensor is ever activated. The
// scenario inputs are: how many simulator ticks to run, which consumers hold
// synthetic consent, whether one consumer's consent is withdrawn, and the
// synthetic detection-energy sequence fed to SyntheticActivityDetector.
//
// The ids form a strict allowlist: any id not present here is rejected by the
// HTTP layer and the bridge.

#include <string>
#include <string_view>
#include <vector>

namespace homeguardian {

struct DemoScenario {
    std::string id;                     // allowlisted, [a-z0-9-]
    std::string title;                  // human-readable title
    std::string description;            // what the scenario demonstrates
    int cycles = 5;                     // simulator ticks (>= 0)
    bool grant_enabled = true;          // grant synthetic consent to device A
    bool grant_assistant = true;        // grant synthetic consent to device B
    bool withdraw_assistant = false;    // withdraw B's consent (denied path)
    bool run_detection = true;          // run the activity detection stage
    bool run_anomaly = false;           // run the statistical anomaly stage (L-12)
    bool anomaly_missing_probe = false; // also exercise the missing-observation path
    bool run_impact = false;            // run the synthetic impact classifier (H-01s)
    std::vector<double> impact_intensities;  // bounded synthetic impact feature [0,1]
    std::vector<double> detection_energies;  // synthetic energies in [0,1]
    std::string expectation;            // what the demo expects (documentation)
};

// The full, ordered catalogue. First entry is the backwards-compatible default.
const std::vector<DemoScenario>& demo_scenarios();

// Look up a scenario by id. Returns nullptr for an unknown id (the caller must
// reject it — this is the allowlist boundary).
const DemoScenario* find_demo_scenario(std::string_view id);

// The default (backwards-compatible) scenario id.
std::string default_demo_scenario_id();

// True when `id` is a syntactically valid scenario token (lowercase letters,
// digits, hyphen; 1..32 chars). This is a SYNTAX check only — use
// find_demo_scenario() for the allowlist.
bool is_valid_scenario_token(std::string_view id);

}  // namespace homeguardian
