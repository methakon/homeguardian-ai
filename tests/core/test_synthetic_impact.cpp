#include "catch2/catch.hpp"

#include "core/AlertPolicy.h"
#include "core/Application.h"
#include "core/ConsentGate.h"
#include "core/ConsentRecord.h"
#include "core/DemoScenarioRunner.h"
#include "core/DemoScenarios.h"
#include "core/DetectionFusionRule.h"
#include "core/ImpactClassifier.h"
#include "core/Logger.h"
#include "core/PersistenceAudit.h"
#include "core/Pipeline.h"
#include "core/SimulatorRuntime.h"
#include "core/SyntheticActivityDetector.h"
#include "core/SyntheticImpactRule.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// H-01s: SIMULATED critical impact scenario + severity prioritisation.
//
// This is NOT fall detection. H-01 (real pose-based fall detection) is not
// implemented. These tests assert the synthetic impact classifier's documented
// semantics, the critical alert it can produce, severity prioritisation, policy
// behaviour, consent gating, persistence and that L-11/L-12 are unaffected.
// ---------------------------------------------------------------------------

namespace {

using clock_t_ = std::chrono::system_clock;
const auto kT0 = clock_t_::time_point(std::chrono::seconds(1'700'000'000));

std::vector<Event> impact_events(SyntheticImpactClassifier& c,
                                 const std::vector<double>& samples) {
    std::vector<Event> out;
    for (size_t i = 0; i < samples.size(); ++i) {
        const ImpactResult r = c.classify(samples[i]);
        // Timestamp with "now": the pipeline's recent-event window is
        // time-based, so a fixed past timestamp would be filtered out.
        out.push_back(c.to_observation(r, "impact-" + std::to_string(i),
                                       std::chrono::system_clock::now())
                          .to_event());
    }
    return out;
}

std::string temp_db(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() /
             ("hg_h01s_" + tag + "_" + std::to_string(stamp) + ".db");
    return p.string();
}
void cleanup_db(const std::string& p) {
    std::filesystem::remove(p);
    std::filesystem::remove(p + "-wal");
    std::filesystem::remove(p + "-shm");
}

ScenarioRunResult run_by_id(const std::string& id) {
    const DemoScenario* spec = find_demo_scenario(id);
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(true);
    return run_demo_scenario(app, *spec);
}

}  // namespace

// ===========================================================================
// 1. Normal activity -> no impact signal, no critical alert
// ===========================================================================

TEST_CASE("H-01s: normal activity produces no impact signal",
          "[impact][h01s]") {
    SyntheticImpactClassifier c;
    const auto rs = [&] {
        std::vector<ImpactResult> v;
        for (double s : {0.10, 0.12, 0.11, 0.09}) v.push_back(c.classify(s));
        return v;
    }();
    for (const auto& r : rs) {
        REQUIRE_FALSE(r.status == DetectorStatus::detected);
        REQUIRE(r.phase == "idle");
    }
    REQUIRE(c.confirmations() == 0);
    REQUIRE(c.pending_transient() == false);

    // ...and no rule fires on that evidence.
    std::vector<std::shared_ptr<ICorrelationRule>> rules{
        std::make_shared<SyntheticImpactRule>("synthetic-impact")};
    Pipeline p(rules);
    std::vector<Event> window;
    auto c2 = SyntheticImpactClassifier{};
    for (const auto& e : impact_events(c2, {0.10, 0.12, 0.11})) {
        window.push_back(e);
        REQUIRE_FALSE(p.process(e).has_value());
    }
}

// ===========================================================================
// 2. A single signal is NOT a confirmation
// ===========================================================================

TEST_CASE("H-01s: a lone high-intensity sample is inconclusive, not confirmed",
          "[impact][h01s]") {
    SyntheticImpactClassifier c;
    const ImpactResult r = c.classify(0.95);
    REQUIRE(r.status == DetectorStatus::inconclusive);
    REQUIRE(r.phase == "transient");
    REQUIRE_FALSE(r.status == DetectorStatus::detected);
    REQUIRE(r.reason.find("not confirmed") != std::string::npos);
    REQUIRE(c.confirmations() == 0);
    REQUIRE(c.pending_transient());

    // The rule must stay silent: 0 confirmations and only 1 supporting sample.
    auto rule = std::make_shared<SyntheticImpactRule>("synthetic-impact");
    SyntheticImpactClassifier c2;
    auto ev = impact_events(c2, {0.95});
    REQUIRE_FALSE(rule->evaluate(ev).has_value());
}

TEST_CASE("H-01s: a transient that expires is not confirmed",
          "[impact][h01s]") {
    SyntheticImpactClassifier c;   // max_window = 4
    // transient, then only mid-range samples (neither stillness nor a new
    // transient) until the pattern window expires.
    const std::vector<double> s = {0.95, 0.40, 0.40, 0.40, 0.40, 0.40};
    std::vector<ImpactResult> rs;
    for (double v : s) rs.push_back(c.classify(v));
    REQUIRE(c.confirmations() == 0);
    bool saw_expired = false;
    for (const auto& r : rs) {
        REQUIRE_FALSE(r.status == DetectorStatus::detected);
        if (r.phase == "expired") saw_expired = true;
    }
    REQUIRE(saw_expired);
}

// ===========================================================================
// 3./4. A confirmed pattern produces a critical alert with real evidence
// ===========================================================================

TEST_CASE("H-01s: a confirmed transient-then-stillness pattern raises a "
          "critical alert whose evidence comes from the detection result",
          "[impact][h01s]") {
    SyntheticImpactClassifier c;
    auto rule = std::make_shared<SyntheticImpactRule>("synthetic-impact");

    std::vector<Event> window;
    std::optional<Alert> last;
    for (const auto& e : impact_events(c, {0.10, 0.12, 0.11, 0.95, 0.10})) {
        window.push_back(e);
        last = rule->evaluate(window);
    }

    REQUIRE(last.has_value());
    REQUIRE(last->severity == Severity::critical);
    REQUIRE(last->rule_name == "synthetic-impact");
    REQUIRE(last->contributing_event_ids.size() >= 2);

    // Evidence is the classifier's own, not a hardcoded string.
    const auto& d = last->details;
    REQUIRE(d["impact_pattern"] == "transient_then_stillness");
    REQUIRE(d["transient_intensity"].get<double>() == Approx(0.95));
    REQUIRE(d["stillness_intensity"].get<double>() == Approx(0.10));
    REQUIRE(d["samples_since_transient"].get<size_t>() == 1);
    REQUIRE(d["supporting_observations"].get<size_t>() >= 2);
    REQUIRE(d["confirmed_observations"].get<size_t>() == 1);
    REQUIRE(d["provenance"] == "synthetic");
    REQUIRE(d["simulated"] == true);
    REQUIRE(d["emergency_services_contacted"] == false);
    // The message must not claim a real fall or an emergency response.
    REQUIRE(last->message.find("SIMULATED") != std::string::npos);
    REQUIRE(last->message.find("does not confirm a real fall") != std::string::npos);
    REQUIRE(last->message.find("emergency services") != std::string::npos);
}

// ===========================================================================
// 5. Severity prioritisation
// ===========================================================================

TEST_CASE("H-01s: a critical alert outranks an info/warning alert on the same "
          "evidence window",
          "[impact][h01s]") {
    // Both rules see the same window; the pipeline returns the highest severity.
    auto detection = std::make_shared<DetectionFusionRule>("detection-fusion");
    auto impact = std::make_shared<SyntheticImpactRule>("synthetic-impact");
    std::vector<std::shared_ptr<ICorrelationRule>> rules{detection, impact};
    Pipeline p(rules);

    // Feed activity evidence (two agreeing detections -> warning) and then a
    // confirmed impact pattern through the pipeline; every event is evaluated
    // against the pipeline's own recent-event window.
    SyntheticActivityDetector det;
    std::vector<Event> evs;
    evs.push_back(det.detect("act-1", 0.95).to_event());
    evs.push_back(det.detect("act-2", 0.93).to_event());
    SyntheticImpactClassifier ic;
    for (const auto& e : impact_events(ic, {0.10, 0.95, 0.10})) evs.push_back(e);

    std::optional<Alert> last;
    bool saw_warning_from_activity = false;
    for (const auto& e : evs) {
        auto a = p.process(e);
        if (!a) continue;
        if (a->rule_name == "detection-fusion") saw_warning_from_activity = true;
        last = a;
    }
    // The activity rule did fire on the same window...
    REQUIRE(saw_warning_from_activity);
    // ...but the critical impact alert is what the pipeline returns.
    REQUIRE(last.has_value());
    REQUIRE(last->severity == Severity::critical);
    REQUIRE(last->rule_name == "synthetic-impact");
}

TEST_CASE("H-01s: the pipeline still returns the single rule's alert unchanged",
          "[impact][h01s]") {
    auto detection = std::make_shared<DetectionFusionRule>("detection-fusion");
    std::vector<std::shared_ptr<ICorrelationRule>> rules{detection};
    Pipeline p(rules);
    SyntheticActivityDetector det;
    REQUIRE_FALSE(p.process(det.detect("a", 0.95).to_event()).has_value());
    auto alert = p.process(det.detect("b", 0.95).to_event());
    REQUIRE(alert.has_value());
    REQUIRE(alert->severity == Severity::warning);
}

// ===========================================================================
// 6./7./8. Policy: suppression, independence, rearm
// ===========================================================================

TEST_CASE("H-01s: duplicate critical alerts are suppressed, independent rules "
          "are not",
          "[impact][h01s]") {
    // Deterministic clock so the cooldown window is explicit.
    auto now = kT0;
    AlertPolicy policy(std::chrono::milliseconds(60000), /*max_keys=*/256,
                       [&now] { return now; });

    auto critical = Alert::create("c-1", Severity::critical, "simulated impact", {"e"},
                                  0.9, "synthetic-impact", nlohmann::json::object());
    bool suppressed = false;
    REQUIRE(policy.admit(critical, suppressed));      // first: delivered
    auto critical2 = Alert::create("c-2", Severity::critical, "simulated impact", {"e"},
                                   0.9, "synthetic-impact", nlohmann::json::object());
    REQUIRE_FALSE(policy.admit(critical2, suppressed));
    REQUIRE(suppressed);                              // duplicate suppressed

    // An unrelated info alert from a different rule is NOT suppressed.
    auto info = Alert::create("i-1", Severity::info, "activity", {"e"}, 0.7,
                              "detection-fusion", nlohmann::json::object());
    bool sup2 = false;
    REQUIRE(policy.admit(info, sup2));
    REQUIRE_FALSE(sup2);

    // Clear/rearm: after the condition clears, a new critical alert is eligible.
    policy.mark_condition_cleared("synthetic-impact");
    auto critical3 = Alert::create("c-3", Severity::critical, "simulated impact", {"e"},
                                   0.9, "synthetic-impact", nlohmann::json::object());
    bool sup3 = false;
    REQUIRE(policy.admit(critical3, sup3));
}

// ===========================================================================
// 9./10. Consent and fail-closed behaviour
// ===========================================================================

TEST_CASE("H-01s: capture disabled produces no impact processing and no critical "
          "alert",
          "[impact][h01s][consent]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("synthetic-critical-impact");
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(false);
    const ScenarioRunResult r = run_demo_scenario(app, *spec);
    REQUIRE_FALSE(r.impact.ran);
    REQUIRE(r.impact.confirmations == 0);
    REQUIRE(r.impact.critical_alerts == 0);
    REQUIRE(r.media_authorized == 0);
    REQUIRE(r.delivered_alerts.empty());
}

TEST_CASE("H-01s: expired and ambiguous consent fail closed", "[impact][h01s][consent]") {
    auto now = kT0;
    AcquisitionRequest req{"harness", "sim_alexa_enabled", "audio"};

    SECTION("expired grant is denied") {
        InMemoryConsentRepo repo;
        repo.save(ConsentRecord("c1", "harness", "sim_alexa_enabled", {"audio"},
                                ConsentDecision::granted, "1.0",
                                now - std::chrono::hours(2), now - std::chrono::hours(1),
                                "user", 1));
        ConsentGate gate(repo, /*capture=*/true);
        const GateDecision d = gate.check(req, now);
        REQUIRE_FALSE(d.authorized);
        REQUIRE(d.reason == DenyReason::expired);
    }
    SECTION("two conflicting active grants are denied") {
        InMemoryConsentRepo repo;
        repo.save(ConsentRecord("c1", "harness", "sim_alexa_enabled", {"audio"},
                                ConsentDecision::granted, "1.0",
                                now - std::chrono::hours(2), now + std::chrono::hours(1),
                                "user", 1));
        repo.save(ConsentRecord("c2", "harness", "sim_alexa_enabled", {"audio"},
                                ConsentDecision::granted, "1.0",
                                now - std::chrono::hours(1), now + std::chrono::hours(1),
                                "user", 1));
        ConsentGate gate(repo, /*capture=*/true);
        const GateDecision d = gate.check(req, now);
        REQUIRE_FALSE(d.authorized);
    }
    SECTION("capture disabled is denied even with a valid grant") {
        InMemoryConsentRepo repo;
        repo.save(ConsentRecord("c1", "harness", "sim_alexa_enabled", {"audio"},
                                ConsentDecision::granted, "1.0",
                                now - std::chrono::hours(1), now + std::chrono::hours(1),
                                "user", 1));
        ConsentGate gate(repo, /*capture=*/false);
        const GateDecision d = gate.check(req, now);
        REQUIRE_FALSE(d.authorized);
        REQUIRE(d.reason == DenyReason::capture_disabled);
    }
}

TEST_CASE("H-01s: withdrawn consent prevents delivery and no critical alert",
          "[impact][h01s][consent]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("consent-denied");
    REQUIRE(r.impact.ran == false);
    REQUIRE(r.impact.critical_alerts == 0);
    REQUIRE(r.privacy.denied == 1);
    REQUIRE(r.privacy.enforcement_ok);
}

// ===========================================================================
// 11. Inconclusive evidence -> no unjustified emergency claim
// ===========================================================================

TEST_CASE("H-01s: inconclusive impact evidence produces no critical alert",
          "[impact][h01s]") {
    Logger::initialize("error");
    // The shipped scenario does confirm a pattern; here we assert the negative
    // path directly on the rule, which is what the scenario depends on.
    SyntheticImpactClassifier c;
    auto rule = std::make_shared<SyntheticImpactRule>("synthetic-impact");
    std::vector<Event> window;
    // A transient and mid-range samples only: never confirmed.
    for (const auto& e : impact_events(c, {0.95, 0.40, 0.40, 0.40, 0.40, 0.40})) {
        window.push_back(e);
        REQUIRE_FALSE(rule->evaluate(window).has_value());
    }
}

TEST_CASE("H-01s: the impact observation states plainly that it is not fall "
          "detection",
          "[impact][h01s]") {
    SyntheticImpactClassifier c;
    const ImpactResult r = c.classify(0.95);
    const Observation o = c.to_observation(r, "i-1", kT0);
    REQUIRE(o.provenance == Provenance::synthetic);
    REQUIRE(o.source == "sim_sensor");
    REQUIRE(o.detector_id == "impact-signal");
    REQUIRE(o.note.find("not fall detection") != std::string::npos);
    REQUIRE(o.note.find("no emergency service") != std::string::npos);
}

// ===========================================================================
// 12. Repeated runs are deterministic
// ===========================================================================

TEST_CASE("H-01s: repeated runs do not leak stale state",
          "[impact][h01s][repeat]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("synthetic-critical-impact");
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(true);
    const ScenarioRunResult a = run_demo_scenario(app, *spec);
    const ScenarioRunResult b = run_demo_scenario(app, *spec);

    REQUIRE(a.impact.confirmations == b.impact.confirmations);
    REQUIRE(a.impact.transients == b.impact.transients);
    REQUIRE(a.impact.critical_alerts == b.impact.critical_alerts);
    REQUIRE(a.alerts_generated == b.alerts_generated);
    REQUIRE(a.alerts_delivered == b.alerts_delivered);
    REQUIRE(a.alerts_suppressed == b.alerts_suppressed);
}

// ===========================================================================
// 13./14. Persistence
// ===========================================================================

TEST_CASE("H-01s: the critical alert is persisted with correct metadata and "
          "survives restart",
          "[impact][h01s][persist]") {
    Logger::initialize("error");
    const std::string db = temp_db("critical");
    const DemoScenario* spec = find_demo_scenario("synthetic-critical-impact");
    REQUIRE(spec != nullptr);
    {
        PersistenceAudit audit(db);
        Application app;
        app.get_status().capture_enabled.store(true);
        const ScenarioRunResult r = run_demo_scenario(app, *spec);
        REQUIRE(r.impact.critical_alerts == 1);
        const size_t written = persist_run_alerts(audit, r);
        REQUIRE(written == 2);   // critical + info, both real deliveries

        bool found_critical = false;
        for (const auto& a : audit.alerts().find_recent(5)) {
            if (a.severity != Severity::critical) continue;
            found_critical = true;
            REQUIRE(a.rule_name == "synthetic-impact");
            REQUIRE(a.details["impact_pattern"] == "transient_then_stillness");
            REQUIRE(a.details["provenance"] == "synthetic");
            REQUIRE(a.details["emergency_services_contacted"] == false);
            // No media/biometric keys.
            REQUIRE_FALSE(a.details.contains("audio"));
            REQUIRE_FALSE(a.details.contains("video"));
            REQUIRE_FALSE(a.details.contains("biometric"));
            REQUIRE_FALSE(a.details.contains("payload"));
        }
        REQUIRE(found_critical);
        REQUIRE(audit.total_alert_count() == 2);
    }
    {
        PersistenceAudit audit(db);   // restart
        REQUIRE(audit.historical_alert_count() == 2);
        bool found_critical = false;
        for (const auto& a : audit.alerts().find_recent(5)) {
            if (a.severity == Severity::critical) found_critical = true;
        }
        REQUIRE(found_critical);
    }
    cleanup_db(db);
}

// ===========================================================================
// 15./16. Allowlist + dashboard-facing fields
// ===========================================================================

TEST_CASE("H-01s: the scenario id is allowlisted and malformed ids stay invalid",
          "[impact][h01s][api]") {
    REQUIRE(find_demo_scenario("synthetic-critical-impact") != nullptr);
    REQUIRE(find_demo_scenario("synthetic critical impact") == nullptr);
    REQUIRE(find_demo_scenario("bogus") == nullptr);
    REQUIRE(is_valid_scenario_token("synthetic-critical-impact"));
    REQUIRE_FALSE(is_valid_scenario_token("synthetic_critical_impact"));
    REQUIRE_FALSE(is_valid_scenario_token(""));

    const DemoScenario* s = find_demo_scenario("synthetic-critical-impact");
    REQUIRE(s->run_impact);
    REQUIRE(s->impact_intensities.size() == static_cast<size_t>(s->cycles));
}

TEST_CASE("H-01s: the scenario result exposes the backend critical evidence",
          "[impact][h01s][api][scenario]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("synthetic-critical-impact");
    REQUIRE(r.impact.ran);
    REQUIRE(r.impact.samples == 6);
    REQUIRE(r.impact.transients == 1);
    REQUIRE(r.impact.confirmations == 1);
    REQUIRE(r.impact.critical_alerts == 1);
    // The dashboard panel must be able to say WHY the pattern was confirmed:
    // the snapshot's last-sample reason describes only the final sample.
    REQUIRE_FALSE(r.impact.confirmed_reason.empty());
    REQUIRE(r.impact.confirmed_reason.find("transient 0.95") != std::string::npos);
    REQUIRE(r.impact.confirmed_reason.find("stillness 0.1") != std::string::npos);

    // The evidence the dashboard/API serialise.
    bool found = false;
    for (const auto& a : r.delivered_alerts) {
        if (a.severity != Severity::critical) continue;
        found = true;
        REQUIRE(a.details["transient_intensity"].get<double>() == Approx(0.95));
        REQUIRE(a.details["stillness_intensity"].get<double>() == Approx(0.10));
        REQUIRE(a.details["simulated"] == true);
    }
    REQUIRE(found);
    // Severity came from the backend, and both severities are present.
    REQUIRE(r.delivered_alerts.size() == 2);
    // The anomaly layer stayed advisory even though it flagged the drop.
    REQUIRE(r.anomaly.ran);
    REQUIRE(r.anomaly.status == "low");
    for (const auto& id : r.evidence_detectors) REQUIRE(id != "activity-anomaly");
}

// ===========================================================================
// 17. L-11 / L-12 unchanged
// ===========================================================================

TEST_CASE("H-01s: L-11 and L-12 scenarios remain correct",
          "[impact][h01s][l11][l12]") {
    Logger::initialize("error");
    {
        const auto r = run_by_id("unusual");
        REQUIRE(r.alerts_delivered == 1);
        REQUIRE(r.alerts_suppressed == 9);
        REQUIRE(r.delivered_alerts.front().severity == Severity::warning);
        REQUIRE(r.delivered_alerts.front().rule_name == "detection-fusion");
        REQUIRE(r.impact.ran == false);
    }
    {
        const auto r = run_by_id("normal");
        REQUIRE(r.alerts_delivered == 0);
        REQUIRE(r.delivered_alerts.empty());
    }
    {
        const auto r = run_by_id("anomaly-deviation");
        REQUIRE(r.anomaly.status == "high");
        REQUIRE(r.anomaly.anomalies >= 2);
        for (const auto& a : r.delivered_alerts) REQUIRE(a.severity != Severity::critical);
    }
}

// ===========================================================================
// 18. No physical sensing, no emergency communication
// ===========================================================================

TEST_CASE("H-01s: nothing here senses a room or contacts anyone",
          "[impact][h01s][privacy]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("synthetic-critical-impact");
    REQUIRE(r.privacy.enforcement_ok);
    for (const auto& a : r.delivered_alerts) {
        REQUIRE(a.details.value("provenance", std::string()) == "synthetic");
        if (a.severity == Severity::critical) {
            REQUIRE(a.details["simulated"] == true);
            REQUIRE(a.details["emergency_services_contacted"] == false);
        }
    }
}
