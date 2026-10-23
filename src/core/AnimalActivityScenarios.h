#pragma once

// AnimalActivityScenarios: a small, DETERMINISTIC catalogue of ANIMAL-ACTIVITY demonstration
// scenarios for the submission workstream.
//
// WHY THIS IS A SEPARATE CATALOGUE
//   `demo_scenarios()` is the demo/HTTP allowlist and its ordering is load-bearing (the first
//   entry is the backwards-compatible default). Adding entries there would change the demo
//   surface for every consumer. This catalogue reuses the SAME `DemoScenario` fixture struct and
//   the SAME runner (`run_demo_scenario`), so the scenarios execute through the real Application
//   pipeline - but it is opt-in by id and touches nothing shared.
//
// WHAT THESE SCENARIOS ARE
//   Inputs only: a bounded synthetic energy sequence per tick, the consent configuration, and the
//   number of ticks. There are NO pre-baked outcomes, so every resulting observation and alert is
//   produced by the existing detectors, fusion, correlation rules and AlertPolicy. Given the same
//   spec the result is identical every run.
//
// WHAT THEY ARE NOT
//   - Not species identification. No scenario asserts "this is a gecko/rodent/dog", and no code
//     path here could: the detector consumes bounded energy scalars, not audio or video.
//   - Not a calamity or seismic prediction. Nothing here can emit such a claim - the fusion frame
//     of discernment (DempsterShafer.h) has no element for it, so it is unrepresentable.
//   - Not real sensor data. Every scenario is SIMULATOR-ONLY and carries synthetic provenance,
//     which the runner reports in `ScenarioDetectionResult::provenance`.
//
// EVIDENCE GRADE (the discipline the precursor layer already records in its own headers):
//   these scenarios exercise Category A signals - measured activity-level change with no
//   interpretation attached - and the uncertainty handling around them. They deliberately do not
//   exercise the folklore (Category C) hypotheses, which have no detector here.

#include "core/DemoScenarios.h"

#include <string>
#include <string_view>
#include <vector>

namespace homeguardian {

// The animal-activity catalogue, in demonstration order.
const std::vector<DemoScenario>& animal_activity_scenarios();

// Look up by id. nullptr for an unknown id - the caller must treat that as a rejection.
const DemoScenario* find_animal_activity_scenario(std::string_view id);

// The provenance label every scenario in this catalogue carries. Stated as a value so the
// documentation, the tests and the runner cannot drift apart on it.
inline constexpr const char* kAnimalActivityProvenance = "synthetic";

}  // namespace homeguardian
