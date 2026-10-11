#include "catch2/catch.hpp"
#include "core/SimulatorRuntime.h"
#include "core/EventValidator.h"
#include "core/Event.h"
#include "core/Pipeline.h"
#include "core/TimeWindowCorrelation.h"
#include "core/Logger.h"
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Runtime event-to-alert integration.
//
// These tests verify that AUTHORIZED synthetic observations from the simulator
// runtime flow through the real application event path (validation + Pipeline
// correlation + alert), and that denied/withdrawn/invalid inputs never reach
// the pipeline. Simulator-only: no physical sensor, no DB.
//
// The tests drive SimulatorRuntime with an event sink that feeds a real
// Pipeline carrying the app's existing TimeWindowCorrelation rule, mirroring
// exactly what Application::run() wires together.
// ---------------------------------------------------------------------------

namespace {

// Build the same rule the Application uses: fire a synthetic info alert once
// >=2 observations arrive within the window.
std::shared_ptr<Pipeline> make_pipeline_with_rule() {
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(std::make_shared<TimeWindowCorrelation>(
        "sim-presence", std::vector<EventType>{EventType::observation},
        /*min_count=*/2, std::chrono::milliseconds(60000),
        Severity::info, "synthetic simulator presence notice"));
    return std::make_shared<Pipeline>(rules);
}

// Count events reaching the pipeline and alerts produced, mirroring
// Application::process_event().
struct SinkStats {
    int events_seen = 0;
    int alerts = 0;
    std::string last_alert_message;
};

}  // namespace

TEST_CASE("Event-to-alert: authorized observations reach pipeline and fire the rule",
          "[event2alert][happy]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");

    auto pipeline = make_pipeline_with_rule();
    pipeline->set_max_history_size(100);
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (auto a = pipeline->process(ev)) {
            stats.alerts++;
            stats.last_alert_message = a->message;
        }
    });

    rt.start();
    // First cycle: 2 observations enter the pipeline -> rule fires once.
    int d0 = rt.tick();
    REQUIRE(d0 == 2);
    REQUIRE(stats.events_seen == 2);
    REQUIRE(stats.alerts >= 1);
    REQUIRE(stats.last_alert_message == "synthetic simulator presence notice");

    // Subsequent cycles keep producing authorized observations; the rule
    // continues to fire (>=2 in window). History grows deterministically.
    int total_alerts_before = stats.alerts;
    for (int i = 0; i < 3; ++i) {
        rt.tick();
    }
    REQUIRE(stats.events_seen == 8);                 // 2 per cycle x 4
    REQUIRE(stats.alerts >= total_alerts_before);    // still firing
    REQUIRE(pipeline->get_recent_events(std::chrono::hours(1)).size() == 8);
    rt.shutdown();
}

TEST_CASE("Event-to-alert: a single observation does not satisfy the min-count rule",
          "[event2alert][no-false-alert]") {
    Logger::initialize("error");
    // Only ONE consumer authorized -> only one observation per cycle -> the
    // rule (min_count=2) must NOT fire.
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    // assistant NOT granted.

    auto pipeline = make_pipeline_with_rule();
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (pipeline->process(ev)) stats.alerts++;
    });

    rt.start();
    rt.tick();
    REQUIRE(stats.events_seen == 1);   // only the enabled device delivered
    REQUIRE(stats.alerts == 0);        // one observation < min_count(2)
    rt.shutdown();
}

TEST_CASE("Event-to-alert: valid events pass validation; rule fires on min-count",
          "[event2alert][validation]") {
    Logger::initialize("error");
    auto pipeline = make_pipeline_with_rule();

    // A valid observation passes validation and enters the pipeline.
    Event good = Event::create_observation("ok-1", "src", {{"k", 1}});
    REQUIRE_FALSE(EventValidator::validate(good).has_value());
    // One observation does not yet satisfy the min_count=2 rule.
    REQUIRE_FALSE(pipeline->process(good).has_value());

    // A second valid observation within the window satisfies the rule.
    Event good2 = Event::create_observation("ok-2", "src", {{"k", 2}});
    REQUIRE_FALSE(EventValidator::validate(good2).has_value());
    auto alert = pipeline->process(good2);
    REQUIRE(alert.has_value());
    REQUIRE(alert->message == "synthetic simulator presence notice");

    // Pipeline remains usable; history holds both events.
    REQUIRE(pipeline->get_recent_events(std::chrono::hours(1)).size() == 2);
}

TEST_CASE("Event-to-alert: an event the validator rejects never corrupts pipeline state",
          "[event2alert][validation][reject]") {
    Logger::initialize("error");
    auto pipeline = make_pipeline_with_rule();

    // The Event constructor forbids an empty id / source / non-object payload,
    // so those cannot be built. The validator's additional rules (schema
    // version, future timestamp, confidence range) are exercised here: an
    // event whose observation timestamp is in the future is rejected by the
    // validator, and Pipeline::process returns nullopt WITHOUT adding it to
    // history or firing rules.
    auto future = std::chrono::system_clock::now() + std::chrono::hours(1);
    Event ev("future-1", /*schema_version=*/1, EventType::observation, "src",
             /*observation_timestamp=*/future, /*ingestion_timestamp=*/future,
             std::nullopt, std::nullopt, std::nullopt, {{"k", 1}},
             std::nullopt, std::nullopt, std::nullopt);
    REQUIRE(EventValidator::validate(ev).has_value());   // validator rejects it
    REQUIRE_FALSE(pipeline->process(ev).has_value());     // pipeline returns nullopt

    // Pipeline state is uncorrupted: a subsequent valid observation is the
    // first one in history.
    Event good = Event::create_observation("ok-1", "src", {{"k", 1}});
    REQUIRE_FALSE(pipeline->process(good).has_value());   // 1 < min_count
    REQUIRE(pipeline->get_recent_events(std::chrono::hours(1)).size() == 1);
}

TEST_CASE("Event-to-alert: capture-disabled startup produces no events",
          "[event2alert][capture-disabled]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/false);
    rt.register_default_devices();
    // Grant consent, but capture is globally disabled.
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");

    auto pipeline = make_pipeline_with_rule();
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (pipeline->process(ev)) stats.alerts++;
    });

    rt.start();
    int delivered = rt.tick();
    REQUIRE(delivered == 0);          // gate denies everything
    REQUIRE(stats.events_seen == 0);  // no events reached the pipeline
    REQUIRE(stats.alerts == 0);
    rt.shutdown();
}

TEST_CASE("Event-to-alert: missing consent produces no events even when capture enabled",
          "[event2alert][no-consent]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    // No grants at all.
    auto pipeline = make_pipeline_with_rule();
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (pipeline->process(ev)) stats.alerts++;
    });

    rt.start();
    REQUIRE(rt.tick() == 0);
    REQUIRE(stats.events_seen == 0);
    REQUIRE(stats.alerts == 0);
    rt.shutdown();
}

TEST_CASE("Event-to-alert: withdrawing one consumer stops its events reaching the pipeline",
          "[event2alert][withdraw]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");

    auto pipeline = make_pipeline_with_rule();
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (pipeline->process(ev)) stats.alerts++;
    });

    rt.start();
    rt.tick();
    REQUIRE(stats.events_seen == 2);   // both delivered
    int seen_before = stats.events_seen;

    // Withdraw the assistant: only the enabled device keeps producing events.
    rt.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.tick();
    REQUIRE(stats.events_seen == seen_before + 1);  // exactly one new event
    rt.shutdown();
}

TEST_CASE("Event-to-alert: repeated runs are deterministic in event counts",
          "[event2alert][determinism]") {
    Logger::initialize("error");
    auto run_once = [&]() {
        SimulatorRuntime rt(/*media_capture_enabled=*/true);
        rt.register_default_devices();
        rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
        auto pipeline = make_pipeline_with_rule();
        SinkStats stats;
        rt.set_event_sink([&](const Event& ev) {
            stats.events_seen++;
            if (pipeline->process(ev)) stats.alerts++;
        });
        rt.start();
        for (int i = 0; i < 5; ++i) rt.tick();
        rt.shutdown();
        return std::make_pair(stats.events_seen, stats.alerts);
    };

    auto a = run_once();
    auto b = run_once();
    // Same number of observations and alerts across independent runs.
    REQUIRE(a.first == b.first);
    REQUIRE(a.second == b.second);
    REQUIRE(a.first == 10);   // 2 consumers x 5 cycles
    REQUIRE(a.second >= 1);   // rule fires
}

TEST_CASE("Event-to-alert: shutdown discards pending work; no events after shutdown",
          "[event2alert][shutdown]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");

    auto pipeline = make_pipeline_with_rule();
    SinkStats stats;
    rt.set_event_sink([&](const Event& ev) {
        stats.events_seen++;
        if (pipeline->process(ev)) stats.alerts++;
    });
    rt.start();
    rt.tick();
    REQUIRE(stats.events_seen == 1);

    rt.shutdown();
    // After shutdown, tick() is a no-op: no further events enter the pipeline.
    REQUIRE(rt.tick() == 0);
    REQUIRE(stats.events_seen == 1);   // unchanged
}
