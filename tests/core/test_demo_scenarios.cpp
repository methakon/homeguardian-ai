#include "catch2/catch.hpp"

#include "core/Application.h"
#include "core/DemoScenarios.h"
#include "core/DemoScenarioRunner.h"
#include "core/Logger.h"
#include "core/PersistenceAudit.h"

#include <chrono>
#include <filesystem>
#include <string>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// Demo scenarios: the deterministic catalogue and the runner that executes each
// scenario through the REAL Application pipeline (SimulatorRuntime -> event
// sink -> Pipeline -> correlation -> AlertPolicy). These assert the ACTUAL
// event/alert results, not documentation. Simulator-only: no physical sensor.
// ---------------------------------------------------------------------------

namespace {

std::string temp_db(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() /
             ("hg_scen_" + tag + "_" + std::to_string(stamp) + ".db");
    return p.string();
}
void cleanup_db(const std::string& p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
}

// Run a scenario by id with capture enabled. Scopes the Application (singleton).
ScenarioRunResult run_by_id(const std::string& id) {
    const DemoScenario* spec = find_demo_scenario(id);
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(true);
    return run_demo_scenario(app, *spec);
}

}  // namespace

// ===========================================================================
// Catalogue
// ===========================================================================

TEST_CASE("DemoScenarios: catalogue is well-formed and uniquely identified",
          "[scenario][catalogue]") {
    const auto& all = demo_scenarios();
    REQUIRE(all.size() >= 5);

    std::vector<std::string> ids;
    for (const auto& s : all) {
        REQUIRE(is_valid_scenario_token(s.id));
        REQUIRE_FALSE(s.title.empty());
        REQUIRE_FALSE(s.description.empty());
        REQUIRE(s.cycles >= 0);
        for (double e : s.detection_energies) {
            REQUIRE(e >= 0.0);
            REQUIRE(e <= 1.0);
        }
        ids.push_back(s.id);
    }
    // Unique ids.
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    // The documented scenarios exist.
    for (const char* id : {"default", "normal", "unusual", "inconclusive",
                           "consent-denied"}) {
        REQUIRE(find_demo_scenario(id) != nullptr);
    }
    // The default is a real entry.
    REQUIRE(find_demo_scenario(default_demo_scenario_id()) != nullptr);
}

TEST_CASE("DemoScenarios: unknown ids are rejected by the allowlist",
          "[scenario][catalogue]") {
    REQUIRE(find_demo_scenario("bogus") == nullptr);
    REQUIRE(find_demo_scenario("") == nullptr);
    REQUIRE(find_demo_scenario("NORMAL") == nullptr);      // case-sensitive
    REQUIRE(find_demo_scenario("consent denied") == nullptr);

    REQUIRE(is_valid_scenario_token("normal"));
    REQUIRE(is_valid_scenario_token("consent-denied"));
    REQUIRE_FALSE(is_valid_scenario_token(""));
    REQUIRE_FALSE(is_valid_scenario_token("A"));
    REQUIRE_FALSE(is_valid_scenario_token("a b"));
    REQUIRE_FALSE(is_valid_scenario_token("a/b"));
    REQUIRE_FALSE(is_valid_scenario_token(std::string(33, 'a')));
}

// ===========================================================================
// Scenario A - normal activity: no unjustified alert
// ===========================================================================

TEST_CASE("Scenario normal: routine activity produces no alert",
          "[scenario][normal]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("normal");

    REQUIRE(r.capture_enabled);
    REQUIRE(r.media_authorized == 4);          // 2 ticks x 2 consumers
    REQUIRE(r.alerts_generated == 0);
    REQUIRE(r.alerts_delivered == 0);
    REQUIRE(r.delivered_alerts.empty());

    // The detector RAN and the fused verdict was not_detected: the alert is
    // withheld because no detection qualified, not because the rule was absent.
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.detected == 0);
    REQUIRE(r.detection.not_detected == 2);
    REQUIRE(r.detection.fusion_verdict == "not_detected");
    REQUIRE(r.detection.fusion_contributing == 2);
    REQUIRE(r.detection.provenance == "synthetic");
    REQUIRE(r.privacy.denied == 0);
    REQUIRE(r.privacy.enforcement_ok);
}

// ===========================================================================
// Scenario B - unusual activity: traceable alert + duplicate suppression
// ===========================================================================

TEST_CASE("Scenario unusual: alert is traceable and duplicates are suppressed",
          "[scenario][unusual]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("unusual");

    REQUIRE(r.capture_enabled);
    REQUIRE(r.media_authorized == 10);         // 5 cycles x 2 consumers
    // The rule is re-evaluated against the recent-event window on EVERY
    // processed event, so once >=2 agreeing detector observations are in the
    // window every processed event re-fires it (10 firings for 15 events). The
    // alert policy delivers one and deduplicates the rest, so the user-visible
    // behaviour (exactly one alert) is correct.
    REQUIRE(r.alerts_generated == 10);
    REQUIRE(r.alerts_delivered == 1);
    REQUIRE(r.alerts_suppressed == 9);
    REQUIRE(r.alerts_suppressed == r.alerts_generated - 1);

    // The delivered alert is REAL pipeline output, traceable to the rule.
    REQUIRE(r.delivered_alerts.size() == 1);
    const Alert& a = r.delivered_alerts.front();
    // The alert comes from the DETECTION rule, and its severity is derived from
    // the fused evidence (confidence 1.00 -> warning).
    REQUIRE(a.rule_name == "detection-fusion");
    REQUIRE(a.severity == Severity::warning);
    REQUIRE_FALSE(a.message.empty());
    REQUIRE(a.details.contains("fusion_verdict"));
    REQUIRE(a.details["fusion_verdict"] == "detected");
    REQUIRE(a.details.contains("fusion_confidence"));
    REQUIRE(a.contributing_event_ids.size() >= 2);   // real evidence
    REQUIRE(a.confidence >= 0.0);
    REQUIRE(a.confidence <= 1.0);

    // Detection stage on the same run: all five inputs detected.
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.detected == 5);
    REQUIRE(r.detection.fusion_verdict == "detected");
}

// ===========================================================================
// Scenario C - inconclusive: no false claim
// ===========================================================================

TEST_CASE("Scenario inconclusive: ambiguous evidence yields no alert and no "
          "fabricated detection",
          "[scenario][inconclusive]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("inconclusive");

    REQUIRE(r.capture_enabled);
    REQUIRE(r.alerts_generated == 0);
    REQUIRE(r.alerts_delivered == 0);
    REQUIRE(r.delivered_alerts.empty());

    REQUIRE(r.media_authorized == 3);          // 3 ticks x 1 consumer
    // The detector reported inconclusive (never "detected") for every input...
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.detected == 0);
    REQUIRE(r.detection.inconclusive == 3);
    // ...and fusion refuses to invent a verdict without usable inputs.
    REQUIRE(r.detection.fusion_verdict == "unavailable");
    REQUIRE(r.detection.fusion_contributing == 0);
    REQUIRE(r.detection.fusion_confidence == 0.0);
}

// ===========================================================================
// Scenario D - consent denied: fail-closed
// ===========================================================================

TEST_CASE("Scenario consent-denied: denied acquisition is blocked, enforcement "
          "holds",
          "[scenario][consent]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("consent-denied");

    REQUIRE(r.capture_enabled);
    // The denied consumer produced no observation; the other one did.
    REQUIRE(r.media_authorized == 1);
    REQUIRE(r.privacy.authorized == 1);
    REQUIRE(r.privacy.denied == 1);
    REQUIRE(r.privacy.acquired == 1);          // only authorized = acquired
    REQUIRE(r.privacy.enforcement_ok);         // no acquisition without consent
    REQUIRE(r.denied_acquisitions == 1);
    REQUIRE(r.delivered_alerts.empty());       // one observation -> no alert
    REQUIRE_FALSE(r.detection.ran);            // no detection for denied input
}

TEST_CASE("Scenario: capture disabled is fail-closed (nothing delivered)",
          "[scenario][consent][failclosed]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("unusual");
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(false);   // config kill switch off
    const ScenarioRunResult r = run_demo_scenario(app, *spec);

    REQUIRE_FALSE(r.capture_enabled);
    REQUIRE(r.cycles == 0);
    REQUIRE(r.media_authorized == 0);
    REQUIRE(r.events_processed == 0);
    REQUIRE(r.alerts_generated == 0);
    REQUIRE(r.alerts_delivered == 0);
    REQUIRE(r.delivered_alerts.empty());
    REQUIRE_FALSE(r.detection.ran);
}

// ===========================================================================
// Repeated execution: no stale state
// ===========================================================================

TEST_CASE("Scenario: repeated execution has no stale state",
          "[scenario][repeat]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("unusual");
    REQUIRE(spec != nullptr);

    Application app;
    app.get_status().capture_enabled.store(true);
    const ScenarioRunResult a = run_demo_scenario(app, *spec);
    const ScenarioRunResult b = run_demo_scenario(app, *spec);

    REQUIRE(a.media_authorized == b.media_authorized);
    REQUIRE(a.alerts_generated == b.alerts_generated);
    REQUIRE(a.alerts_delivered == b.alerts_delivered);
    REQUIRE(a.alerts_suppressed == b.alerts_suppressed);
    REQUIRE(a.delivered_alerts.size() == b.delivered_alerts.size());
    REQUIRE(a.events_processed == b.events_processed);
}

// ===========================================================================
// Persistence + restart of the REAL delivered alerts
// ===========================================================================

TEST_CASE("Scenario: delivered alerts persist durably and survive restart",
          "[scenario][persist]") {
    Logger::initialize("error");
    const std::string db = temp_db("persist");
    const DemoScenario* spec = find_demo_scenario("unusual");
    REQUIRE(spec != nullptr);

    {
        PersistenceAudit audit(db);
        REQUIRE(audit.healthy());
        Application app;
        app.get_status().capture_enabled.store(true);
        const ScenarioRunResult r = run_demo_scenario(app, *spec);
        const size_t written = persist_run_alerts(audit, r);
        REQUIRE(written == 1);
        REQUIRE(audit.total_alert_count() == 1);
        // The persisted alert is the real rule output.
        auto recent = audit.alerts().find_recent(5);
        REQUIRE(recent.size() == 1);
        REQUIRE(recent.front().rule_name == "detection-fusion");
    }
    {
        // Reopen: the alert survived the restart and is counted as historical.
        PersistenceAudit audit(db);
        REQUIRE(audit.total_alert_count() == 1);
        REQUIRE(audit.historical_alert_count() == 1);
    }
    cleanup_db(db);
}

// ===========================================================================
// Severity mapping (non-sensitive JSON projection)
// ===========================================================================

TEST_CASE("Scenario: severity maps to stable non-sensitive strings",
          "[scenario][severity]") {
    // The Event header serializes Severity for JSON; the demo relies on this
    // stable mapping (info / warning / critical).
    REQUIRE(nlohmann::json(Severity::info).get<std::string>() == "info");
    REQUIRE(nlohmann::json(Severity::warning).get<std::string>() == "warning");
    REQUIRE(nlohmann::json(Severity::critical).get<std::string>() == "critical");
}

// ===========================================================================
// No physical sensor / hardcoded outcome
// ===========================================================================

TEST_CASE("Scenario: results are synthetic-only with no fabricated alert",
          "[scenario][privacy]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("unusual");
    // Provenance is always synthetic in this build.
    REQUIRE(r.detection.provenance == "synthetic");
    // Every delivered alert came from a real rule decision (rule name present).
    for (const auto& a : r.delivered_alerts) {
        REQUIRE_FALSE(a.rule_name.empty());
        REQUIRE_FALSE(a.alert_id.empty());
    }
}

// ===========================================================================
// Isolation: different scenarios in ONE long-lived process must not leak state
// (a regression guard for consent grants/withdrawals persisting across runs).
// ===========================================================================

TEST_CASE("Scenario: scenarios are isolated within one long-lived process",
          "[scenario][repeat][isolation]") {
    Logger::initialize("error");
    Application app;                                   // ONE process, many runs
    app.get_status().capture_enabled.store(true);

    const ScenarioRunResult normal1 = run_demo_scenario(app, *find_demo_scenario("normal"));
    REQUIRE(normal1.media_authorized == 4);            // 2 ticks x 2 consumers
    REQUIRE(normal1.alerts_delivered == 0);

    const ScenarioRunResult unusual = run_demo_scenario(app, *find_demo_scenario("unusual"));
    REQUIRE(unusual.media_authorized == 10);           // both consumers granted
    REQUIRE(unusual.alerts_delivered == 1);
    REQUIRE(unusual.privacy.denied == 0);              // no leakage of denials

    const ScenarioRunResult denied = run_demo_scenario(app, *find_demo_scenario("consent-denied"));
    REQUIRE(denied.media_authorized == 1);
    REQUIRE(denied.privacy.denied == 1);

    // After a run that WITHDREW consent, a later scenario must not inherit it.
    const ScenarioRunResult inconcl = run_demo_scenario(app, *find_demo_scenario("inconclusive"));
    REQUIRE(inconcl.media_authorized == 3);            // would exceed 3 if leaked
    REQUIRE(inconcl.alerts_delivered == 0);
    REQUIRE(inconcl.detection.fusion_verdict == "unavailable");

    // And the first scenario is reproducible after everything else ran.
    const ScenarioRunResult normal2 = run_demo_scenario(app, *find_demo_scenario("normal"));
    REQUIRE(normal2.media_authorized == normal1.media_authorized);
    REQUIRE(normal2.alerts_generated == normal1.alerts_generated);
    REQUIRE(normal2.privacy.denied == normal1.privacy.denied);
}
