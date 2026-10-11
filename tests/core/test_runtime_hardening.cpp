#include "catch2/catch.hpp"
#include "core/SimulatorRuntime.h"
#include "core/AlertPolicy.h"
#include "core/Pipeline.h"
#include "core/TimeWindowCorrelation.h"
#include "core/EventValidator.h"
#include "core/Event.h"
#include "core/Config.h"
#include "core/Logger.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Runtime hardening: lifecycle, configuration validation, and long-run
// reliability. These drive the SAME components Application composes
// (SimulatorRuntime + AlertPolicy + Pipeline + Config) so the running-app
// behavior is covered without the Application singleton. Simulator-only.
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<Pipeline> make_pipeline(size_t max_history = 1000) {
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(std::make_shared<TimeWindowCorrelation>(
        "sim-presence", std::vector<EventType>{EventType::observation},
        /*min_count=*/2, std::chrono::milliseconds(60000),
        Severity::info, "synthetic simulator presence notice"));
    auto p = std::make_shared<Pipeline>(rules);
    p->set_max_history_size(max_history);
    return p;
}

// Mirror Application::process_event(): pipeline -> alert policy.
struct AppLikeProcessor {
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<AlertPolicy> policy;
    int events = 0;
    int delivered = 0;
    int suppressed = 0;

    void process(const Event& ev) {
        events++;
        auto alert = pipeline->process(ev);
        if (alert) {
            bool sup = false;
            if (policy->admit(*alert, sup)) delivered++;
            if (sup) suppressed++;
        } else {
            policy->mark_condition_cleared("sim-presence");
        }
    }
};

AppLikeProcessor make_processor(size_t max_history = 1000,
                                std::chrono::milliseconds cooldown = std::chrono::milliseconds(60000)) {
    AppLikeProcessor p;
    p.pipeline = make_pipeline(max_history);
    p.policy = std::make_shared<AlertPolicy>(cooldown);
    return p;
}

void write_config(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

}  // namespace

// ===========================================================================
// Lifecycle: repeated start/stop/shutdown are safe and deterministic
// ===========================================================================

TEST_CASE("Lifecycle: repeated start is idempotent; repeated shutdown is safe",
          "[lifecycle]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");

    rt.start();
    REQUIRE(rt.capture_started());
    rt.start();  // idempotent: second start does not throw or double-start
    REQUIRE(rt.capture_started());

    REQUIRE(rt.tick() == 1);

    rt.shutdown();
    REQUIRE(rt.shut_down());
    rt.shutdown();  // idempotent
    REQUIRE(rt.shut_down());
    // No delivery after shutdown.
    REQUIRE(rt.tick() == 0);
}

TEST_CASE("Lifecycle: tick before start delivers nothing (fail-closed)",
          "[lifecycle]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    // Not started.
    REQUIRE_FALSE(rt.capture_started());
    REQUIRE(rt.tick() == 0);
}

TEST_CASE("Lifecycle: repeated consent withdraw/restore cycles are bounded",
          "[lifecycle][consent-cycles]") {
    Logger::initialize("error");
    // Each cycle is a fresh runtime (shutdown is terminal by design). This
    // mirrors the operator pattern: start -> run -> withdraw -> shutdown.
    int total_events = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto proc = make_processor();
        SimulatorRuntime rt(/*capture=*/true);
        rt.register_default_devices();
        rt.set_event_sink([&](const Event& ev) { proc.process(ev); });
        rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
        rt.start();
        rt.tick();  // both consumers deliver
        // Withdraw both, then a final tick delivers nothing.
        rt.withdraw_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        rt.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
        REQUIRE(rt.tick() == 0);
        rt.shutdown();
        total_events += proc.events;
        // Per-cycle pipeline history is bounded by its cap.
        REQUIRE(proc.pipeline->get_recent_events(std::chrono::hours(1)).size() <= 1000);
    }
    // Each cycle delivered exactly 2 observations before withdrawal.
    REQUIRE(total_events == 10);
}

TEST_CASE("Lifecycle: capture kill-switch toggling is deterministic",
          "[lifecycle][capture-toggle]") {
    Logger::initialize("error");
    auto proc = make_processor();
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    rt.set_event_sink([&](const Event& ev) { proc.process(ev); });

    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.start();
    REQUIRE(rt.tick() == 1);       // delivered
    int events_after_first = proc.events;

    // Kill switch OFF at runtime blocks delivery (no restart needed: the gate
    // itself denies, and tick() still runs the capture path).
    rt.set_media_capture_enabled(false);
    rt.start();
    REQUIRE(rt.tick() == 0);       // blocked
    REQUIRE(proc.events == events_after_first);  // no new events

    // Kill switch ON again -> delivery resumes (consent grant persists in repo).
    rt.set_media_capture_enabled(true);
    rt.start();
    REQUIRE(rt.tick() == 1);       // delivered again
    REQUIRE(proc.events == events_after_first + 1);
    rt.shutdown();
}

// ===========================================================================
// Failure recovery + bounded memory
// ===========================================================================

TEST_CASE("Hardening: bounded event history under sustained load",
          "[hardening][bounded]") {
    Logger::initialize("error");
    auto proc = make_processor(/*max_history=*/16);
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.set_event_sink([&](const Event& ev) { proc.process(ev); });
    rt.start();

    for (int i = 0; i < 200; ++i) rt.tick();

    // Event history never exceeds the cap regardless of load.
    REQUIRE(proc.pipeline->get_recent_events(std::chrono::hours(1)).size() <= 16);
    // Alert policy state is bounded too.
    REQUIRE(proc.policy->tracked_keys() <= 256);
    rt.shutdown();
}

TEST_CASE("Hardening: alert dedup keeps delivered alerts bounded under repeated firing",
          "[hardening][alert-bound]") {
    Logger::initialize("error");
    auto proc = make_processor(/*max_history=*/1000,
                               /*cooldown=*/std::chrono::milliseconds(60000));
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.set_event_sink([&](const Event& ev) { proc.process(ev); });
    rt.start();

    for (int i = 0; i < 50; ++i) rt.tick();  // 100 observations

    // 100 observations would naively yield many alerts; dedup collapses the
    // continuous condition into a small number of delivered notifications.
    REQUIRE(proc.events == 100);
    REQUIRE(proc.delivered >= 1);
    REQUIRE(proc.delivered < 100);         // dedup effective
    REQUIRE(static_cast<size_t>(proc.delivered + proc.suppressed) ==
            proc.policy->delivered_count() + proc.policy->suppressed_count());
    rt.shutdown();
}

TEST_CASE("Hardening: shutdown with pending speaker work clears the queue",
          "[hardening][shutdown]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*capture=*/true, /*speaker_queue_capacity=*/4);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.start();
    rt.tick();
    // Whatever is pending is bounded by capacity.
    REQUIRE(rt.speaker_queue_size() <= 4);
    rt.shutdown();
    REQUIRE(rt.speaker_queue_size() == 0);   // cleared on shutdown
    REQUIRE_FALSE(rt.speaker_busy());
}

TEST_CASE("Hardening: invalid (future-timestamp) event is rejected; state uncorrupted",
          "[hardening][validation]") {
    Logger::initialize("error");
    auto proc = make_processor();
    auto future = std::chrono::system_clock::now() + std::chrono::hours(1);
    Event bad("future-1", 1, EventType::observation, "src", future, future,
              std::nullopt, std::nullopt, std::nullopt, {{"k", 1}},
              std::nullopt, std::nullopt, std::nullopt);
    REQUIRE(EventValidator::validate(bad).has_value());
    // Processing it through the app-like path does not crash or add history.
    proc.process(bad);
    REQUIRE(proc.pipeline->get_recent_events(std::chrono::hours(1)).size() == 0);
}

// ===========================================================================
// Configuration validation / fail-closed
// ===========================================================================

TEST_CASE("Config: safe defaults keep capture disabled",
          "[config][failclosed]") {
    Config c;
    REQUIRE_FALSE(c.media_capture_enabled);
    REQUIRE_FALSE(c.cloud_processing_enabled);
}

TEST_CASE("Config: invalid correlation_window_ms is rejected",
          "[config]") {
    Config c;
    c.correlation_window_ms = 0;
    REQUIRE_THROWS_AS(c.save("test_bad_window.json"), std::invalid_argument);
    c.correlation_window_ms = 60001;
    REQUIRE_THROWS_AS(c.save("test_bad_window2.json"), std::invalid_argument);
    c.correlation_window_ms = 5000;
}

TEST_CASE("Config: invalid alert_confidence_threshold is rejected",
          "[config]") {
    Config c;
    c.alert_confidence_threshold = -0.1;
    REQUIRE_THROWS_AS(c.save("test_bad_conf.json"), std::invalid_argument);
    c.alert_confidence_threshold = 1.1;
    REQUIRE_THROWS_AS(c.save("test_bad_conf2.json"), std::invalid_argument);
    c.alert_confidence_threshold = 0.5;
}

TEST_CASE("Config: invalid max_event_history is rejected",
          "[config]") {
    Config c;
    c.max_event_history = 0;
    REQUIRE_THROWS_AS(c.save("test_bad_hist.json"), std::invalid_argument);
    c.max_event_history = 1000;
}

TEST_CASE("Config: missing config file fails safely (does not enable capture)",
          "[config]") {
    Config c;
    REQUIRE_THROWS(c.load("/nonexistent/path/homeguardian.json"));
    // Defaults remain safe after a failed load.
    REQUIRE_FALSE(c.media_capture_enabled);
}

TEST_CASE("Config: malformed JSON is rejected",
          "[config]") {
    const std::string path = "test_malformed_cfg.json";
    write_config(path, "{ this is not valid json ");
    Config c;
    REQUIRE_THROWS(c.load(path));
    std::remove(path.c_str());
    REQUIRE_FALSE(c.media_capture_enabled);
}

TEST_CASE("Config: capture_enabled=true loads but cannot bypass the gate without consent",
          "[config][failclosed]") {
    // A config with capture enabled is valid, but the runtime still requires
    // consent per consumer; capture-enabled alone delivers nothing.
    Logger::initialize("error");
    SimulatorRuntime rt(/*capture=*/true);
    rt.register_default_devices();
    // No grants.
    rt.start();
    REQUIRE(rt.tick() == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
    rt.shutdown();
}

// ===========================================================================
// Determinism across repeated runs
// ===========================================================================

TEST_CASE("Hardening: repeated identical runs produce identical outcomes",
          "[hardening][determinism]") {
    auto run_once = []() {
        auto proc = make_processor(/*max_history=*/1000,
                                   /*cooldown=*/std::chrono::milliseconds(60000));
        SimulatorRuntime rt(/*capture=*/true);
        rt.register_default_devices();
        rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
        rt.set_event_sink([&](const Event& ev) { proc.process(ev); });
        rt.start();
        for (int i = 0; i < 10; ++i) rt.tick();
        rt.shutdown();
        return std::make_tuple(proc.events, proc.delivered, proc.suppressed);
    };
    auto a = run_once();
    auto b = run_once();
    REQUIRE(a == b);
    REQUIRE(std::get<0>(a) == 20);   // 2 consumers x 10 cycles
}
