#include "catch2/catch.hpp"

#include "core/AlertPolicy.h"
#include "core/DetectionFusionRule.h"
#include "core/EnvironmentalCorrelation.h"
#include "core/Pipeline.h"
#include "core/PrecursorDetection.h"
#include "core/SecurityObservation.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Skills-integration: the new precursor and security observations must flow
// through the EXISTING pipeline, and must not change what the existing product
// does by default.
//
// The load-bearing invariants:
//   1. The new detector ids are ADVISORY by default: they are validated,
//      recorded and auditable, but they are not fusion votes unless an operator
//      explicitly allowlists them (the existing L-12 mechanism).
//   2. When they ARE allowlisted they can alert - and the severity stays info or
//      warning. No sensor report or precursor result can produce Severity::critical;
//      critical remains reserved for the explicitly designated synthetic impact
//      demonstration.
//   3. An observation keeps its provenance, detector identity and authorization
//      context across the Event round trip.
// ---------------------------------------------------------------------------

namespace {

std::vector<int16_t> pcm(size_t frames, int16_t amplitude) {
    std::vector<int16_t> out(frames, amplitude);
    for (size_t i = 0; i < frames; ++i) {
        out[i] = (i % 2 == 0) ? amplitude : static_cast<int16_t>(-amplitude);
    }
    return out;
}

// A sustained motion + audio + vector regime change: enough for the precursor
// suite to reach its strongest (still hypothetical) label.
std::vector<Event> precursor_events(const std::string& source) {
    PrecursorSuite suite;
    const auto now = std::chrono::system_clock::now();
    for (int i = 0; i < 12; ++i) {
        suite.observe_motion(0.05, now);
        auto samples = pcm(1200, 200);
        suite.observe_audio(samples.data(), 1200, 1, 16000, now);
        suite.observe_vector({1.0, 0.0, 0.0}, now);
    }
    for (int i = 0; i < 6; ++i) {
        suite.observe_motion(0.45, now);
        auto samples = pcm(1200, 12000);
        suite.observe_audio(samples.data(), 1200, 1, 16000, now);
        suite.observe_vector({0.0, 1.0, 0.0}, now);
    }
    std::vector<Event> out;
    for (const Observation& o : suite.observations(source, Provenance::synthetic)) {
        out.push_back(o.to_event());
    }
    return out;
}

SecurityObservation sensor(SecuritySourceType type, const std::string& state,
                           std::optional<double> confidence = std::nullopt) {
    const auto now = std::chrono::system_clock::now();
    SecurityObservation o;
    o.device_id = "node:hg-0123456789abcdef";
    o.source = "node";
    o.source_type = type;
    o.acquired_at = now;
    o.ingested_at = now;
    o.provenance = Provenance::real;
    o.state = state;
    o.consent_subject = "node:hg-0123456789abcdef";
    o.consent_purpose = "security_monitor";
    o.authorized = true;
    o.confidence = confidence;
    o.features = nlohmann::json::object();
    return o;
}

std::vector<Event> sensor_events() {
    std::vector<Event> out;
    out.push_back(sensor(SecuritySourceType::motion, "on", 0.9).to_observation().to_event());
    out.push_back(sensor(SecuritySourceType::contact, "open", 0.9).to_observation().to_event());
    out.push_back(sensor(SecuritySourceType::presence, "present", 0.9).to_observation().to_event());
    out.push_back(sensor(SecuritySourceType::tamper, "tamper", 0.9).to_observation().to_event());
    out.push_back(
        sensor(SecuritySourceType::device_availability, "offline", 0.9).to_observation().to_event());
    out.push_back(sensor(SecuritySourceType::camera_availability, "unavailable", 0.9)
                      .to_observation()
                      .to_event());
    out.push_back(sensor(SecuritySourceType::microphone_availability, "unavailable", 0.9)
                      .to_observation()
                      .to_event());
    out.push_back(sensor(SecuritySourceType::environmental_threshold, "crossed", 0.9)
                      .to_observation()
                      .to_event());
    return out;
}

std::shared_ptr<Pipeline> make_pipeline(std::shared_ptr<DetectionFusionRule> rule) {
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(rule);
    return std::make_shared<Pipeline>(rules);
}

}  // namespace

TEST_CASE("the default evidence allowlist is unchanged, so existing behaviour is preserved",
          "[integration][skills]") {
    DetectionFusionRule rule("detection-fusion");
    // The pre-existing detector still votes.
    REQUIRE(rule.accepts_detector("activity-energy"));
    // Everything this milestone adds is advisory until explicitly allowlisted.
    REQUIRE_FALSE(rule.accepts_detector("motion-activity"));
    REQUIRE_FALSE(rule.accepts_detector("audio-activity"));
    REQUIRE_FALSE(rule.accepts_detector("audio-activity-deviation"));
    REQUIRE_FALSE(rule.accepts_detector("vector-drift"));
    REQUIRE_FALSE(rule.accepts_detector("precursor-fusion"));
    REQUIRE_FALSE(rule.accepts_detector("environmental-correlation"));
    REQUIRE_FALSE(rule.accepts_detector("sensor.motion"));
    REQUIRE_FALSE(rule.accepts_detector("sensor.tamper"));
    REQUIRE_FALSE(rule.accepts_detector("sensor.device_availability"));
    // And the L-12 invariant survives: the statistical anomaly monitor stays
    // advisory too.
    REQUIRE_FALSE(rule.accepts_detector("activity-anomaly"));
}

TEST_CASE("precursor and sensor observations are validated, recorded and advisory by default",
          "[integration][skills]") {
    auto rule = std::make_shared<DetectionFusionRule>("detection-fusion");
    auto pipeline = make_pipeline(rule);

    std::vector<Event> events = precursor_events("node:hg-0123456789abcdef");
    const std::vector<Event> sensors = sensor_events();
    events.insert(events.end(), sensors.begin(), sensors.end());

    // An environmental correlation observation too (Category B: never an alert).
    CorrelationWindow window;
    CorrelationWindow::ChangeRecord change;
    change.source = "node:hg-0123456789abcdef";
    change.kind = "activity_change";
    change.at = std::chrono::system_clock::now();
    window.add_change(change);
    EnvironmentalSample sample;
    sample.source = "sensor.living_room_temperature";
    sample.metric = "temperature";
    sample.value = 21.4;
    sample.unit = "C";
    sample.observed_at = std::chrono::system_clock::now();
    sample.provenance = Provenance::real;
    sample.authorization = "subject=node:hg-1;purpose=environment_monitor;decision=granted";
    window.add_sample(sample);
    const CorrelationWindow::Result correlation = window.correlate(std::chrono::system_clock::now());
    REQUIRE(correlation.correlated);
    events.push_back(window.to_observation(correlation, "corr-1", "hub").to_event());

    size_t alerts = 0;
    for (const Event& e : events) {
        const auto alert = pipeline->process(e);
        if (alert.has_value()) {
            ++alerts;
            REQUIRE(alert->severity != Severity::critical);
        }
    }

    REQUIRE(alerts == 0);  // advisory: no new alert path by default
    REQUIRE(events.size() > 12);
    // They are still fully processed and auditable.
    const auto history = pipeline->get_recent_events(std::chrono::milliseconds(60000));
    REQUIRE(history.size() == events.size());
}

TEST_CASE("allowlisting a precursor detector opts into alerting, with conservative severity",
          "[integration][skills]") {
    auto rule = std::make_shared<DetectionFusionRule>("detection-fusion");
    rule->set_evidence_detector_ids({"motion-activity", "audio-activity"});
    auto pipeline = make_pipeline(rule);

    size_t delivered = 0;
    for (const Event& e : precursor_events("node:hg-0123456789abcdef")) {
        const auto alert = pipeline->process(e);
        if (!alert.has_value()) continue;
        ++delivered;
        REQUIRE(alert->rule_name == "detection-fusion");
        REQUIRE(alert->severity != Severity::critical);
        REQUIRE((alert->severity == Severity::info || alert->severity == Severity::warning));
    }
    REQUIRE(delivered >= 1);
}

TEST_CASE("no security sensor report can raise a critical alert, even when everything is allowlisted",
          "[integration][skills]") {
    auto rule = std::make_shared<DetectionFusionRule>("detection-fusion");
    rule->set_evidence_detector_ids({});  // accept EVERY detector: the worst case
    auto pipeline = make_pipeline(rule);

    for (const Event& e : sensor_events()) {
        const auto alert = pipeline->process(e);
        if (!alert.has_value()) continue;
        REQUIRE(alert->severity != Severity::critical);
        REQUIRE((alert->severity == Severity::info || alert->severity == Severity::warning));
    }

    // The same guarantee for the DST fusion result, whose strongest label is the
    // explicitly-hypothetical possible_animal_activity.
    for (const Event& e : precursor_events("node:hg-0123456789abcdef")) {
        const auto alert = pipeline->process(e);
        if (!alert.has_value()) continue;
        REQUIRE(alert->severity != Severity::critical);
    }
}

TEST_CASE("observations keep provenance, detector identity and consent context across the pipeline",
          "[integration][skills]") {
    auto pipeline = make_pipeline(std::make_shared<DetectionFusionRule>("detection-fusion"));

    SecurityObservation original = sensor(SecuritySourceType::contact, "open");
    const Event e = original.to_observation().to_event();
    pipeline->process(e);

    const auto history = pipeline->get_recent_events(std::chrono::milliseconds(60000));
    REQUIRE(history.size() == 1);
    const auto back = Observation::from_event(history[0]);
    REQUIRE(back.has_value());
    REQUIRE(back->provenance == Provenance::real);      // real vs synthetic survives
    REQUIRE(back->detector_id == "sensor.contact");
    REQUIRE(back->status == DetectorStatus::detected);
    REQUIRE(back->features["authorized"] == true);
    REQUIRE(back->features["consent_purpose"] == "security_monitor");
    REQUIRE(back->features.contains("acquired_at_ms"));
}

TEST_CASE("a synthetic fixture stays synthetic through the same path", "[integration][skills]") {
    auto pipeline = make_pipeline(std::make_shared<DetectionFusionRule>("detection-fusion"));
    SecurityObservation fixture = sensor(SecuritySourceType::motion, "on");
    fixture.provenance = Provenance::synthetic;
    fixture.source = "simulator";
    pipeline->process(fixture.to_observation().to_event());

    const auto history = pipeline->get_recent_events(std::chrono::milliseconds(60000));
    REQUIRE(history.size() == 1);
    const auto back = Observation::from_event(history[0]);
    REQUIRE(back.has_value());
    REQUIRE(back->provenance == Provenance::synthetic);
}

TEST_CASE("AlertPolicy still de-duplicates and bounds state for the new observations",
          "[integration][skills]") {
    AlertPolicy policy(std::chrono::milliseconds(60000));

    const Alert alert = Alert::create("alert-tamper", Severity::warning, "tamper detected", {"e1"},
                                      0.6, "detection-fusion", nlohmann::json::object());
    bool suppressed = false;
    REQUIRE(policy.admit(alert, suppressed));
    REQUIRE_FALSE(suppressed);
    REQUIRE_FALSE(policy.admit(alert, suppressed));
    REQUIRE(suppressed);
    REQUIRE(policy.delivered_count() == 1);
    REQUIRE(policy.suppressed_count() == 1);
    REQUIRE(policy.tracked_keys() >= 1);
}
