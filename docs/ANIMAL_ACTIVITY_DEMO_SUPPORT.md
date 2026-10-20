# Animal-activity demonstration support

**Date:** 2026-10-18
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Audience:** the submission workstream

**Isolation:** this document and its scenarios touch no other workstream. The animal-activity
catalogue is a separate, opt-in fixture file; the shared demo allowlist (`demo_scenarios()`) is
unchanged, so the demo/HTTP surface is byte-for-byte what it was.

## 1. Scope, in one paragraph

The repository already contains a **precursor/animal-activity layer** built in C++. This document
inventories what is there, adds a small set of **deterministic, reproducible demonstration
scenarios** that run through the real pipeline, and states exactly what may and may not be claimed
about them. It adds **no** new detection capability, no classifier, no camera, and no prediction.

## 2. Inventory (assignment item 6)

### 2.1 Event and observation model

| Piece | Where | What it gives |
|---|---|---|
| `EventType` = observation / inference / alert | `src/core/Event.h` | the three event kinds the pipeline processes |
| `Severity` = info / warning / critical | `src/core/Event.h` | bounded severity vocabulary |
| `Event` (+ `get_payload()`, correlation id, evidence ref) | `src/core/Event.h` | the durable, validated carrier; payload is bounded JSON |
| `Provenance` (synthetic / real) | `src/core/Observation.h` | **is this from a simulator/fixture or a physical sensor?** |
| `DetectorStatus` = detected / not_detected / unavailable / inconclusive | `src/core/Observation.h` | "we could not run the detector" is never confused with "it ran and found nothing" |
| `Observation` | `src/core/Observation.h` | thin wrapper that converts to a standard `Event`; carries bounded features only, never raw media |

### 2.2 Detection categories

| Detector | Where | Signal | Grade |
|---|---|---|---|
| `ActivityChangeDetector` | `src/core/ActivityDetectors.h` | sustained activity level vs a learned slow baseline, with a confirmation window | A (measured quantity, no interpretation) |
| `VectorDriftDetector` | `src/core/ActivityDetectors.h` | cosine drift of an embedding vector from a quiet baseline — needs **no labels** | A |
| `SyntheticActivityDetector` | used by the scenario runner | deterministic synthetic energy sequence driving the above | fixture |
| `SyntheticImpactRule` / `ImpactClassifier` | `src/core/` | transient-then-stillness pattern | fixture, and explicitly **not** fall detection |

Deferred with the reason recorded in the code: YAMNet log-mel + 1024-d embeddings (needs ONNX
Runtime and weights), YOLOv8 and Farneback optical flow (need OpenCV). Nothing is stubbed in their
place.

### 2.3 Uncertainty handling

| Piece | Where | Rule |
|---|---|---|
| `DempsterShafer` fusion | `src/core/DempsterShafer.h` | frame Θ = { routine, activity_change, environmental_anomaly }; conflict mass **K** is computed and returned |
| The conflict brake | same | **K → 1 means the sensors contradict: suppress, do not average** |
| `PrecursorLabel` | `src/core/PrecursorDetection.h` | `activity_change_detected` · `audio_activity_deviation` · `possible_animal_activity` (**low-confidence hypothesis**) · `environmental_correlation` · `insufficient_evidence` |
| Explicit unrepresentability | same | the frame has no `earthquake_imminent` / `flood_will_occur` / species element, so such a claim cannot be constructed — not merely discouraged |

Note the frame is deliberately **not** the source skill's `{Routine, SeismicPrecursor, WeatherAnomaly}`:
the skill's own correction ledger records that animal seismology is debunked, so a life-safety
hypothesis has no place in an alerting frame here.

### 2.4 Temporal correlation

| Piece | Where | Behaviour |
|---|---|---|
| `TimeWindowCorrelation` | `src/core/TimeWindowCorrelation.h` | associates observations inside a bounded time window |
| `CorrelationRule` | `src/core/CorrelationRule.h` | the rule shape the pipeline evaluates |
| `EnvironmentalCorrelation` | `src/core/EnvironmentalCorrelation.h` | metric allowlist, plausibility ranges, bounded strings, authorization context; a sample that is not authorized is **not ingested**; output records the **lag** and the pairing used |

### 2.5 Alert behaviour

| Piece | Where | Behaviour |
|---|---|---|
| `Alert` | `src/core/Alert.h` | id, severity, message, contributing event ids, confidence, rule name |
| `AlertPolicy` | `src/core/AlertPolicy.h` | raises/suppresses; repeats are suppressed so one condition is not a stream of alerts |

## 3. Demonstration scenarios (assignment item 7)

New, opt-in catalogue: `src/core/AnimalActivityScenarios.h/.cpp`. Same fixture struct and same
runner as the existing demo scenarios, so each one executes through the **real pipeline**. Inputs
only — no pre-baked outcomes.

| Scenario id | Input (synthetic energy per tick) | Documented outcome |
|---|---|---|
| `animal-activity-quiet` | `0.05 0.04 0.06 0.05 0.05` | not_detected throughout, **no alert** — a quiet window stays quiet |
| `animal-activity-sustained` | `0.95 0.90 0.88 0.92 0.85` | detected every tick, fusion agrees at 1.00, **alert delivered** |
| `animal-activity-single-spike` | `0.05 0.05 0.97 0.05 0.05` | **no confirmed change, no alert** — the confirmation window rejects one anomalous sample |
| `animal-activity-inconclusive` | `0.52 0.49 0.51` | observations **inconclusive**, fusion refuses a verdict, no alert |
| `animal-activity-no-consent` | the sustained sequence, consent withheld | **nothing authorized, nothing processed, no alert** |

Run them: `./build/homeguardian_tests '[animal]'`.

## 4. The chain, end to end (assignment item 8)

```
synthetic energy per tick            (fixture input, declared in the scenario)
        |
SyntheticActivityDetector + ActivityChangeDetector / VectorDriftDetector
        |                            bounded features, confirmation windows
FusionEngine over DempsterShafer     verdict + confidence + contributing detectors + K
        |
Observation{ provenance=synthetic, detector_id, status, features }   (bounded; never raw media)
        |
Observation::to_event() -> Event(type=observation)
        |
Application::process_event -> Pipeline -> TimeWindowCorrelation / CorrelationRule
        |                            (the existing rule engine; no second event engine)
AlertPolicy                          raises / suppresses
        |
Alert delivered by the pipeline      (captured by the runner; the tests assert THESE alerts)
        |
read-only MCP tool  get_alert_summary  ->  { alert_count, highest_severity, alert_ids, read_only }
```

Every arrow is existing code. The new material is the fixture catalogue and the tests that assert
each arrow's output — including that the MCP tool's answer is built from the run's **real**
delivered alerts, so the demo cannot show an alert the pipeline did not produce.

## 5. Evidence grading, and the folklore boundary (assignment item 9)

| Claim about the demo | Status |
|---|---|
| Detects a sustained change in a bounded activity-level signal | **Implemented and tested** (grade A: a measured quantity, no interpretation) |
| Correlates an authorized environmental sample with a detected change in the same window | **Implemented** (grade B: plausible correlation, reported as correlation at `info`, never an alert, never a prediction) |
| Produces `possible_animal_activity` as an explicit **low-confidence hypothesis** | **Implemented** (the label itself is the claim's ceiling) |
| Identifies a species, or "this is a gecko / rodent / dog" | **NOT implemented.** No classifier, no dataset, no code path |
| Predicts an earthquake, flood or other calamity from animal behaviour | **NOT implemented, and unrepresentable** in the alerting frame |
| Uses folklore (*Khonar Bachan*) as a detector | **NOT implemented.** The folk corpus is documented as a hypothesis source; the ant/seismic claim is debunked (ants react to shaking above M2.0, they do not predict it), and the gecko-call reading is superstition, usable at most as a generic ecosystem-activity proxy |
| Reads a live camera or microphone | **NOT implemented here.** Every scenario is simulator-only with synthetic provenance |

Language for submission material: *"demonstrates bounded activity-change detection with explicit
uncertainty handling and correlation, on synthetic inputs"* — **not** "predicts earthquakes",
"recognises animals", or "monitors your home in real time".

## 6. Provenance labels (assignment item 10)

- Every scenario observation carries **`synthetic`**, asserted by the tests, and the runner reports
  it in `ScenarioDetectionResult::provenance`.
- The label is derived from the detector/adapter class, not passed in, so it cannot be mislabelled
  by a caller.
- Moving to `real` would require a physical sensor, an authorized adapter, and a consent record —
  none of which exists for animal activity today, so no `real` animal observation is claimed.

## 7. Test evidence

| Suite | Result |
|---|---|
| `[animal]` — catalogue well-formedness, the five scenarios' outputs, the MCP-visible summary, and determinism (the same input reproduces the same outcome) | **7 cases, 161 assertions, all passing** |
| Full suite | **7319 assertions / 562 cases, all passing** (no regressions) |

Revisions: `src/core/AnimalActivityScenarios.h/.cpp`, `tests/core/test_animal_activity_scenarios.cpp`,
plus this document — see the branch's commit for exact SHAs.

## 8. What the submission may reuse

- The scenario ids and their documented outcomes as a **reproducible** demo (deterministic, no
  hardware, no network).
- The grading table in §5, verbatim, as the honest capability claim.
- The chain diagram in §4 as the architecture description.
