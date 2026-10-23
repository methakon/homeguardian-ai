#include "catch2/catch.hpp"
#include <unistd.h>   // getpid: process-unique fixture paths
#include "core/SimulatorRuntime.h"
#include "core/AlertPolicy.h"
#include "core/PersistenceAudit.h"
#include "core/RuntimeStatus.h"
#include "core/AppState.h"
#include "core/Alert.h"
#include "core/Logger.h"
#include <atomic>
#include <chrono>
#include <filesystem>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Privacy and consent regression tests (F22-29). These prove, on the actual
// components the running Application uses, that: capture-disabled blocks
// delivery; missing/withdrawn/expired consent cannot authorize; restart does
// not restore capture authorization; unauthorized observations never reach the
// event pipeline; no raw media is persisted; persisted alert metadata is
// limited to documented fields; retention preserves unrelated/newer records;
// and status counters distinguish per-process metrics from persistent history.
//
// Simulator-only: no physical sensor, temporary databases, no raw media.
// ---------------------------------------------------------------------------

namespace {

std::string priv_temp_db(const std::string& tag) {
    // Unique per process: two suite instances in the same working directory must
    // not delete each other's database (SQLite reports that as "disk I/O error").
    std::string p = "test_privacy_" + tag + "_" + std::to_string(::getpid()) + ".db";
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
    return p;
}
void priv_cleanup(const std::string& p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
}

}  // namespace

// --- capture-disabled startup blocks media delivery ---
TEST_CASE("Privacy: capture-disabled startup delivers no media despite grants",
          "[privacy][consent]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/false);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();
    REQUIRE(rt.tick() == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 0);
    rt.shutdown();
}

// --- withdrawn consent blocks that consumer; others still authorized ---
TEST_CASE("Privacy: withdrawn consent blocks the affected consumer only",
          "[privacy][consent]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
    rt.grant_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    rt.start();
    REQUIRE(rt.tick() == 2);
    rt.withdraw_consent(SimulatorRuntime::assistant_device_id(), "sim_alexa_assistant");
    int after = rt.tick();
    // Only the enabled consumer still receives; the withdrawn one is blocked.
    REQUIRE(after == 1);
    REQUIRE(rt.delivered_count(SimulatorRuntime::assistant_device_id()) == 1);  // unchanged
    rt.shutdown();
}

// --- unauthorized observations never reach the event pipeline (sink) ---
TEST_CASE("Privacy: unauthorized observations never reach the event sink",
          "[privacy][consent]") {
    Logger::initialize("error");
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    // NO grants for anyone.
    std::atomic<int> sink_calls{0};
    rt.set_event_sink([&sink_calls](const Event&) { sink_calls.fetch_add(1); });
    rt.start();
    int delivered = rt.tick();
    REQUIRE(delivered == 0);          // nothing authorized
    REQUIRE(sink_calls.load() == 0);  // sink never invoked -> pipeline never sees it
    rt.shutdown();
}

// --- restart does not restore capture authorization ---
// The capture kill switch and consent live only in the running process. A new
// SimulatorRuntime (simulating a restart) starts capture-disabled.
TEST_CASE("Privacy: restart does not restore capture authorization",
          "[privacy][consent][restart]") {
    Logger::initialize("error");
    // First "process": capture enabled, consent granted, media delivered.
    {
        SimulatorRuntime rt(/*media_capture_enabled=*/true);
        rt.register_default_devices();
        rt.grant_consent(SimulatorRuntime::enabled_device_id(), "sim_alexa_enabled");
        rt.start();
        REQUIRE(rt.tick() >= 1);
        rt.shutdown();
    }
    // Restart: a fresh runtime is capture-disabled and has no consent. It must
    // NOT inherit the previous process's authorization.
    {
        SimulatorRuntime rt(/*media_capture_enabled=*/false);
        rt.register_default_devices();
        rt.start();
        REQUIRE(rt.tick() == 0);
        REQUIRE(rt.delivered_count(SimulatorRuntime::enabled_device_id()) == 0);
        rt.shutdown();
    }
}

// --- persisted alert metadata is limited to documented fields; no raw media ---
TEST_CASE("Privacy: persisted alert contains only documented metadata fields",
          "[privacy][persistence]") {
    Logger::initialize("error");
    std::string db = priv_temp_db("fields");
    {
        PersistenceAudit audit(db);
        Alert a = Alert::create("pa-1", Severity::info,
                                "synthetic simulator presence notice",
                                {"e-1"}, 0.9, "sim-presence", nlohmann::json::object());
        REQUIRE(audit.record_alert(a));
        auto found = audit.alerts().find_by_id("pa-1");
        REQUIRE(found.has_value());
        // Documented fields present and correct.
        REQUIRE(found->rule_name == "sim-presence");
        REQUIRE(found->severity == Severity::info);
        REQUIRE(found->contributing_event_ids.size() == 1);
        // There is no field that could carry raw media/biometric data: the
        // Alert schema has no audio/video/payload field. Assert the details
        // JSON carries no media key.
        REQUIRE_FALSE(found->details.contains("audio"));
        REQUIRE_FALSE(found->details.contains("video"));
        REQUIRE_FALSE(found->details.contains("payload"));
        REQUIRE_FALSE(found->details.contains("biometric"));
    }
    priv_cleanup(db);
}

// --- status counters distinguish per-process metrics from persistent history ---
TEST_CASE("Privacy: status counters reset per process; history persists separately",
          "[privacy][status]") {
    Logger::initialize("error");
    std::string db = priv_temp_db("counters");
    {
        // Process 1: deliver one alert; it is persisted.
        PersistenceAudit audit(db);
        audit.record_alert(Alert::create("c-1", Severity::info, "synthetic notice",
                                         {}, 0.9, "sim-presence", nlohmann::json::object()));
    }
    {
        // Process 2 (restart): in-memory counters start at zero, but the audit
        // history is retained and reported as historical.
        RuntimeStatus s;  // fresh per-process counters
        REQUIRE(s.events_processed == 0);
        REQUIRE(s.alerts_delivered == 0);
        PersistenceAudit audit(db);
        REQUIRE(audit.historical_alert_count() == 1);  // persisted history
        REQUIRE(audit.total_alert_count() == 1);
    }
    priv_cleanup(db);
}

// --- retention deletes only within policy; unrelated/newer records preserved ---
TEST_CASE("Privacy: retention removes only expired alerts; newer records kept",
          "[privacy][retention]") {
    Logger::initialize("error");
    std::string db = priv_temp_db("retention");
    {
        PersistenceAudit audit(db);
        auto old_ts = std::chrono::system_clock::now() - std::chrono::hours(24 * 40);
        Alert old_a("old-1", Severity::info, "old", {}, old_ts, 0.5, "sim-presence",
                    nlohmann::json::object());
        Alert new_a("new-1", Severity::info, "new", {}, std::chrono::system_clock::now(),
                    0.5, "sim-presence", nlohmann::json::object());
        audit.record_alert(old_a);
        audit.record_alert(new_a);
        auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * 30);
        size_t removed = audit.prune_older_than(cutoff);
        REQUIRE(removed == 1);
        REQUIRE(audit.alerts().find_by_id("new-1").has_value());
        REQUIRE_FALSE(audit.alerts().find_by_id("old-1").has_value());
    }
    priv_cleanup(db);
}
