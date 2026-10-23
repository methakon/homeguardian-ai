#include "catch2/catch.hpp"

#include "core/AnomalyMonitor.h"
#include "core/Application.h"
#include "core/DemoScenarioRunner.h"
#include "core/DemoScenarios.h"
#include "core/DetectionFusionRule.h"
#include "core/FusionEngine.h"
#include "core/Logger.h"
#include "core/PersistenceAudit.h"
#include "core/Pipeline.h"
#include "core/SyntheticActivityDetector.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace homeguardian;

// ---------------------------------------------------------------------------
// L-12: statistical anomaly detection integrated into the synthetic pipeline.
// Covers warm-up, normal, deviation, missing, the advisory/allowlist evidence
// semantics, consent/capture gating, repetition, and that L-11 alert semantics
// are unchanged.
// ---------------------------------------------------------------------------

namespace {

using clock_t_ = std::chrono::system_clock;
const auto kT0 = clock_t_::time_point(std::chrono::seconds(1'700'000'000));

AnomalyMonitor::Config cfg_for_demo() { return AnomalyMonitor::defaults(); }

std::vector<AnomalyResult> observe_all(AnomalyMonitor& m,
                                       const std::vector<double>& values) {
    std::vector<AnomalyResult> out;
    for (size_t i = 0; i < values.size(); ++i) {
        out.push_back(m.observe(values[i], kT0 + std::chrono::seconds(i)));
    }
    return out;
}

std::string temp_db(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = std::filesystem::temp_directory_path() /
             ("hg_anom_" + tag + "_" + std::to_string(stamp) + ".db");
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

// Build a pipeline containing only the detection/fusion rule, with a given
// evidence allowlist.
std::shared_ptr<DetectionFusionRule> make_rule(const std::vector<std::string>& allow) {
    auto rule = std::make_shared<DetectionFusionRule>("detection-fusion");
    rule->set_evidence_detector_ids(allow);
    return rule;
}

}  // namespace

// ===========================================================================
// 1. Warm-up must not produce a false anomaly
// ===========================================================================

TEST_CASE("L-12: baseline warm-up never flags an anomaly", "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    const auto rs = observe_all(m, {0.70, 0.72, 0.71});   // 3 < min_samples(4)
    REQUIRE(rs.size() == 3);
    for (const auto& r : rs) {
        REQUIRE_FALSE(r.anomalous);
        REQUIRE(r.kind == AnomalyKind::none);
    }
    const AnomalyStatus s = m.snapshot();
    REQUIRE_FALSE(s.baseline_ready);
    REQUIRE(s.status == "warm-up");
    REQUIRE(s.phase == "warm-up");
    REQUIRE_FALSE(s.deviation);
    REQUIRE(s.anomalies == 0);
    REQUIRE(s.reason.find("warming up") != std::string::npos);
}

// ===========================================================================
// 2. Normal observations stay within the baseline
// ===========================================================================

TEST_CASE("L-12: normal observations remain within the learned baseline",
          "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    const auto rs = observe_all(m, {0.70, 0.72, 0.71, 0.70, 0.72, 0.71});
    // The last two samples are evaluated (n >= min_samples) and must be normal.
    REQUIRE(rs[4].sample_count >= 4);
    REQUIRE_FALSE(rs[4].anomalous);
    REQUIRE_FALSE(rs[5].anomalous);
    const AnomalyStatus s = m.snapshot();
    REQUIRE(s.baseline_ready);
    REQUIRE(s.phase == "ready");
    REQUIRE(s.status == "normal");
    REQUIRE_FALSE(s.deviation);
    REQUIRE(s.anomalies == 0);
    REQUIRE(s.baseline_mean == Approx(0.71).epsilon(0.02));
}

// ===========================================================================
// 3. A qualifying deviation is explainable
// ===========================================================================

TEST_CASE("L-12: a qualifying deviation is flagged with an explanation",
          "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    const auto rs = observe_all(m, {0.70, 0.72, 0.71, 0.70, 0.72, 0.71, 0.99, 0.99});
    const auto s = m.snapshot();
    REQUIRE(s.baseline_ready);
    REQUIRE(s.anomalies >= 2);
    REQUIRE(s.deviation);
    REQUIRE(s.status == "high");
    REQUIRE(s.last_zscore > cfg_for_demo().zscore_threshold);
    // The explanation is the detector's own, and it is bounded.
    REQUIRE(s.reason.find("sigma above") != std::string::npos);
    REQUIRE(s.reason.size() <= 256);
    // ...and the observation handed to the pipeline carries it, plus an explicit
    // "this is not a safety verdict" note.
    const Observation o = m.to_observation(s.missing_checked ? rs[0] : rs[7],
                                           "a-1", kT0, cfg_for_demo().min_samples);
    REQUIRE(o.detector_id == "activity-anomaly");
    REQUIRE(o.provenance == Provenance::synthetic);
    REQUIRE(o.note.find("not a safety verdict") != std::string::npos);
    REQUIRE(o.features.contains("zscore"));
}

// ===========================================================================
// 4. Missing observations
// ===========================================================================

TEST_CASE("L-12: a deliberate gap is reported as missing, a recent one is not",
          "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    observe_all(m, {0.70, 0.72, 0.71});
    const auto last = kT0 + std::chrono::seconds(2);

    auto recent = m.check_missing(last + std::chrono::seconds(1));
    REQUIRE_FALSE(recent.anomalous);

    auto gap = m.check_missing(last + m.config().max_gap + std::chrono::seconds(1));
    REQUIRE(gap.anomalous);
    REQUIRE(gap.kind == AnomalyKind::missing);
    REQUIRE(gap.reason.find("no observation for") != std::string::npos);

    const AnomalyStatus s = m.snapshot();
    REQUIRE(s.missing_checked);
    REQUIRE(s.missing);
    REQUIRE(s.status == "missing");
    REQUIRE_FALSE(s.missing_reason.empty());
    // The value baseline is preserved (the gap probe does not erase it).
    REQUIRE(s.sample_count == 2);

    // A missing observation is never a detection observation.
    const Observation o = m.to_observation(gap, "a-missing", kT0,
                                           cfg_for_demo().min_samples);
    REQUIRE(o.status == DetectorStatus::inconclusive);
}

TEST_CASE("L-12: an unknown key is not reported missing", "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    auto r = m.check_missing(kT0 + std::chrono::hours(1));
    REQUIRE_FALSE(r.anomalous);
    REQUIRE(r.reason.find("never observed") != std::string::npos);
}

// ===========================================================================
// 5. Anomaly uncertainty never becomes a definitive alert
// ===========================================================================

TEST_CASE("L-12: anomaly observations alone do not alert when they are not "
          "evidence",
          "[anomaly][l12]") {
    // Default allowlist = activity detector only -> anomaly evidence is advisory.
    std::vector<std::shared_ptr<ICorrelationRule>> rules{make_rule({"activity-energy"})};
    Pipeline pipeline(rules);

    AnomalyMonitor m(cfg_for_demo());
    auto rs = observe_all(m, {0.70, 0.72, 0.71, 0.70, 0.72, 0.71, 0.99, 0.99});
    for (size_t i = 0; i < rs.size(); ++i) {
        auto o = m.to_observation(rs[i], "a-" + std::to_string(i), kT0,
                                  cfg_for_demo().min_samples);
        REQUIRE_FALSE(pipeline.process(o.to_event()).has_value());
    }
}

TEST_CASE("L-12: a warm-up anomaly observation is never a positive vote",
          "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    auto rs = observe_all(m, {0.70, 0.72});   // warm-up
    for (size_t i = 0; i < rs.size(); ++i) {
        auto o = m.to_observation(rs[i], "a-" + std::to_string(i), kT0,
                                  cfg_for_demo().min_samples);
        REQUIRE(o.status == DetectorStatus::inconclusive);
    }
    // Even if anomaly evidence IS allowed, warm-up observations contribute nothing.
    std::vector<std::shared_ptr<ICorrelationRule>> rules{make_rule({"activity-anomaly"})};
    Pipeline pipeline(rules);
    for (size_t i = 0; i < rs.size(); ++i) {
        auto o = m.to_observation(rs[i], "a-" + std::to_string(i), kT0,
                                  cfg_for_demo().min_samples);
        REQUIRE_FALSE(pipeline.process(o.to_event()).has_value());
    }
}

// ===========================================================================
// Evidence allowlist: the influence is explicit, causal and tested
// ===========================================================================

TEST_CASE("L-12: including anomaly evidence in the allowlist is what makes it "
          "influence the alert (explicit + causal)",
          "[anomaly][l12][causal]") {
    AnomalyMonitor m(cfg_for_demo());
    auto rs = observe_all(m, {0.70, 0.72, 0.71, 0.70, 0.72, 0.71, 0.99, 0.99});
    REQUIRE(m.snapshot().anomalies >= 2);

    // The detector's own evidence for the deviation: the flagged samples. (Its
    // within-baseline samples vote NEGATIVELY, which is one reason anomaly
    // evidence is advisory by default rather than fused wholesale.)
    std::vector<Event> anomalies;
    for (size_t i = 0; i < rs.size(); ++i) {
        if (!rs[i].anomalous) continue;
        anomalies.push_back(m.to_observation(rs[i], "a-" + std::to_string(i), kT0,
                                             cfg_for_demo().min_samples)
                                .to_event());
    }
    REQUIRE(anomalies.size() >= 2);

    // (a) With the anomaly detector NOT allowed, the same evidence cannot alert.
    {
        auto rule = make_rule({"activity-energy"});
        std::vector<Event> window;
        std::optional<Alert> last;
        for (const auto& e : anomalies) {
            window.push_back(e);
            last = rule->evaluate(window);
        }
        REQUIRE_FALSE(last.has_value());
    }
    // (b) Allowing it makes the SAME evidence raise a detection-driven alert.
    {
        auto rule = make_rule({"activity-anomaly"});
        std::vector<Event> window;
        std::optional<Alert> last;
        for (const auto& e : anomalies) {
            window.push_back(e);
            last = rule->evaluate(window);
        }
        REQUIRE(last.has_value());
        REQUIRE(last->rule_name == "detection-fusion");
        REQUIRE(last->details["fusion_verdict"] == "detected");
    }
}

TEST_CASE("L-12: an empty allowlist accepts every detector (documented)",
          "[anomaly][l12]") {
    auto rule = make_rule({});
    REQUIRE(rule->accepts_detector("activity-energy"));
    REQUIRE(rule->accepts_detector("activity-anomaly"));
    REQUIRE(rule->accepts_detector("anything"));
    rule->set_evidence_detector_ids({"activity-energy"});
    REQUIRE(rule->accepts_detector("activity-energy"));
    REQUIRE_FALSE(rule->accepts_detector("activity-anomaly"));
}

// ===========================================================================
// 6. Consent / capture gating
// ===========================================================================

TEST_CASE("L-12: capture disabled disables the anomaly stage entirely",
          "[anomaly][l12][consent]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("anomaly-deviation");
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(false);
    const ScenarioRunResult r = run_demo_scenario(app, *spec);
    REQUIRE_FALSE(r.anomaly.ran);          // never ran -> no anomaly processing
    REQUIRE(r.anomaly.sample_count == 0);
    REQUIRE(r.anomaly.anomalies == 0);
    REQUIRE(r.media_authorized == 0);
}

TEST_CASE("L-12: withdrawn consent stops observations before anomaly processing",
          "[anomaly][l12][consent]") {
    Logger::initialize("error");
    const ScenarioRunResult r = run_by_id("consent-denied");
    REQUIRE(r.anomaly.ran == false);       // scenario does not run the stage
    // The denied consumer produced no observation, and enforcement held.
    REQUIRE(r.privacy.denied == 1);
    REQUIRE(r.privacy.enforcement_ok);
    REQUIRE(r.delivered_alerts.empty());
}

// ===========================================================================
// 7. Repetition / baseline reset semantics
// ===========================================================================

TEST_CASE("L-12: the baseline resets per run, so repeats are identical",
          "[anomaly][l12][repeat]") {
    Logger::initialize("error");
    const DemoScenario* spec = find_demo_scenario("anomaly-deviation");
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(true);
    const ScenarioRunResult a = run_demo_scenario(app, *spec);
    const ScenarioRunResult b = run_demo_scenario(app, *spec);
    REQUIRE(a.anomaly.anomalies == b.anomaly.anomalies);
    REQUIRE(a.anomaly.status == b.anomaly.status);
    REQUIRE(a.anomaly.sample_count == b.anomaly.sample_count);
    REQUIRE(a.anomaly.baseline_mean == Approx(b.anomaly.baseline_mean));
    REQUIRE(a.anomaly.last_zscore == Approx(b.anomaly.last_zscore));
}

TEST_CASE("L-12: reset clears the learned baseline", "[anomaly][l12]") {
    AnomalyMonitor m(cfg_for_demo());
    observe_all(m, {0.70, 0.72, 0.71, 0.70, 0.72, 0.71});
    REQUIRE(m.snapshot().baseline_ready);
    m.reset();
    const AnomalyStatus s = m.snapshot();
    REQUIRE_FALSE(s.baseline_ready);
    REQUIRE(s.sample_count == 0);
    REQUIRE(s.anomalies == 0);
    REQUIRE(s.status == "warm-up");
}

// ===========================================================================
// Scenario-level results (what the API/dashboard serialise)
// ===========================================================================

TEST_CASE("L-12: anomaly scenarios report their real backend results",
          "[anomaly][l12][scenario]") {
    Logger::initialize("error");
    {
        const auto r = run_by_id("anomaly-warmup");
        REQUIRE(r.anomaly.ran);
        REQUIRE_FALSE(r.anomaly.baseline_ready);
        REQUIRE(r.anomaly.status == "warm-up");
        REQUIRE_FALSE(r.anomaly.deviation);
        REQUIRE(r.anomaly.anomalies == 0);
    }
    {
        const auto r = run_by_id("anomaly-normal");
        REQUIRE(r.anomaly.baseline_ready);
        REQUIRE(r.anomaly.status == "normal");
        REQUIRE_FALSE(r.anomaly.deviation);
        REQUIRE(r.anomaly.anomalies == 0);
    }
    {
        const auto r = run_by_id("anomaly-deviation");
        REQUIRE(r.anomaly.baseline_ready);
        REQUIRE(r.anomaly.status == "high");
        REQUIRE(r.anomaly.deviation);
        REQUIRE(r.anomaly.anomalies >= 2);
        // Advisory by default: the anomaly does not vote on the alert.
        REQUIRE_FALSE(r.anomaly.missing);
        for (const auto& id : r.evidence_detectors) {
            REQUIRE(id != "activity-anomaly");
        }
    }
    {
        const auto r = run_by_id("anomaly-missing");
        REQUIRE(r.anomaly.ran);
        REQUIRE(r.anomaly.missing_checked);
        REQUIRE(r.anomaly.missing);
        REQUIRE(r.anomaly.status == "missing");
        REQUIRE_FALSE(r.anomaly.missing_reason.empty());
    }
}

// ===========================================================================
// 8. L-11 alert semantics and explanations are unchanged
// ===========================================================================

TEST_CASE("L-12: the L-11 detection-driven alert semantics are unchanged",
          "[anomaly][l12][l11]") {
    Logger::initialize("error");
    const auto r = run_by_id("unusual");
    REQUIRE(r.alerts_delivered == 1);
    REQUIRE(r.alerts_suppressed == 9);
    REQUIRE(r.delivered_alerts.size() == 1);
    const Alert& a = r.delivered_alerts.front();
    REQUIRE(a.rule_name == "detection-fusion");
    REQUIRE(a.severity == Severity::warning);
    REQUIRE(a.details["provenance"] == "synthetic");
    REQUIRE(a.details.contains("fusion_verdict"));
    REQUIRE_FALSE(r.anomaly.ran);              // this scenario does not run it
    REQUIRE(r.evidence_detectors == std::vector<std::string>{"activity-energy"});
    // The detection evidence is unchanged by L-12: five activity observations.
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.detected == 5);
    REQUIRE(r.detection.observations == 5);
}

// ===========================================================================
// 9. Persistence + privacy remain correct
// ===========================================================================

TEST_CASE("L-12: anomaly scenarios persist only real detection-driven alerts",
          "[anomaly][l12][persist]") {
    Logger::initialize("error");
    const std::string db = temp_db("persist");
    const DemoScenario* spec = find_demo_scenario("anomaly-deviation");
    REQUIRE(spec != nullptr);
    {
        PersistenceAudit audit(db);
        Application app;
        app.get_status().capture_enabled.store(true);
        const ScenarioRunResult r = run_demo_scenario(app, *spec);
        REQUIRE(r.privacy.enforcement_ok);
        const size_t written = persist_run_alerts(audit, r);
        REQUIRE(written == r.delivered_alerts.size());
        REQUIRE(audit.total_alert_count() == written);
        for (const auto& a : audit.alerts().find_recent(5)) {
            REQUIRE(a.rule_name == "detection-fusion");
            // Metadata only: no anomaly internals leak as media.
            REQUIRE_FALSE(a.details.contains("audio"));
            REQUIRE_FALSE(a.details.contains("video"));
            REQUIRE_FALSE(a.details.contains("payload"));
        }
    }
    {
        PersistenceAudit audit(db);   // restart
        REQUIRE(audit.historical_alert_count() >= 1);
    }
    cleanup_db(db);
}

// ===========================================================================
// 10. No anomaly result can be fabricated without an acquisition
// ===========================================================================

TEST_CASE("L-12: the anomaly stage consumes only authorized observations",
          "[anomaly][l12][privacy]") {
    Logger::initialize("error");
    const auto r = run_by_id("anomaly-deviation");
    // One anomaly sample per authorized acquisition (per tick that delivered).
    REQUIRE(r.anomaly.sample_count == static_cast<size_t>(r.cycles - 1));
    REQUIRE(r.privacy.acquired == r.media_authorized);
    REQUIRE(r.privacy.enforcement_ok);
}
