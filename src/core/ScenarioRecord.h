#pragma once

// ScenarioRecord: what a staged physical test produced, and whether it may be used as evidence at all.
//
// WHY THIS EXISTS
//   The unresolved question in this work is not "did the detector fire" but "did anybody move". The same
//   controlled lighting experiment produced 19 motion-labelled observations once and 61 another time, and
//   the difference could not be attributed, because nothing recorded whether a person was in the scene.
//   A run without a scene annotation is therefore not evidence about false positives, however clean its
//   numbers look - and a harness that quietly assumes "probably empty" would manufacture exactly the
//   conclusion this project has refused to manufacture.
//
// WHAT IT DOES
//   Keeps the operator's annotation, the capture's own facts, and the fusion outcome SEPARATE, then states
//   whether the combination is attributable and names every blocker. Frame characters and fusion alerts
//   are never merged, because a scene can be full of labelled movement and still produce no alert - and
//   that is a result, not a contradiction.
//
// WHAT IT DOES NOT DO: it invents nothing. A missing annotation stays missing, an incomplete capture stays
// incomplete, and a class of scenario is reported only against its own runs.

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace homeguardian {

// Independently labelled scenario classes. A lighting run and a movement run are never pooled.
enum class ScenarioClass {
    lighting,           // room light switched on/off, or a controlled illumination change
    object_motion,      // a small object moved across the field by hand
    person_motion,      // a person crossing the field, with consent
    camera_vibration,   // the mount was disturbed
    occlusion,          // something temporarily covered the view
    occupancy,          // the scene's occupancy changed deliberately (someone sat down, left the room)
    ambient_none,       // no deliberate stimulus: the baseline run
    unknown,
};

const char* scenario_class_to_string(ScenarioClass c);
ScenarioClass scenario_class_from_string(const std::string& name);

enum class OccupancyState { empty, person_present, unknown };
const char* occupancy_state_to_string(OccupancyState s);
OccupancyState occupancy_state_from_string(const std::string& name);

// The operator's assertion about the scene. `confirmed` is separate from the state on purpose: "unknown"
// that nobody asserted must not be readable as "empty".
struct SceneAnnotation {
    OccupancyState occupancy = OccupancyState::unknown;
    bool operator_confirmed = false;
    std::string deliberate_change;   // what the operator actually did, in their words
    std::string note;
};

// What the capture actually produced, measured - not assumed.
struct CaptureFacts {
    size_t frames_requested = 0;
    size_t frames_captured = 0;
    size_t driver_dropped = 0;
    bool run_finished = false;
    size_t observations = 0;
    std::map<std::string, size_t> character_counts{};   // frame-level labels
    double peak_motion = 0.0;
    std::string motion_verdict;                          // the scalar rule's own verdict
    bool alert_raised = false;                           // the FUSION outcome, kept separate
    double alert_confidence = 0.0;
    std::string alert_rule;
    std::chrono::system_clock::time_point runner_started{};
    std::chrono::system_clock::time_point runner_ended{};
    std::optional<std::chrono::system_clock::time_point> pipeline_timestamp{};
};

struct ScenarioPolicy {
    double min_capture_fraction = 0.95;         // frames captured / requested
    std::chrono::seconds max_clock_drift{5};    // the pipeline's stamp vs the harness's own end time
    bool require_confirmed_annotation = true;
};

struct ScenarioRecord {
    std::string scenario_id;
    ScenarioClass klass = ScenarioClass::unknown;
    SceneAnnotation annotation{};
    CaptureFacts facts{};
    bool complete = false;
    bool timestamps_sane = false;
    bool attributable = false;              // may this run be used as evidence?
    std::vector<std::string> blockers{};    // why not, in the operator's terms
    size_t motion_labelled() const;         // frames labelled as any kind of movement
    std::string summary() const;            // one line, characters AND alert reported separately
};

ScenarioRecord evaluate_scenario(const std::string& scenario_id, ScenarioClass klass,
                                 const SceneAnnotation& annotation, const CaptureFacts& facts,
                                 const ScenarioPolicy& policy = {});

// The report groups by scenario class, never pooling classes, and prints the unattributable runs with
// their blockers rather than dropping them.
std::string scenario_report(const std::vector<ScenarioRecord>& records);

}  // namespace homeguardian
