#include "catch2/catch.hpp"

#include "action/ActionContract.h"
#include "action/IActionProvider.h"
#include "action/ActionAuditLog.h"
#include "action/ActionPolicyEngine.h"
#include "action/LocalActionProviders.h"
#include "action/AlexaIntegrationBoundary.h"
#include "action/ActionOrchestrator.h"
#include "action/AlertActionAdapter.h"

#include "core/Alert.h"
#include "core/Event.h"
#include "core/EventValidator.h"
#include "core/Pipeline.h"
#include "core/SimulatorRuntime.h"
#include "core/TimeWindowCorrelation.h"
#include "core/ConsentGate.h"
#include "core/Logger.h"

#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <future>

using namespace homeguardian;
using namespace homeguardian::action;
using json = nlohmann::json;

namespace {

Alert make_test_alert(
    const std::string& alert_id = "alt-001",
    Severity severity = Severity::warning,
    const std::string& rule_name = "PresenceRule",
    double confidence = 0.85,
    const std::string& device_id = "sensor-living-room")
{
    return Alert::create(
        alert_id,
        severity,
        "Motion detected in living room zone",
        {"evt-" + alert_id},
        confidence,
        rule_name,
        json{{"device_id", device_id}, {"zone", "living-room"}}
    );
}

} // namespace

// =============================================================================
// TASK 1: Contract Compatibility & Severity Mapping
// =============================================================================

TEST_CASE("AlertActionAdapter contract and severity mapping", "[alert_action][contract]") {
    auto policy_engine = std::make_shared<ActionPolicyEngine>();
    auto audit_log = std::make_shared<ActionAuditLog>();
    auto orchestrator = std::make_shared<ActionOrchestrator>(policy_engine, audit_log);
    AlertActionAdapter adapter(orchestrator);

    SECTION("Contract version verification") {
        REQUIRE(std::string(ALERT_ACTION_ADAPTER_CONTRACT_VERSION) == "hg-alert-action/1.0.0");
    }

    SECTION("Source identity resolution across heterogeneous Alert details") {
        // 1. Explicit device_id
        Alert a1 = Alert::create("a1", Severity::info, "m", {}, 0.9, "r", json{{"device_id", "cam-hallway"}});
        REQUIRE(AlertActionAdapter::resolve_source_identity(a1) == "cam-hallway");

        // 2. Source field
        Alert a2 = Alert::create("a2", Severity::info, "m", {}, 0.9, "r", json{{"source", "net-node-02"}});
        REQUIRE(AlertActionAdapter::resolve_source_identity(a2) == "net-node-02");

        // 3. Fallback to causal event prefix
        Alert a3 = Alert::create("a3", Severity::info, "m", {"cam-bedroom-evt1"}, 0.9, "r", json::object());
        REQUIRE(AlertActionAdapter::resolve_source_identity(a3) == "cam-bedroom-evt1");

        // 4. Default fallback
        Alert a4 = Alert::create("a4", Severity::info, "m", {}, 0.9, "r", json::object());
        REQUIRE(AlertActionAdapter::resolve_source_identity(a4) == "system-sensor-bridge");
    }

    SECTION("Severity to ActionType permissibility rules") {
        // INFO severity must NEVER authorize voice, hook, or emergency actions
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::info, ActionType::AuditEventGeneration));
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::info, ActionType::DashboardNotification));
        REQUIRE(!AlertActionAdapter::is_action_permissible_for_severity(Severity::info, ActionType::SimulatedVoiceAnnouncement));
        REQUIRE(!AlertActionAdapter::is_action_permissible_for_severity(Severity::info, ActionType::AutomationHook));
        REQUIRE(!AlertActionAdapter::is_action_permissible_for_severity(Severity::info, ActionType::EmergencyEscalation));

        // WARNING severity
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::warning, ActionType::DashboardNotification));
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::warning, ActionType::SimulatedVoiceAnnouncement));
        REQUIRE(!AlertActionAdapter::is_action_permissible_for_severity(Severity::warning, ActionType::EmergencyEscalation));

        // CRITICAL severity
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::critical, ActionType::DashboardNotification));
        REQUIRE(AlertActionAdapter::is_action_permissible_for_severity(Severity::critical, ActionType::SimulatedVoiceAnnouncement));
        REQUIRE(!AlertActionAdapter::is_action_permissible_for_severity(Severity::critical, ActionType::EmergencyEscalation));
    }
}

// =============================================================================
// TASK 2: End-to-End Pipeline
// Synthetic authorized sensor observations -> genuine HomeGuardian detection/fusion
// -> real stored alert -> authorized action policy -> local dashboard notification
// -> cryptographic action audit record
// =============================================================================

TEST_CASE("End-to-end pipeline: Synthetic observation to authorized dashboard action", "[alert_action][e2e]") {
    Logger::initialize("error");

    // 1. Initialize SimulatorRuntime with active media capture
    SimulatorRuntime rt(/*media_capture_enabled=*/true);
    rt.register_default_devices();
    const std::string dev1 = SimulatorRuntime::enabled_device_id();
    const std::string dev2 = SimulatorRuntime::assistant_device_id();
    rt.grant_consent(dev1, "sim_alexa_enabled");
    rt.grant_consent(dev2, "sim_alexa_assistant");

    // 2. Build Pipeline with real TimeWindowCorrelation rule (fires when >=2 observations arrive)
    std::vector<std::shared_ptr<ICorrelationRule>> rules;
    rules.push_back(std::make_shared<TimeWindowCorrelation>(
        "sim-presence",
        std::vector<EventType>{EventType::observation},
        /*min_count=*/2,
        std::chrono::milliseconds(60000),
        Severity::info,
        "synthetic simulator presence notice"
    ));
    auto pipeline = std::make_shared<Pipeline>(rules);
    pipeline->set_max_history_size(100);

    // 3. Initialize Action Framework
    auto policy_engine = std::make_shared<ActionPolicyEngine>();
    auto audit_log = std::make_shared<ActionAuditLog>();
    auto orchestrator = std::make_shared<ActionOrchestrator>(policy_engine, audit_log);

    auto dash_provider = std::make_shared<DashboardNotificationProvider>();
    orchestrator->register_provider(dash_provider);

    // Policy Rule: "sim-presence" allows DashboardNotification for ScopeLocalNotify
    ActionPolicyRule presence_policy;
    presence_policy.rule_name = "sim-presence";
    presence_policy.action_type = ActionType::DashboardNotification;
    presence_policy.required_scope = AuthorizationScope::ScopeLocalNotify;
    presence_policy.min_confidence = 0.50;
    presence_policy.cooldown = std::chrono::milliseconds(2000);
    presence_policy.requires_explicit_consent = false;
    policy_engine->add_rule(presence_policy);

    // Grant owner scope
    policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);

    // 4. Initialize AlertActionAdapter configured to map info to DashboardNotification
    AlertActionMappingPolicy mapping_policy;
    mapping_policy.info_action_type = ActionType::DashboardNotification;
    AlertActionAdapter adapter(orchestrator, mapping_policy);

    // 5. Connect SimulatorRuntime -> Pipeline -> AlertActionAdapter
    std::vector<Alert> captured_alerts;
    rt.set_event_sink([&](const Event& ev) {
        if (auto alert_opt = pipeline->process(ev)) {
            captured_alerts.push_back(*alert_opt);
            adapter.on_alert(*alert_opt);
        }
    });

    rt.start();

    // 6. Execute simulator tick: delivers 2 authorized synthetic observations into the pipeline
    int delivered_observations = rt.tick();
    REQUIRE(delivered_observations == 2);
    REQUIRE(captured_alerts.size() >= 1);

    const Alert& genuine_alert = captured_alerts.front();
    REQUIRE(genuine_alert.rule_name == "sim-presence");
    REQUIRE(genuine_alert.message == "synthetic simulator presence notice");

    // 7. Verify action executed and delivered to DashboardNotificationProvider
    auto notifications = dash_provider->get_notifications();
    REQUIRE(notifications.size() >= 1);
    REQUIRE(notifications.front().alert_id == genuine_alert.alert_id);
    REQUIRE(notifications.front().message == "synthetic simulator presence notice");

    // 8. Verify tamper-evident cryptographic action audit record
    REQUIRE(audit_log->size() >= 2); // Receipt + completion
    REQUIRE(audit_log->verify_chain_integrity());

    rt.shutdown();
}

// =============================================================================
// TASK 3: Security Invariants
// =============================================================================

TEST_CASE("Security Invariants for Alert-to-Action", "[alert_action][security]") {
    auto policy_engine = std::make_shared<ActionPolicyEngine>();
    auto audit_log = std::make_shared<ActionAuditLog>();
    auto orchestrator = std::make_shared<ActionOrchestrator>(policy_engine, audit_log);

    auto dash_provider = std::make_shared<DashboardNotificationProvider>();
    auto voice_provider = std::make_shared<SimulatedVoiceAnnouncementProvider>();
    orchestrator->register_provider(dash_provider);
    orchestrator->register_provider(voice_provider);

    // Base policy rule for critical fall alerts
    ActionPolicyRule fall_rule;
    fall_rule.rule_name = "FallRule";
    fall_rule.action_type = ActionType::DashboardNotification;
    fall_rule.required_scope = AuthorizationScope::ScopeLocalNotify;
    fall_rule.min_confidence = 0.75;
    fall_rule.cooldown = std::chrono::milliseconds(5000);
    fall_rule.requires_explicit_consent = true;
    policy_engine->add_rule(fall_rule);

    ActionPolicyRule voice_rule;
    voice_rule.rule_name = "FallRule";
    voice_rule.action_type = ActionType::SimulatedVoiceAnnouncement;
    voice_rule.required_scope = AuthorizationScope::ScopeLocalVoice;
    voice_rule.min_confidence = 0.75;
    voice_rule.cooldown = std::chrono::milliseconds(5000);
    voice_rule.requires_explicit_consent = true;
    policy_engine->add_rule(voice_rule);

    AlertActionAdapter adapter(orchestrator);

    SECTION("Invariant 1: Default-deny when no owner scope is granted") {
        policy_engine->set_granted_scopes(static_cast<uint32_t>(AuthorizationScope::ScopeNone));

        Alert alert = make_test_alert("alt-deny-01", Severity::critical, "FallRule", 0.90, "sensor-01");
        auto results = adapter.on_alert(alert);

        REQUIRE(!results.empty());
        REQUIRE(!results[0].success);
        REQUIRE(results[0].state == ExecutionState::RefusedUnauthorized);
        REQUIRE(dash_provider->get_notifications().empty());

        auto denied_entries = audit_log->get_entries_by_state(ExecutionState::RefusedUnauthorized);
        REQUIRE(denied_entries.size() >= 1);
        REQUIRE(audit_log->verify_chain_integrity());
    }

    SECTION("Invariant 2: INFO alerts cannot trigger safety-critical external or voice actions") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalVoice);

        // Attempt to configure policy mapping info to voice announcement
        AlertActionMappingPolicy risky_policy;
        risky_policy.info_action_type = ActionType::SimulatedVoiceAnnouncement; // Forbidden!
        adapter.set_policy(risky_policy);

        Alert info_alert = make_test_alert("alt-info-01", Severity::info, "InfoRule", 0.95, "sensor-01");
        auto results = adapter.on_alert(info_alert);

        REQUIRE(!results.empty());
        REQUIRE(!results[0].success);
        REQUIRE(results[0].state == ExecutionState::RefusedUnauthorized);
        REQUIRE(results[0].failure_reason.find("Safety Invariant Refusal") != std::string::npos);
        REQUIRE(voice_provider->get_announcements().empty());

        REQUIRE(adapter.get_statistics().info_alerts_quarantined == 1);
    }

    SECTION("Invariant 3: Consent revocation before dispatch stops the action") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalVoice);

        bool consent_active = true;
        policy_engine->set_consent_checker([&](const std::string& dev) {
            return consent_active && dev == "sensor-granted";
        });

        Alert alert = make_test_alert("alt-consent-01", Severity::critical, "FallRule", 0.90, "sensor-granted");

        // Action succeeds while consent active
        auto r1 = adapter.on_alert(alert);
        REQUIRE(!r1.empty());
        REQUIRE(r1[0].success);

        // Revoke consent
        consent_active = false;
        Alert alert2 = make_test_alert("alt-consent-02", Severity::critical, "FallRule", 0.90, "sensor-granted");
        auto r2 = adapter.on_alert(alert2);
        REQUIRE(!r2.empty());
        REQUIRE(!r2[0].success);
        REQUIRE(r2[0].state == ExecutionState::RefusedConsentRevoked);
    }

    SECTION("Invariant 4: Expired authorization is refused") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalVoice);

        AlertActionMappingPolicy expired_policy;
        expired_policy.default_ttl = std::chrono::milliseconds(0); // Expires immediately
        adapter.set_policy(expired_policy);

        Alert alert = make_test_alert("alt-exp-01", Severity::critical, "FallRule", 0.90);
        auto results = adapter.on_alert(alert);
        REQUIRE(!results.empty());
        REQUIRE(!results[0].success);
        REQUIRE(results[0].state == ExecutionState::RefusedExpired);
    }

    SECTION("Invariant 5: Duplicate alerts cannot produce duplicate effects") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalVoice);
        policy_engine->set_consent_checker([](const std::string&) { return true; });

        Alert alert = make_test_alert("alt-dup-01", Severity::warning, "FallRule", 0.88);

        // First dispatch succeeds
        auto r1 = adapter.on_alert(alert);
        REQUIRE(!r1.empty());
        REQUIRE(r1[0].success);
        REQUIRE(dash_provider->get_notifications().size() == 1);

        // Immediate duplicate dispatch within cooldown window is suppressed
        auto r2 = adapter.on_alert(alert);
        REQUIRE(!r2.empty());
        REQUIRE(!r2[0].success);
        REQUIRE(r2[0].state == ExecutionState::RefusedDuplicate);
        // Notification count remains exactly 1
        REQUIRE(dash_provider->get_notifications().size() == 1);
    }

    SECTION("Invariant 6: Device disconnect does not imply safety") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->set_consent_checker([](const std::string&) { return true; });

        // Simulate target dashboard going offline
        dash_provider->set_online(false);

        Alert alert = make_test_alert("alt-off-01", Severity::warning, "FallRule", 0.88);
        auto results = adapter.on_alert(alert);

        REQUIRE(!results.empty());
        REQUIRE(!results[0].success);
        REQUIRE(results[0].state == ExecutionState::RefusedOffline);

        // Verify offline refusal is recorded in audit ledger
        auto offline_entries = audit_log->get_entries_by_state(ExecutionState::RefusedOffline);
        REQUIRE(offline_entries.size() >= 1);
        REQUIRE(audit_log->verify_chain_integrity());
    }

    SECTION("Invariant 7: Concurrent alerts respect action limits") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->set_consent_checker([](const std::string&) { return true; });
        dash_provider->set_online(true);
        dash_provider->clear();

        // Add wildcard rule for concurrent test
        ActionPolicyRule concurrent_rule;
        concurrent_rule.rule_name = "*";
        concurrent_rule.action_type = ActionType::DashboardNotification;
        concurrent_rule.required_scope = AuthorizationScope::ScopeLocalNotify;
        concurrent_rule.min_confidence = 0.50;
        concurrent_rule.cooldown = std::chrono::milliseconds(0);
        concurrent_rule.requires_explicit_consent = false;
        policy_engine->add_rule(concurrent_rule);

        constexpr int kThreadCount = 10;
        constexpr int kAlertsPerThread = 10;
        std::vector<std::future<void>> futures;

        for (int t = 0; t < kThreadCount; ++t) {
            futures.push_back(std::async(std::launch::async, [&adapter, t]() {
                for (int i = 0; i < kAlertsPerThread; ++i) {
                    std::string id = "conc-" + std::to_string(t) + "-" + std::to_string(i);
                    Alert a = make_test_alert(id, Severity::warning, "ConcurrentRule", 0.80);
                    adapter.on_alert(a);
                }
            }));
        }

        for (auto& f : futures) {
            f.get();
        }

        // Verify all 100 actions handled thread-safely
        REQUIRE(adapter.get_statistics().alerts_received == kThreadCount * kAlertsPerThread);
        REQUIRE(adapter.get_statistics().actions_completed == kThreadCount * kAlertsPerThread);
        REQUIRE(dash_provider->get_notifications().size() <= 100);
        REQUIRE(audit_log->verify_chain_integrity());
    }

    SECTION("Invariant 8: Process crashes and retries preserve idempotency semantics") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        policy_engine->set_consent_checker([](const std::string&) { return true; });

        auto flaky_mock = std::make_shared<MockActionProvider>(ActionType::DashboardNotification, "flaky_mock");
        flaky_mock->fail_first_n_times(2, "Simulated transient crash/disconnect");
        orchestrator->register_provider(flaky_mock);

        ActionPolicyRule mock_rule;
        mock_rule.rule_name = "RetryRule";
        mock_rule.action_type = ActionType::DashboardNotification;
        mock_rule.required_scope = AuthorizationScope::ScopeLocalNotify;
        mock_rule.min_confidence = 0.70;
        mock_rule.cooldown = std::chrono::milliseconds(10000);
        policy_engine->add_rule(mock_rule);

        Alert alert = make_test_alert("alt-retry-01", Severity::warning, "RetryRule", 0.85);

        // First attempt succeeds after 2 internal retries
        auto r1 = adapter.on_alert(alert);
        REQUIRE(!r1.empty());
        REQUIRE(r1[0].success);
        REQUIRE(r1[0].attempts == 3);

        // Immediate replay of same alert is caught by idempotency cache
        auto r2 = adapter.on_alert(alert);
        REQUIRE(!r2.empty());
        REQUIRE(!r2[0].success);
        REQUIRE(r2[0].state == ExecutionState::RefusedDuplicate);
    }
}

// =============================================================================
// TASK 4: Audit-Log Hardening & Protected Checkpoints
// =============================================================================

TEST_CASE("Audit-Log Hardening: Local SHA-256 limits and protected checkpoint verification", "[audit][hardening]") {
    ActionAuditLog log;
    MockExternalCheckpointAnchor anchor("mock-external-witness-vault");

    // Populate log with 5 distinct records
    for (int i = 1; i <= 5; ++i) {
        log.record_event(
            "req-" + std::to_string(i),
            "alt-" + std::to_string(i),
            ActionType::DashboardNotification,
            "EVENT_" + std::to_string(i),
            ExecutionState::Completed,
            "Success rationale " + std::to_string(i)
        );
    }
    REQUIRE(log.size() == 5);
    REQUIRE(log.verify_chain_integrity());

    // Anchor checkpoint at sequence 5
    AuditCheckpoint cp5 = log.create_checkpoint(anchor.get_anchor_id());
    REQUIRE(cp5.sequence_num == 5);
    REQUIRE(!cp5.tip_hash.empty());
    REQUIRE(anchor.anchor_checkpoint(cp5));
    REQUIRE(anchor.get_anchored_checkpoints().size() == 1);

    SECTION("Happy Path: Unmodified log verifies against checkpoint") {
        auto res = log.verify_against_checkpoint(cp5);
        REQUIRE(res.valid);
        REQUIRE(res.status == CheckpointVerificationStatus::Valid);
    }

    SECTION("Attack 1: Detection of Log Modification (Payload Tampering)") {
        // Tamper with record #2's reason
        log.tamper_with_entry_for_testing(1, "MALICIOUSLY_ALTERED_REASON");

        auto res = log.verify_against_checkpoint(cp5);
        REQUIRE(!res.valid);
        REQUIRE(res.status == CheckpointVerificationStatus::LogModified);
        REQUIRE(res.details.find("Log modification detected") != std::string::npos);
    }

    SECTION("Attack 2: Detection of Log Truncation (Tail Deletion)") {
        // Truncate the last 2 records (log size becomes 3, while checkpoint expects 5)
        log.truncate_for_testing(2);
        REQUIRE(log.size() == 3);

        auto res = log.verify_against_checkpoint(cp5);
        REQUIRE(!res.valid);
        REQUIRE(res.status == CheckpointVerificationStatus::LogTruncated);
        REQUIRE(res.details.find("Log truncation detected") != std::string::npos);
    }

    SECTION("Attack 3: Detection of Unauthorized Replacement / Fork") {
        // Create an entirely new forged chain with 5 records starting from genesis
        ActionAuditLog forged_log;
        for (int i = 1; i <= 5; ++i) {
            forged_log.record_event(
                "forged-req-" + std::to_string(i),
                "forged-alt-" + std::to_string(i),
                ActionType::DashboardNotification,
                "FORGED_EVENT_" + std::to_string(i),
                ExecutionState::Completed,
                "Forged rationale " + std::to_string(i)
            );
        }
        REQUIRE(forged_log.size() == 5);
        REQUIRE(forged_log.verify_chain_integrity()); // Internally valid!

        // Replace local log with the forged log
        log.replace_chain_for_testing(forged_log.get_all_entries());

        // Verifying against the external anchor detects the fork/replacement!
        auto res = log.verify_against_checkpoint(cp5);
        REQUIRE(!res.valid);
        REQUIRE(res.status == CheckpointVerificationStatus::UnauthorizedForkOrReplacement);
        REQUIRE(res.details.find("Unauthorized fork or replacement detected") != std::string::npos);
    }
}
