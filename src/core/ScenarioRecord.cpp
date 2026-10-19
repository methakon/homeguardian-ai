#include "core/ScenarioRecord.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace homeguardian {

const char* scenario_class_to_string(ScenarioClass c) {
    switch (c) {
        case ScenarioClass::lighting:         return "lighting";
        case ScenarioClass::object_motion:    return "object_motion";
        case ScenarioClass::person_motion:    return "person_motion";
        case ScenarioClass::camera_vibration: return "camera_vibration";
        case ScenarioClass::occlusion:        return "occlusion";
        case ScenarioClass::occupancy:        return "occupancy";
        case ScenarioClass::ambient_none:     return "ambient_none";
        case ScenarioClass::unknown:          return "unknown";
    }
    return "unknown";
}

ScenarioClass scenario_class_from_string(const std::string& name) {
    for (ScenarioClass c : {ScenarioClass::lighting, ScenarioClass::object_motion,
                            ScenarioClass::person_motion, ScenarioClass::camera_vibration,
                            ScenarioClass::occlusion, ScenarioClass::occupancy,
                            ScenarioClass::ambient_none}) {
        if (name == scenario_class_to_string(c)) return c;
    }
    return ScenarioClass::unknown;
}

const char* occupancy_state_to_string(OccupancyState s) {
    switch (s) {
        case OccupancyState::empty:         return "empty";
        case OccupancyState::person_present: return "person_present";
        case OccupancyState::unknown:       return "unknown";
    }
    return "unknown";
}

OccupancyState occupancy_state_from_string(const std::string& name) {
    if (name == "empty") return OccupancyState::empty;
    if (name == "person_present") return OccupancyState::person_present;
    return OccupancyState::unknown;
}

size_t ScenarioRecord::motion_labelled() const {
    size_t total = 0;
    for (const auto& [character, count] : facts.character_counts) {
        if (character == "localized_motion" || character == "diffuse_motion") total += count;
    }
    return total;
}

ScenarioRecord evaluate_scenario(const std::string& scenario_id, ScenarioClass klass,
                                 const SceneAnnotation& annotation, const CaptureFacts& facts,
                                 const ScenarioPolicy& policy) {
    ScenarioRecord record;
    record.scenario_id = scenario_id;
    record.klass = klass;
    record.annotation = annotation;
    record.facts = facts;

    // ---- completeness: a run that did not finish is not a result ---------------------------------
    record.complete = true;
    if (!facts.run_finished) {
        record.complete = false;
        record.blockers.push_back("the capture did not finish: the run was interrupted");
    }
    if (facts.frames_requested > 0) {
        const double fraction =
            static_cast<double>(facts.frames_captured) / static_cast<double>(facts.frames_requested);
        if (fraction < policy.min_capture_fraction) {
            record.complete = false;
            std::ostringstream os;
            os << "incomplete capture: " << facts.frames_captured << " of " << facts.frames_requested
               << " frames (" << std::fixed << std::setprecision(0) << fraction * 100.0 << "%)";
            record.blockers.push_back(os.str());
        }
    } else {
        record.complete = false;
        record.blockers.push_back("the capture recorded no requested frame count");
    }
    if (facts.driver_dropped > 0) {
        // Dropped frames are reported rather than hidden: they bound how much of the scene was seen.
        std::ostringstream os;
        os << facts.driver_dropped << " frame(s) were dropped by the driver";
        record.blockers.push_back(os.str());
    }

    // ---- timestamps: the harness's clock, the pipeline's clock, and the order of events ----------
    record.timestamps_sane = true;
    if (facts.runner_ended < facts.runner_started) {
        record.timestamps_sane = false;
        record.blockers.push_back("the harness clock went backwards across the run");
    }
    if (facts.pipeline_timestamp.has_value()) {
        const auto drift = *facts.pipeline_timestamp > facts.runner_ended
                               ? *facts.pipeline_timestamp - facts.runner_ended
                               : facts.runner_ended - *facts.pipeline_timestamp;
        if (drift > policy.max_clock_drift) {
            record.timestamps_sane = false;
            std::ostringstream os;
            os << "the pipeline's timestamp differs from the harness by "
               << std::chrono::duration_cast<std::chrono::seconds>(drift).count() << "s, beyond the "
               << policy.max_clock_drift.count() << "s allowance";
            record.blockers.push_back(os.str());
        }
    } else {
        record.timestamps_sane = false;
        record.blockers.push_back("the log carried no pipeline timestamp: clock drift cannot be checked");
    }

    // ---- the annotation, which is the whole point ------------------------------------------------
    // An unconfirmed annotation is the blocker that matters most: without it, movement and the camera's
    // own gain response are indistinguishable, so the run cannot support any rate at all.
    const bool annotation_missing = policy.require_confirmed_annotation && !annotation.operator_confirmed;
    if (annotation_missing) {
        record.blockers.push_back(
            "no operator occupancy annotation: movement cannot be separated from camera gain artifacts, "
            "so this run cannot support a false-positive or false-negative claim");
    } else if (annotation.occupancy == OccupancyState::unknown) {
        // Confirmed-unknown is honest but weak: the run is reported, and it says what it cannot support.
        record.blockers.push_back(
            "the scene was confirmed as UNKNOWN: the run is reported, and it is not used to attribute "
            "movement");
    }

    // Attributable means: it finished, its clock is sane, and a human asserted the scene. Everything else
    // stays visible in `blockers` rather than being quietly averaged away.
    record.attributable = record.complete && record.timestamps_sane && !annotation_missing;
    return record;
}

std::string ScenarioRecord::summary() const {
    std::ostringstream os;
    os << scenario_class_to_string(klass) << " / " << scenario_id << ": ";
    os << "frames " << facts.frames_captured << "/" << facts.frames_requested << ", "
       << facts.observations << " observation(s), ";
    os << "characters {";
    bool first = true;
    for (const auto& [character, count] : facts.character_counts) {
        if (!first) os << ", ";
        os << character << "=" << count;
        first = false;
    }
    os << "} -> " << motion_labelled() << " motion-labelled";
    os << "; fusion: " << (facts.alert_raised ? "ALERT RAISED" : "no alert");
    if (facts.alert_raised && !facts.alert_rule.empty()) os << " (" << facts.alert_rule << ")";
    os << "; occupancy " << occupancy_state_to_string(annotation.occupancy)
       << (annotation.operator_confirmed ? " (confirmed)" : " (UNCONFIRMED)");
    if (!attributable) {
        os << "; NOT ATTRIBUTABLE";
        for (const auto& b : blockers) os << " [" << b << "]";
    }
    return os.str();
}

std::string scenario_report(const std::vector<ScenarioRecord>& records) {
    std::ostringstream os;
    os << "staged-scenario report: " << records.size() << " run(s)\n";
    // Grouped by class: a lighting run never contributes to a movement run's numbers.
    for (ScenarioClass klass : {ScenarioClass::ambient_none, ScenarioClass::lighting,
                                ScenarioClass::object_motion, ScenarioClass::person_motion,
                                ScenarioClass::camera_vibration, ScenarioClass::occlusion,
                                ScenarioClass::occupancy, ScenarioClass::unknown}) {
        std::vector<const ScenarioRecord*> group;
        for (const auto& r : records) {
            if (r.klass == klass) group.push_back(&r);
        }
        if (group.empty()) continue;
        size_t attributable = 0, alerts = 0, motion = 0, observations = 0;
        for (const auto* r : group) {
            if (r->attributable) ++attributable;
            if (r->facts.alert_raised) ++alerts;
            motion += r->motion_labelled();
            observations += r->facts.observations;
        }
        os << "\n== " << scenario_class_to_string(klass) << ": " << group.size() << " run(s), "
           << attributable << " attributable, " << observations << " observation(s), " << motion
           << " motion-labelled, " << alerts << " fusion alert(s)\n";
        for (const auto* r : group) os << "  " << r->summary() << "\n";
        // Per-class rates are stated only when every run in the class is attributable.
        if (attributable == group.size() && observations > 0) {
            os << "  rate: " << std::fixed << std::setprecision(3)
               << (static_cast<double>(motion) / static_cast<double>(observations))
               << " of observations labelled as movement, "
               << (static_cast<double>(alerts) / static_cast<double>(group.size()))
               << " of runs alerted\n";
        } else if (attributable != group.size()) {
            os << "  rate: WITHHELD - " << (group.size() - attributable)
               << " run(s) in this class are not attributable\n";
        }
    }
    return os.str();
}

}  // namespace homeguardian
