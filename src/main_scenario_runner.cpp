// hg_scenario_runner: the operator-guided harness for the staged physical scenarios.
//
// WHAT IT IS FOR
//   Seven staged scenarios remain unrun because each needs a person in the room. This drives them one at a
//   time: it prints what the operator should do, runs the existing capture tool unchanged, records exactly
//   what the run produced (frame characters AND the fusion outcome, kept separate), and attaches the
//   operator's own statement about the scene.
//
// WHAT IT REFUSES TO DO
//   It never assumes an occupancy. Without `--occupancy` plus `--confirmed`, every record is marked NOT
//   ATTRIBUTABLE, because a run whose scene nobody described cannot tell a person moving from the camera's
//   own gain response - and that is the open question this work has refused to answer by guessing.
//
// WHAT IT DOES NOT DO: it stores no frame and no audio. The capture tool reduces frames to scalars and
// retains nothing; this only reads the scalars it printed.
//
// USAGE
//   hg_scenario_runner --instructions
//   hg_scenario_runner --occupancy empty --confirmed --deliberate "light on at t=8s, off at t=20s"
//   hg_scenario_runner --only light_on_off --interactive
//   hg_scenario_runner --dry-run

#include "core/ScenarioRecord.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

struct PlannedScenario {
    const char* id;
    ScenarioClass klass;
    uint32_t seconds;
    const char* instruction;
};

// The seven outstanding scenarios, each independently labelled so no class is ever pooled with another.
const std::vector<PlannedScenario>& default_plan() {
    static const std::vector<PlannedScenario> plan = {
        {"ambient_baseline", ScenarioClass::ambient_none, 20,
         "Do nothing. Stand out of frame. This is the baseline the other runs are compared against."},
        {"light_on_off", ScenarioClass::lighting, 30,
         "Switch the room light ON, wait 10 seconds, switch it OFF, wait 10 seconds. Do not move."},
        {"object_cross", ScenarioClass::object_motion, 25,
         "Move a small object (a mug, a phone) slowly across the field of view by hand, twice."},
        {"person_cross", ScenarioClass::person_motion, 25,
         "Walk through the scene once at normal pace, entering and leaving the frame. You consent to "
         "being measured as scalars only; no frame is stored."},
        {"shadow_move", ScenarioClass::object_motion, 25,
         "Wave a hand close to a wall so the shadow moves across the field. Do not enter the frame."},
        {"camera_vibration", ScenarioClass::camera_vibration, 20,
         "Tap the camera's mount gently twice, then do not touch it for 15 seconds."},
        {"curtain_air", ScenarioClass::object_motion, 25,
         "Move a curtain, cloth or plant near the field of view, or blow air across it, repeatedly."},
    };
    return plan;
}

struct Options {
    bool interactive = false;
    bool dry_run = false;
    bool instructions = false;
    bool confirmed = false;
    OccupancyState occupancy = OccupancyState::unknown;
    std::string deliberate;
    std::string only;
    std::string bridge = "./build/hg_camera_bridge";
    std::string out_dir = ".";
    uint32_t frames = 600;
    uint32_t cadence = 5;
    uint32_t seconds_override = 0;
};

// A run stamp, so a second run of the same scenario cannot overwrite the first. The first version named
// the log after the scenario alone, and a repeat run silently destroyed the earlier evidence - the exact
// data loss this harness exists to prevent.
std::string run_stamp(std::chrono::system_clock::time_point t) {
    const std::time_t as_time = std::chrono::system_clock::to_time_t(t);
    std::tm utc{};
    gmtime_r(&as_time, &utc);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

// Read a number that follows a substring on any line.
bool number_after(const std::string& log, const std::string& key, double* out) {
    const size_t at = log.find(key);
    if (at == std::string::npos) return false;
    // Skip everything up to the number: the first version only skipped spaces, colons and equals, so a
    // JSON value like "observation_timestamp": 123 - with its closing quote - parsed as nothing, and the
    // timestamp blocker fired on a log that carried the timestamp all along.
    const char* p = log.c_str() + at + key.size();
    while (*p != '\0' && !std::isdigit(static_cast<unsigned char>(*p)) && *p != '-' && *p != '+') ++p;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

bool pair_after(const std::string& log, const std::string& key, size_t* first, size_t* second) {
    const size_t at = log.find(key);
    if (at == std::string::npos) return false;
    size_t a = 0, b = 0;
    if (std::sscanf(log.c_str() + at + key.size(), " : %zu of %zu", &a, &b) != 2) return false;
    *first = a;
    *second = b;
    return true;
}

std::string text_after(const std::string& log, const std::string& key) {
    const size_t at = log.find(key);
    if (at == std::string::npos) return {};
    const size_t colon = log.find(':', at);
    if (colon == std::string::npos) return {};
    size_t end = log.find('\n', colon);
    if (end == std::string::npos) end = log.size();
    std::string value = log.substr(colon + 1, end - colon - 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

CaptureFacts parse_capture_log(const std::string& path, bool run_finished,
                               std::chrono::system_clock::time_point started,
                               std::chrono::system_clock::time_point ended) {
    CaptureFacts facts;
    facts.run_finished = run_finished;
    facts.runner_started = started;
    facts.runner_ended = ended;

    std::ifstream in(path);
    if (!in) return facts;
    const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    size_t captured = 0, requested = 0;
    if (pair_after(log, "frames captured", &captured, &requested)) {
        facts.frames_captured = captured;
        facts.frames_requested = requested;
    }
    double value = 0.0;
    if (number_after(log, "driver-dropped frames", &value)) {
        facts.driver_dropped = static_cast<size_t>(value);
    }
    if (number_after(log, "observations built", &value)) facts.observations = static_cast<size_t>(value);
    if (number_after(log, "peak motion", &value)) facts.peak_motion = value;
    if (number_after(log, "observation_timestamp", &value)) {
        facts.pipeline_timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(
            static_cast<long long>(value)));
    }
    facts.motion_verdict = text_after(log, "MOTION-VERDICT");

    // The fusion outcome, read from the rule evaluation block - and kept separate from the characters.
    const size_t rule_at = log.find("detection-fusion rule");
    if (rule_at != std::string::npos) {
        const size_t line_end = log.find('\n', rule_at);
        const std::string line = log.substr(rule_at, line_end - rule_at);
        facts.alert_raised = line.find("ALERT RAISED") != std::string::npos;
    }
    if (facts.alert_raised) {
        facts.alert_rule = text_after(log.substr(rule_at), "rule                     :");
        if (facts.alert_rule.empty()) facts.alert_rule = text_after(log, "rule");
        if (number_after(log.substr(rule_at), "confidence               :", &value)) {
            facts.alert_confidence = value;
        }
    }

    // Frame characters, one per observation line: the token after the ratio column.
    size_t observations_seen = 0;
    std::istringstream stream(log);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t ratio_at = line.find("ratio=");
        if (line.find("seq=") == std::string::npos || ratio_at == std::string::npos) continue;
        const char* p = line.c_str() + ratio_at + 6;
        char* end = nullptr;
        std::strtod(p, &end);
        if (end == p) continue;
        std::string rest(end);
        std::istringstream tokens(rest);
        std::string character;
        if (!(tokens >> character)) continue;
        if (character.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) continue;
        ++facts.character_counts[character];
        ++observations_seen;
    }
    if (facts.observations == 0) facts.observations = observations_seen;
    return facts;
}

int run_bridge(const Options& options, const std::string& log_path) {
    const std::vector<std::string> argv{options.bridge,
                                        "--capture",
                                        "--confirm",
                                        "--frames",
                                        std::to_string(options.frames),
                                        "--cadence",
                                        std::to_string(options.cadence),
                                        "--log",
                                        log_path};
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(args[0], args.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void print_instructions() {
    std::printf("STAGED PHYSICAL SCENARIOS - operator instructions\n");
    std::printf("Area: the designated non-private test area only. Occupancy: only people who consent.\n");
    std::printf("Nothing is recorded: captures reduce frames to scalars and retain no frame.\n\n");
    int i = 1;
    for (const auto& s : default_plan()) {
        std::printf("%d. %-18s (%s, ~%us)\n     %s\n\n", i++, s.id,
                    scenario_class_to_string(s.klass), s.seconds, s.instruction);
    }
    std::printf("Then run one scenario at a time, e.g.:\n");
    std::printf("  hg_scenario_runner --only light_on_off --occupancy empty --confirmed \\\n");
    std::printf("      --deliberate \"light on at t=8s, off at t=20s\"\n");
    std::printf("An unannotated run is reported and marked NOT ATTRIBUTABLE: it cannot separate movement\n");
    std::printf("from the camera's own gain response, so it supports no rate at all.\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](std::string& target) {
            if (i + 1 < argc) target = argv[++i];
        };
        if (arg == "--interactive") options.interactive = true;
        else if (arg == "--dry-run") options.dry_run = true;
        else if (arg == "--instructions") options.instructions = true;
        else if (arg == "--confirmed") options.confirmed = true;
        else if (arg == "--occupancy") { std::string v; next(v); options.occupancy = occupancy_state_from_string(v); }
        else if (arg == "--deliberate") next(options.deliberate);
        else if (arg == "--only") next(options.only);
        else if (arg == "--bridge") next(options.bridge);
        else if (arg == "--out") next(options.out_dir);
        else if (arg == "--frames") { std::string v; next(v); options.frames = std::stoul(v); }
        else if (arg == "--cadence") { std::string v; next(v); options.cadence = std::stoul(v); }
        else if (arg == "--help") { print_instructions(); return 0; }
    }
    if (options.instructions) {
        print_instructions();
        return 0;
    }

    std::vector<ScenarioRecord> records;
    for (const auto& planned : default_plan()) {
        if (!options.only.empty() && options.only != planned.id) continue;

        SceneAnnotation annotation;
        annotation.occupancy = options.occupancy;
        annotation.operator_confirmed = options.confirmed;
        annotation.deliberate_change = options.deliberate;

        if (options.interactive) {
            std::printf("\n--- %s (%s) ---\n%s\nPress Enter to start the capture (it takes ~%us)...\n",
                        planned.id, scenario_class_to_string(planned.klass), planned.instruction,
                        planned.seconds);
            std::fflush(stdout);
            (void)std::getchar();
        } else {
            std::printf("--- %s (%s): %s\n", planned.id, scenario_class_to_string(planned.klass),
                        planned.instruction);
        }

        if (options.dry_run) {
            std::printf("    [dry run] would capture %u frames at cadence %u\n", options.frames,
                        options.cadence);
            records.push_back(evaluate_scenario(planned.id, planned.klass, annotation, CaptureFacts{}));
            continue;
        }

        const std::string log_path =
            options.out_dir + "/scenario-" + planned.id + "-" + run_stamp(std::chrono::system_clock::now()) + ".log";
        const auto started = std::chrono::system_clock::now();
        const int rc = run_bridge(options, log_path);
        const auto ended = std::chrono::system_clock::now();

        const CaptureFacts facts = parse_capture_log(log_path, rc == 0, started, ended);
        ScenarioRecord record = evaluate_scenario(planned.id, planned.klass, annotation, facts);
        std::printf("    %s\n", record.summary().c_str());
        records.push_back(record);
    }

    if (records.empty()) {
        std::printf("no scenario matched\n");
        return 1;
    }

    std::printf("\n%s", scenario_report(records).c_str());

    // Records as JSON: the annotation travels with the numbers, so a later reader cannot lose it.
    std::ostringstream json;
    json << "{\n  \"scenarios\": [\n";
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        json << "    {\"scenario_id\": \"" << r.scenario_id << "\", \"class\": \""
             << scenario_class_to_string(r.klass) << "\", \"occupancy\": \""
             << occupancy_state_to_string(r.annotation.occupancy) << "\", \"annotation_confirmed\": "
             << (r.annotation.operator_confirmed ? "true" : "false") << ", \"deliberate_change\": \""
             << r.annotation.deliberate_change << "\", \"frames_captured\": " << r.facts.frames_captured
             << ", \"frames_requested\": " << r.facts.frames_requested
             << ", \"driver_dropped\": " << r.facts.driver_dropped
             << ", \"observations\": " << r.facts.observations
             << ", \"motion_labelled\": " << r.motion_labelled()
             << ", \"alert_raised\": " << (r.facts.alert_raised ? "true" : "false")
             << ", \"alert_rule\": \"" << r.facts.alert_rule << "\""
             << ", \"attributable\": " << (r.attributable ? "true" : "false") << ", \"blockers\": [";
        for (size_t b = 0; b < r.blockers.size(); ++b) {
            json << (b > 0 ? ", " : "") << "\"" << r.blockers[b] << "\"";
        }
        json << "]}" << (i + 1 < records.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    const std::string json_path =
        options.out_dir + "/scenario-records-" + run_stamp(std::chrono::system_clock::now()) + ".json";
    std::ofstream out(json_path);
    out << json.str();
    out.close();
    std::printf("\nrecords written to %s\n", json_path.c_str());

    size_t unattributable = 0;
    for (const auto& r : records) {
        if (!r.attributable) ++unattributable;
    }
    if (unattributable > 0) {
        std::printf("%zu of %zu run(s) are NOT ATTRIBUTABLE; no rate is claimed from them.\n",
                    unattributable, records.size());
    }
    return 0;
}
