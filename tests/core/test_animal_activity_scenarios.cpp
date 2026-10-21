#include "catch2/catch.hpp"

#include "core/AnimalActivityScenarios.h"
#include "core/Application.h"
#include "core/AlexaContract.h"
#include "core/DemoScenarioRunner.h"
#include "core/Logger.h"

#include "mcp/McpProtocol.h"
#include "mcp/McpTools.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace homeguardian;
using namespace homeguardian::mcp;
using nlohmann::json;

// ---------------------------------------------------------------------------
// Animal-activity demonstration scenarios (assignment B, items 6-9).
//
// WHAT THESE TESTS PROVE
//   That each scenario's INPUT produces its documented OUTPUT through the REAL pipeline:
//   simulator ticks -> detection stage (SyntheticActivityDetector + FusionEngine) ->
//   Observation -> Application::process_event -> correlation -> AlertPolicy -> delivered
//   alert -> the read-only MCP tool's alert summary. No step is stubbed and no alert is
//   fabricated: every alert asserted here was produced by existing rule/policy code.
//
// WHAT THEY DO NOT PROVE
//   Nothing about animals, species, live cameras or calamity prediction. The inputs are
//   bounded synthetic energy scalars with synthetic provenance; there is no audio, no video,
//   no classifier and no prediction path anywhere in this chain.
// ---------------------------------------------------------------------------

namespace {

class RehearsalVerifier : public IAlexaCredentialVerifier {
public:
    AlexaAuthContext verify(const std::string& credential,
                            std::chrono::system_clock::time_point) const override {
        AlexaAuthContext ctx;
        if (credential == "rehearsal-token") {
            ctx.authenticated = true;
            ctx.principal = "rehearsal-principal";
            ctx.allow_status = true;
            ctx.allow_alert_summary = true;
        }
        return ctx;
    }
};

// Reports what the run ACTUALLY delivered. It never invents an alert: the snapshot is built
// from the runner's delivered-alert list, so the MCP answer is a view of real pipeline output.
class RunBackedProvider : public IAlexaStatusProvider {
public:
    explicit RunBackedProvider(const ScenarioRunResult& r) {
        snap_.available = true;
        snap_.state = "Running";
        snap_.capture_enabled = r.capture_enabled;
        snap_.alert_count = r.delivered_alerts.size();
        size_t worst = 0;
        for (const auto& a : r.delivered_alerts) {
            const size_t rank = static_cast<size_t>(a.severity);
            worst = std::max(worst, rank);
            if (snap_.alert_ids.size() < ALEXA_MAX_ALERTS_RETURNED) snap_.alert_ids.push_back(a.alert_id);
        }
        if (!r.delivered_alerts.empty()) {
            snap_.highest_severity = severity_name(static_cast<Severity>(worst));
        }
    }
    AlexaStatusSnapshot snapshot() const override { return snap_; }

private:
    static std::string severity_name(Severity s) {
        switch (s) {
            case Severity::critical: return "critical";
            case Severity::warning:  return "warning";
            case Severity::info:     return "info";
        }
        return "info";
    }
    AlexaStatusSnapshot snap_{};
};

ScenarioRunResult run_animal_scenario(const std::string& id) {
    const DemoScenario* spec = find_animal_activity_scenario(id);
    REQUIRE(spec != nullptr);
    Application app;
    app.get_status().capture_enabled.store(true);
    return run_demo_scenario(app, *spec);
}

// Ask the REAL MCP tool the same question an MCP client would (read-only).
json mcp_alert_summary(const ScenarioRunResult& run) {
    RehearsalVerifier verifier;
    RunBackedProvider provider(run);
    ToolRegistry tools = make_homeguardian_tools(verifier, provider);
    Dispatcher dispatcher(tools);
    SessionState session;
    const std::string credential = "rehearsal-token";
    (void)dispatcher.handle(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"rehearsal","version":"1"}}})",
        session, credential);
    const auto out = dispatcher.handle(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}})",
        session, credential);
    REQUIRE(out.has_body);
    return json::parse(out.body);
}

}  // namespace

// ---------------------------------------------------------------- catalogue

TEST_CASE("Animal-activity catalogue: well-formed, unique, and separate from the demo allowlist",
          "[animal][scenario][catalogue]") {
    Logger::initialize("error");
    const auto& all = animal_activity_scenarios();
    REQUIRE(all.size() >= 4);

    std::vector<std::string> ids;
    for (const auto& s : all) {
        REQUIRE(is_valid_scenario_token(s.id));
        REQUIRE(s.id.rfind("animal-activity-", 0) == 0);   // namespaced by convention
        REQUIRE_FALSE(s.title.empty());
        REQUIRE_FALSE(s.description.empty());
        REQUIRE_FALSE(s.expectation.empty());
        REQUIRE(s.cycles >= 0);
        REQUIRE(s.detection_energies.size() <= 16);        // bounded inputs
        for (double e : s.detection_energies) REQUIRE(e >= 0.0);
        for (double e : s.detection_energies) REQUIRE(e <= 1.0);
        ids.push_back(s.id);
    }
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::unique(sorted.begin(), sorted.end()) == sorted.end());   // ids are unique

    // The ids must NOT collide with the shared demo allowlist: this catalogue augments the
    // demo, it does not silently change it.
    for (const auto& id : ids) REQUIRE(find_demo_scenario(id) == nullptr);
    for (const auto& s : demo_scenarios()) {
        REQUIRE(find_animal_activity_scenario(s.id) == nullptr);
    }

    // Lookup is an allowlist: an unknown id is rejected, not defaulted.
    REQUIRE(find_animal_activity_scenario("animal-activity-quiet") != nullptr);
    REQUIRE(find_animal_activity_scenario("animal-activity-nope") == nullptr);
    REQUIRE(find_animal_activity_scenario("") == nullptr);
}

// ---------------------------------------------------------------- the chain

TEST_CASE("Animal-activity: a quiet baseline produces no alert",
          "[animal][scenario][chain]") {
    Logger::initialize("error");
    const auto r = run_animal_scenario("animal-activity-quiet");

    REQUIRE(r.scenario_id == "animal-activity-quiet");
    REQUIRE(r.error.empty());
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.provenance == kAnimalActivityProvenance);   // labelled, not implied
    REQUIRE(r.detection.observations >= 1);
    REQUIRE(r.delivered_alerts.empty());
    REQUIRE(r.alerts_delivered == 0);

    // The MCP view agrees: nothing to report.
    const json summary = mcp_alert_summary(r);
    REQUIRE(summary["result"]["content"].is_array());
    const json payload = json::parse(summary["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(payload["alert_count"].get<size_t>() == 0);
    REQUIRE(payload["alert_ids"].empty());
}

TEST_CASE("Animal-activity: sustained change is detected, fused, alerted, and MCP-visible",
          "[animal][scenario][chain]") {
    Logger::initialize("error");
    const auto r = run_animal_scenario("animal-activity-sustained");

    REQUIRE(r.error.empty());
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.provenance == "synthetic");
    REQUIRE(r.detection.detector_id.empty() == false);
    REQUIRE(r.detection.observations >= 1);
    REQUIRE(r.detection.detected >= 1);                       // the change was seen
    REQUIRE(r.detection.fusion_verdict == "detected");        // fusion agreed
    REQUIRE(r.detection.fusion_confidence == Approx(1.0));
    REQUIRE(r.detection.fusion_contributing >= 1);

    // The alert came out of the pipeline, not out of the fixture.
    REQUIRE_FALSE(r.delivered_alerts.empty());
    REQUIRE(r.alerts_delivered >= 1);
    REQUIRE(r.alerts_delivered == r.delivered_alerts.size());

    // And it is visible through the read-only MCP tool.
    const json summary = mcp_alert_summary(r);
    const json payload = json::parse(summary["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(payload["alert_count"].get<size_t>() == r.delivered_alerts.size());
    REQUIRE(payload["read_only"].get<bool>());
    REQUIRE_FALSE(payload["highest_severity"].get<std::string>().empty());
    REQUIRE(payload["alert_ids"].size() == std::min<size_t>(r.delivered_alerts.size(),
                                                            ALEXA_MAX_ALERTS_RETURNED));
}

TEST_CASE("Animal-activity: one isolated spike is not a sustained change",
          "[animal][scenario][chain]") {
    Logger::initialize("error");
    const auto r = run_animal_scenario("animal-activity-single-spike");

    REQUIRE(r.error.empty());
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.observations >= 1);
    // The confirmation rule is the point of this scenario: no confirmed change, no alert.
    REQUIRE(r.alerts_delivered == 0);
    REQUIRE(r.delivered_alerts.empty());
    REQUIRE(r.detection.fusion_verdict != "detected");
}

TEST_CASE("Animal-activity: ambiguous evidence stays inconclusive and is never promoted",
          "[animal][scenario][uncertainty]") {
    Logger::initialize("error");
    const auto r = run_animal_scenario("animal-activity-inconclusive");

    REQUIRE(r.error.empty());
    REQUIRE(r.detection.ran);
    REQUIRE(r.detection.inconclusive >= 1);        // recorded AS uncertain
    REQUIRE(r.detection.fusion_verdict != "detected");
    REQUIRE(r.alerts_delivered == 0);
    REQUIRE(r.delivered_alerts.empty());
}

TEST_CASE("Animal-activity: consent withholds the observation, so there is nothing to alert on",
          "[animal][scenario][consent]") {
    Logger::initialize("error");
    const auto r = run_animal_scenario("animal-activity-no-consent");

    INFO("events_processed=" << r.events_processed << " media_authorized=" << r.media_authorized
                              << " denied_acquisitions=" << r.denied_acquisitions);
    REQUIRE(r.error.empty());
    REQUIRE(r.delivered_alerts.empty());
    REQUIRE(r.alerts_delivered == 0);
    // The fixture contains the same sustained activity as the sustained-change scenario, and
    // it must not reach the pipeline at all: no media was authorized and nothing was processed.
    REQUIRE(r.media_authorized == 0);
    REQUIRE(r.events_processed == 0);

    // The presence of activity in the fixture is irrelevant: without consent the pipeline
    // never receives it.
    const json summary = mcp_alert_summary(r);
    const json payload = json::parse(summary["result"]["content"][0]["text"].get<std::string>());
    REQUIRE(payload["alert_count"].get<size_t>() == 0);
}

TEST_CASE("Animal-activity: the same input reproduces the same outcome",
          "[animal][scenario][determinism]") {
    Logger::initialize("error");
    const auto a = run_animal_scenario("animal-activity-sustained");
    const auto b = run_animal_scenario("animal-activity-sustained");

    REQUIRE(a.detection.observations == b.detection.observations);
    REQUIRE(a.detection.detected == b.detection.detected);
    REQUIRE(a.detection.fusion_verdict == b.detection.fusion_verdict);
    REQUIRE(a.detection.fusion_confidence == Approx(b.detection.fusion_confidence));
    REQUIRE(a.alerts_delivered == b.alerts_delivered);
    REQUIRE(a.delivered_alerts.size() == b.delivered_alerts.size());
}
