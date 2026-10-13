#pragma once

// DemoScenarioRunner: runs a synthetic DemoScenario through the REAL
// Application pipeline and reports exactly what happened. It is the single
// place the demo scenarios are executed, so the web demo and the tests exercise
// identical logic.
//
// WHAT IT DOES:
//   - resets the per-run counters/state (fresh-run semantics for a long-lived
//     process), exactly as the previous demo did,
//   - grants / withdraws the scenario's synthetic consent,
//   - drives SimulatorRuntime ticks; each AUTHORIZED observation is routed
//     through Application::process_event -> Pipeline -> correlation ->
//     AlertPolicy (no second event engine),
//   - captures the REAL delivered alerts (via Application's alert observer),
//   - optionally runs the SyntheticActivityDetector + FusionEngine detection
//     stage and feeds its Observations through the same pipeline.
//
// WHAT IT DOES NOT DO:
//   - It never persists (callers persist the returned delivered alerts), never
//     activates a physical sensor, and never inserts a fabricated alert: every
//     Alert in the result came out of the existing rule/policy processing.
//
// SIMULATOR-ONLY. Deterministic given the same spec.

#include "core/Alert.h"
#include "core/AnomalyMonitor.h"
#include "core/ImpactClassifier.h"
#include "core/DemoScenarios.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace homeguardian {

class Application;
class PersistenceAudit;

// Detection-stage summary (only populated when the scenario runs detection).
struct ScenarioDetectionResult {
    bool ran = false;
    std::string detector_id;
    std::string model_id;
    std::string provenance;      // "synthetic"
    size_t observations = 0;
    int detected = 0;
    int not_detected = 0;
    int inconclusive = 0;        // includes "unavailable"
    std::string fusion_verdict;  // detected / not_detected / inconclusive / unavailable
    double fusion_confidence = 0.0;
    size_t fusion_contributing = 0;
    std::string fusion_reason;
};

// Synthetic impact-classifier summary (H-01s). Synthetic demonstration only:
// NOT fall detection.
struct ScenarioImpactResult {
    bool ran = false;
    size_t samples = 0;
    size_t transients = 0;        // isolated high-intensity samples seen
    size_t confirmations = 0;     // completed transient-then-stillness patterns
    std::string last_phase;       // idle | transient | confirmed | expired
    std::string last_reason;      // bounded explanation from the classifier
    double last_value = 0.0;
    double transient_value = 0.0;          // last sample's pending transient (0 if none)
    double stillness_value = 0.0;          // last sample's stillness (0 if none)
    size_t samples_since_transient = 0;    // last sample's wait
    double confirmed_transient_value = 0.0;  // the pattern that was confirmed
    double confirmed_stillness_value = 0.0;
    size_t confirmed_samples_since_transient = 0;
    std::string confirmed_reason;            // why THAT sample confirmed the pattern
    size_t critical_alerts = 0;   // critical alerts DELIVERED this run
};

// Privacy-audit summary for the run (metadata only; never media).
struct ScenarioPrivacyResult {
    size_t entries = 0;
    size_t authorized = 0;
    size_t denied = 0;
    size_t acquired = 0;
    bool enforcement_ok = true;
    size_t capacity = 0;
};

struct ScenarioRunResult {
    std::string scenario_id;
    std::string title;
    std::string description;
    std::string expectation;

    bool capture_enabled = false;   // config kill switch at run time
    int cycles = 0;
    uint64_t media_authorized = 0;
    uint64_t denied_acquisitions = 0;  // gate denials recorded this run
    uint64_t events_processed = 0;
    uint64_t alerts_generated = 0;
    uint64_t alerts_delivered = 0;
    uint64_t alerts_suppressed = 0;

    // The REAL alerts delivered by the pipeline this run (rule/policy output).
    std::vector<Alert> delivered_alerts;

    ScenarioDetectionResult detection;
    AnomalyStatus anomaly;                      // L-12 statistical anomaly stage
    ScenarioImpactResult impact;                // H-01s synthetic impact stage
    std::vector<std::string> evidence_detectors; // detector ids that may vote
    ScenarioPrivacyResult privacy;

    std::string error;              // non-empty if the simulator could not run
};

// Run `spec` through the REAL Application pipeline. Does not persist and does
// not start a second event engine. Simulator-only.
ScenarioRunResult run_demo_scenario(Application& app, const DemoScenario& spec);

// Persist the REAL delivered alerts from a run to the durable audit store
// (metadata only). Returns the number of alerts written. Never fabricates an
// alert: it writes exactly what the pipeline delivered.
size_t persist_run_alerts(PersistenceAudit& audit, const ScenarioRunResult& r);

}  // namespace homeguardian
