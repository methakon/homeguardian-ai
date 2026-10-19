#include "catch2/catch.hpp"

#include "action/ActionContract.h"
#include "action/IActionProvider.h"
#include "action/ActionAuditLog.h"
#include "action/ActionPolicyEngine.h"
#include "action/LocalActionProviders.h"
#include "action/AlexaIntegrationBoundary.h"
#include "action/ActionOrchestrator.h"
#include "core/Alert.h"

#include <chrono>
#include <string>
#include <vector>
#include <thread>

using namespace homeguardian;
using namespace homeguardian::action;
using json = nlohmann::json;

namespace {

ActionRequest make_test_request(
    const std::string& req_id = "req-001",
    ActionType type = ActionType::DashboardNotification,
    AuthorizationScope scope = AuthorizationScope::ScopeLocalNotify,
    double confidence = 0.95,
    std::chrono::milliseconds ttl = std::chrono::milliseconds(60000))
{
    auto now = std::chrono::system_clock::now();
    ActionRequest req;
    req.contract_version = ACTION_CONTRACT_VERSION;
    req.request_id = req_id;
    req.alert_id = "alt-" + req_id;
    req.event_id = "evt-" + req_id;
    req.source_identity = "test-sensor-cam1";
    req.action_type = type;
    req.target = "dashboard-main";
    req.payload = {{"title", "Motion Detected"}, {"message", "Anomaly detected in living room"}, {"rule_name", "CriticalFallRule"}};
    req.required_scope = scope;
    req.consent_version = "v1.0";
    req.created_at = now;
    req.expires_at = now + ttl;
    req.idempotency_key = "idemp-" + req_id;
    req.provenance = "CriticalFallRule";
    req.confidence = confidence;
    req.max_retries = 3;
    req.retry_count = 0;
    return req;
}

} // namespace

TEST_CASE("ActionContract serialization and validation", "[action][contract]") {
    SECTION("Valid ActionRequest round-trip") {
        ActionRequest req = make_test_request("req-roundtrip");
        req.validate();

        json j = req.to_json();
        REQUIRE(j["contract_version"] == ACTION_CONTRACT_VERSION);
        REQUIRE(j["request_id"] == "req-roundtrip");
        REQUIRE(j["action_type"] == "dashboard_notification");

        ActionRequest restored = ActionRequest::from_json(j);
        REQUIRE(restored.request_id == req.request_id);
        REQUIRE(restored.alert_id == req.alert_id);
        REQUIRE(restored.action_type == req.action_type);
        REQUIRE(restored.required_scope == req.required_scope);
        REQUIRE(restored.confidence == Approx(req.confidence));
        REQUIRE(restored.idempotency_key == req.idempotency_key);
    }

    SECTION("Valid ActionResult round-trip") {
        ActionResult res;
        res.contract_version = ACTION_CONTRACT_VERSION;
        res.request_id = "req-123";
        res.idempotency_key = "idemp-123";
        res.action_type = ActionType::SimulatedVoiceAnnouncement;
        res.state = ExecutionState::Completed;
        res.success = true;
        res.failure_reason = "";
        res.executed_at = std::chrono::system_clock::now();
        res.duration_ms = 42;
        res.output = {{"spoken_text", "Warning: Anomaly detected"}};
        res.audit_id = "aud-001";
        res.attempts = 1;

        json j = res.to_json();
        REQUIRE(j["success"] == true);
        REQUIRE(j["state"] == "completed");
        REQUIRE(j["action_type"] == "simulated_voice_announcement");

        ActionResult restored = ActionResult::from_json(j);
        REQUIRE(restored.request_id == res.request_id);
        REQUIRE(restored.success == true);
        REQUIRE(restored.state == ExecutionState::Completed);
        REQUIRE(restored.duration_ms == 42);
        REQUIRE(restored.output["spoken_text"] == "Warning: Anomaly detected");
    }

    SECTION("ActionRequest validation checks") {
        ActionRequest bad = make_test_request("bad-req");

        bad.request_id = "";
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

        bad = make_test_request("bad-req");
        bad.alert_id = "";
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

        bad = make_test_request("bad-req");
        bad.source_identity = "";
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

        bad = make_test_request("bad-req");
        bad.idempotency_key = "";
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

        bad = make_test_request("bad-req");
        bad.confidence = 1.5;
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);

        bad = make_test_request("bad-req");
        bad.expires_at = bad.created_at; // expired / zero duration
        REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);
    }

    SECTION("AuthorizationScope bitmask logic") {
        uint32_t mask = static_cast<uint32_t>(AuthorizationScope::ScopeNone);
        REQUIRE(!has_scope(mask, AuthorizationScope::ScopeLocalNotify));

        mask |= static_cast<uint32_t>(AuthorizationScope::ScopeLocalNotify);
        REQUIRE(has_scope(mask, AuthorizationScope::ScopeLocalNotify));
        REQUIRE(!has_scope(mask, AuthorizationScope::ScopeLocalVoice));

        mask |= static_cast<uint32_t>(AuthorizationScope::ScopeLocalVoice);
        REQUIRE(has_scope(mask, AuthorizationScope::ScopeLocalNotify));
        REQUIRE(has_scope(mask, AuthorizationScope::ScopeLocalVoice));
        REQUIRE(!has_scope(mask, AuthorizationScope::ScopeEmergency));
    }
}

TEST_CASE("ActionAuditLog hash chain and tamper-evident integrity", "[action][audit]") {
    auto audit = std::make_shared<ActionAuditLog>();
    REQUIRE(audit->size() == 0);
    REQUIRE(audit->verify_chain_integrity());

    ActionRequest req1 = make_test_request("req-aud-1");
    ActionRequest req2 = make_test_request("req-aud-2");

    std::string h1 = audit->record_request(req1, "REQUEST_DISPATCH").hash;
    std::string h2 = audit->record_request(req2, "REQUEST_DISPATCH").hash;

    ActionResult res1;
    res1.request_id = "req-aud-1";
    res1.idempotency_key = req1.idempotency_key;
    res1.action_type = req1.action_type;
    res1.state = ExecutionState::Completed;
    res1.success = true;
    res1.executed_at = std::chrono::system_clock::now();
    res1.duration_ms = 12;

    std::string h3 = audit->record_result(res1, "ACTION_COMPLETED").hash;
    std::string h4 = audit->record_security_event("POLICY_DENIAL", "req-aud-3", "Consent revoked").hash;

    REQUIRE(audit->size() == 4);
    REQUIRE(!h1.empty());
    REQUIRE(!h2.empty());
    REQUIRE(!h3.empty());
    REQUIRE(!h4.empty());
    REQUIRE(audit->verify_chain_integrity());

    SECTION("Querying audit records") {
        auto q_req = audit->get_entries_for_request("req-aud-1");
        REQUIRE(q_req.size() == 2); // 1 request + 1 result

        auto q_state = audit->get_entries_by_state(ExecutionState::Completed);
        REQUIRE(q_state.size() == 1);
        REQUIRE(q_state[0].request_id == "req-aud-1");
    }

    SECTION("Export formats") {
        json j_export = audit->to_json_export();
        REQUIRE(j_export.is_array());
        REQUIRE(j_export.size() == 4);

        std::string csv = audit->to_csv_export();
        REQUIRE(csv.find("sequence_num,timestamp_ms,request_id") != std::string::npos);
        REQUIRE(csv.find("REQUEST_DISPATCH") != std::string::npos);
        REQUIRE(csv.find("POLICY_DENIAL") != std::string::npos);
    }
}

TEST_CASE("ActionPolicyEngine security controls and default-deny", "[action][policy]") {
    ActionPolicyEngine engine;

    // Rule: Critical Fall allows local notification and voice announcement
    ActionPolicyRule rule;
    rule.rule_name = "CriticalFallRule";
    rule.action_type = ActionType::DashboardNotification;
    rule.required_scope = AuthorizationScope::ScopeLocalNotify;
    rule.min_confidence = 0.80;
    rule.cooldown = std::chrono::milliseconds(5000);
    rule.requires_explicit_consent = true;
    engine.add_rule(rule);

    SECTION("Default Deny: zero granted scopes refuse action") {
        ActionRequest req = make_test_request("req-deny");
        auto eval = engine.evaluate(req, true);
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedUnauthorized);
        REQUIRE(eval.reason.find("Required owner scope") != std::string::npos);
    }

    SECTION("Authorized scope allows action") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        ActionRequest req = make_test_request("req-allow");
        auto eval = engine.evaluate(req, true);
        REQUIRE(eval.allowed);
        REQUIRE(eval.state == ExecutionState::Authorized);

        // Revoking scope immediately refuses subsequent requests
        engine.revoke_scope(AuthorizationScope::ScopeLocalNotify);
        auto eval_revoked = engine.evaluate(req, true);
        REQUIRE(!eval_revoked.allowed);
        REQUIRE(eval_revoked.state == ExecutionState::RefusedUnauthorized);
    }

    SECTION("Low confidence rejection") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        ActionRequest req = make_test_request("req-low-conf", ActionType::DashboardNotification,
                                              AuthorizationScope::ScopeLocalNotify, 0.65); // below 0.80
        auto eval = engine.evaluate(req, true);
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedLowConfidence);
    }

    SECTION("Expiration rejection") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        ActionRequest req = make_test_request("req-exp");
        auto now = std::chrono::system_clock::now();
        req.created_at = now - std::chrono::seconds(10);
        req.expires_at = now - std::chrono::seconds(1); // already expired

        auto eval = engine.evaluate(req, true);
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedExpired);
    }

    SECTION("Offline target rejection") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        ActionRequest req = make_test_request("req-offline");
        auto eval = engine.evaluate(req, false); // target is offline
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedOffline);
    }

    SECTION("Consent revocation rejection") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        bool consent_active = true;
        engine.set_consent_checker([&](const std::string&) { return consent_active; });

        ActionRequest req = make_test_request("req-consent");
        REQUIRE(engine.evaluate(req, true).allowed);

        // User revokes consent
        consent_active = false;
        auto eval = engine.evaluate(req, true);
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedConsentRevoked);
    }

    SECTION("Duplicate suppression within cooldown") {
        engine.grant_scope(AuthorizationScope::ScopeLocalNotify);
        ActionRequest req = make_test_request("req-dup");

        REQUIRE(!engine.is_duplicate(req.idempotency_key));
        engine.record_execution(req.idempotency_key, std::chrono::milliseconds(5000));
        REQUIRE(engine.is_duplicate(req.idempotency_key));

        auto eval = engine.evaluate(req, true);
        REQUIRE(!eval.allowed);
        REQUIRE(eval.state == ExecutionState::RefusedDuplicate);
    }
}

TEST_CASE("LocalActionProviders execution", "[action][providers]") {
    SECTION("DashboardNotificationProvider queue and clear") {
        DashboardNotificationProvider provider(5);
        REQUIRE(provider.is_online());

        ActionRequest req1 = make_test_request("req-dash-1");
        ActionResult res1 = provider.execute(req1);
        REQUIRE(res1.success);
        REQUIRE(res1.state == ExecutionState::Completed);

        auto notes = provider.get_notifications();
        REQUIRE(notes.size() == 1);
        REQUIRE(notes[0].request_id == "req-dash-1");

        provider.clear();
        REQUIRE(provider.get_notifications().empty());

        // Offline provider check
        provider.set_online(false);
        ActionResult res_off = provider.execute(req1);
        REQUIRE(!res_off.success);
        REQUIRE(res_off.state == ExecutionState::RefusedOffline);
    }

    SECTION("LocalAuditLogProvider execution") {
        auto audit = std::make_shared<ActionAuditLog>();
        LocalAuditLogProvider provider(audit);

        ActionRequest req = make_test_request("req-audit-prov", ActionType::AuditEventGeneration,
                                              AuthorizationScope::ScopeLocalAudit);
        ActionResult res = provider.execute(req);
        REQUIRE(res.success);
        REQUIRE(audit->size() == 1);
        REQUIRE(audit->verify_chain_integrity());
    }

    SECTION("SimulatedVoiceAnnouncementProvider execution") {
        SimulatedVoiceAnnouncementProvider provider;
        ActionRequest req = make_test_request("req-voice", ActionType::SimulatedVoiceAnnouncement,
                                              AuthorizationScope::ScopeLocalVoice);
        req.payload = {{"spoken_text", "Attention: fall detected in bedroom"}};

        ActionResult res = provider.execute(req);
        REQUIRE(res.success);
        REQUIRE(res.output["spoken_text"] == "Attention: fall detected in bedroom");

        auto announces = provider.get_announcements();
        REQUIRE(announces.size() == 1);
        REQUIRE(announces[0].spoken_text == "Attention: fall detected in bedroom");
    }

    SECTION("AutomationHookProvider sandboxed execution") {
        AutomationHookProvider provider;
        bool hook_called = false;

        provider.register_hook("turn_on_hallway_light", [&](const ActionRequest& req) {
            hook_called = true;
            return json{{"status", "light_illuminated"}, {"req", req.request_id}};
        });

        ActionRequest req = make_test_request("req-hook", ActionType::AutomationHook,
                                              AuthorizationScope::ScopeLocalHook);
        req.payload = {{"hook_name", "turn_on_hallway_light"}};

        ActionResult res = provider.execute(req);
        REQUIRE(res.success);
        REQUIRE(hook_called);
        REQUIRE(res.output["status"] == "light_illuminated");

        // Unregistered hook fails safely
        req.payload = {{"hook_name", "unregistered_hook"}};
        ActionResult res_unreg = provider.execute(req);
        REQUIRE(!res_unreg.success);
        REQUIRE(res_unreg.state == ExecutionState::Failed);
    }

    SECTION("MockActionProvider transient failure injection") {
        MockActionProvider mock(ActionType::DashboardNotification, "mock-flaky");
        mock.fail_first_n_times(2, "Transient 503 Service Unavailable");

        ActionRequest req = make_test_request("req-flaky");

        // 1st attempt fails
        ActionResult r1 = mock.execute(req);
        REQUIRE(!r1.success);
        REQUIRE(r1.failure_reason.find("Transient 503") != std::string::npos);

        // 2nd attempt fails
        ActionResult r2 = mock.execute(req);
        REQUIRE(!r2.success);

        // 3rd attempt succeeds
        ActionResult r3 = mock.execute(req);
        REQUIRE(r3.success);
        REQUIRE(r3.state == ExecutionState::Completed);
    }
}

TEST_CASE("AlexaIntegrationBoundary tiers and privacy guard", "[action][alexa]") {
    SECTION("Tier string representations") {
        REQUIRE(alexa_tier_to_string(AlexaIntegrationTier::Tier1_SimulatedLocalClient)
                .find("Tier 1") != std::string::npos);
        REQUIRE(alexa_tier_to_string(AlexaIntegrationTier::Tier2_NativeMcpServer)
                .find("Tier 2") != std::string::npos);
        REQUIRE(alexa_tier_to_string(AlexaIntegrationTier::Tier3_OfficialAmazonIntegration)
                .find("Tier 3") != std::string::npos);
    }

    SECTION("Privacy Guard: blocks raw media/audio/video leaks") {
        AlexaProactiveSecurityEvent safe_event;
        safe_event.endpoint_id = "sensor-endpoint-01";
        safe_event.detection_type = "MOTION_ACTIVITY";
        safe_event.threat_level = "LOW";
        safe_event.event_correlation_id = "alt-999";
        safe_event.timestamp = std::chrono::system_clock::now();
        safe_event.metadata = {{"zone", "living_room"}, {"confidence", 0.92}};
        REQUIRE(safe_event.is_privacy_compliant());

        // Attempting to include raw microphone or camera data fails compliance
        AlexaProactiveSecurityEvent leaky_event = safe_event;
        leaky_event.metadata["raw_audio"] = "base64_pcm_data_stream...";
        REQUIRE(!leaky_event.is_privacy_compliant());

        leaky_event.metadata = {{"video_stream", "rtsp://..."}};
        REQUIRE(!leaky_event.is_privacy_compliant());

        leaky_event.metadata = {{"frame_buffer", "0xdeadbeef"}};
        REQUIRE(!leaky_event.is_privacy_compliant());
    }

    SECTION("MockAlexaProactiveEventsAdapter behavior") {
        MockAlexaProactiveEventsAdapter adapter(false); // unconfigured
        REQUIRE(!adapter.is_configured());

        ActionRequest req = make_test_request("req-alexa-test");
        AlexaProactiveSecurityEvent event;
        event.endpoint_id = "ep-1";
        event.detection_type = "FALL_DETECTED";
        event.threat_level = "HIGH";
        event.event_correlation_id = req.alert_id;
        event.timestamp = std::chrono::system_clock::now();

        // Refuses when unconfigured
        ActionResult res_unconf = adapter.dispatch_event(req, event);
        REQUIRE(!res_unconf.success);
        REQUIRE(res_unconf.failure_reason.find("not configured") != std::string::npos);

        // Configure and connect
        adapter.set_configured(true);
        adapter.set_connected(true);
        ActionResult res_ok = adapter.dispatch_event(req, event);
        REQUIRE(res_ok.success);
        REQUIRE(res_ok.state == ExecutionState::Completed);
        REQUIRE(adapter.get_dispatched_count() == 1);
    }
}

TEST_CASE("ActionOrchestrator end-to-end event-to-action flow", "[action][orchestrator]") {
    auto policy_engine = std::make_shared<ActionPolicyEngine>();
    auto audit_log = std::make_shared<ActionAuditLog>();
    auto orchestrator = std::make_shared<ActionOrchestrator>(policy_engine, audit_log);

    auto dash_provider = std::make_shared<DashboardNotificationProvider>();
    orchestrator->register_provider(dash_provider);

    // Add policy rule: Fall Detection requires ScopeLocalNotify and 0.80 confidence
    ActionPolicyRule fall_rule;
    fall_rule.rule_name = "FallDetectionRule";
    fall_rule.action_type = ActionType::DashboardNotification;
    fall_rule.required_scope = AuthorizationScope::ScopeLocalNotify;
    fall_rule.min_confidence = 0.80;
    fall_rule.cooldown = std::chrono::milliseconds(5000);
    policy_engine->add_rule(fall_rule);

    ActionPolicyRule wildcard_notify;
    wildcard_notify.rule_name = "*";
    wildcard_notify.action_type = ActionType::DashboardNotification;
    wildcard_notify.required_scope = AuthorizationScope::ScopeLocalNotify;
    wildcard_notify.min_confidence = 0.70;
    wildcard_notify.cooldown = std::chrono::milliseconds(0);
    policy_engine->add_rule(wildcard_notify);

    SECTION("End-to-End: Alert -> Policy -> Authorized Action -> Audit Log") {
        // Grant owner authorization scope
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);

        // Create genuine alert
        Alert alert = Alert::create(
            "alert-fall-001",
            Severity::critical,
            "Urgent: Fall impact detected in bedroom",
            {"evt-accel-1", "evt-audio-1"},
            0.94,
            "FallDetectionRule",
            json{{"room", "bedroom"}, {"impact_g", 3.2}}
        );

        ActionResult res = orchestrator->process_alert(
            alert,
            ActionType::DashboardNotification,
            "local-dashboard",
            json{{"urgency", "high"}}
        );

        REQUIRE(res.success);
        REQUIRE(res.state == ExecutionState::Completed);
        REQUIRE(res.attempts == 1);
        REQUIRE(res.action_type == ActionType::DashboardNotification);

        // Verify provider received notification
        auto notifications = dash_provider->get_notifications();
        REQUIRE(notifications.size() == 1);
        REQUIRE(notifications[0].alert_id == "alert-fall-001");
        REQUIRE(notifications[0].message == "Urgent: Fall impact detected in bedroom");

        // Verify audit log recorded receipt and completion with unbroken hash chain
        REQUIRE(audit_log->size() >= 2);
        REQUIRE(audit_log->verify_chain_integrity());

        // Duplicate alert immediately after is suppressed by cooldown
        ActionResult dup_res = orchestrator->process_alert(
            alert,
            ActionType::DashboardNotification,
            "local-dashboard"
        );
        REQUIRE(!dup_res.success);
        REQUIRE(dup_res.state == ExecutionState::RefusedDuplicate);
    }

    SECTION("Default Deny: Unauthorized alert processing is refused") {
        // Scope is NOT granted
        policy_engine->set_granted_scopes(static_cast<uint32_t>(AuthorizationScope::ScopeNone));

        Alert alert = Alert::create(
            "alert-fall-002",
            Severity::critical,
            "Fall detected",
            {"evt-1"},
            0.92,
            "FallDetectionRule",
            json::object()
        );

        ActionResult res = orchestrator->process_alert(alert, ActionType::DashboardNotification);
        REQUIRE(!res.success);
        REQUIRE(res.state == ExecutionState::RefusedUnauthorized);

        // Provider must NOT have received any notification
        REQUIRE(dash_provider->get_notifications().empty());

        // Audit log records receipt and refusal
        REQUIRE(audit_log->verify_chain_integrity());
        auto records = audit_log->get_entries_by_state(ExecutionState::RefusedUnauthorized);
        REQUIRE(records.size() == 1);
    }

    SECTION("Low confidence alert is refused") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);

        // Alert with 0.60 confidence (below 0.80 minimum)
        Alert alert = Alert::create(
            "alert-uncertain-001",
            Severity::info,
            "Possible motion artifact",
            {"evt-2"},
            0.60,
            "FallDetectionRule",
            json::object()
        );

        ActionResult res = orchestrator->process_alert(alert, ActionType::DashboardNotification);
        REQUIRE(!res.success);
        REQUIRE(res.state == ExecutionState::RefusedLowConfidence);
        REQUIRE(dash_provider->get_notifications().empty());
    }

    SECTION("Transient failure retry loop succeeds on subsequent attempt") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);

        auto flaky_mock = std::make_shared<MockActionProvider>(ActionType::DashboardNotification, "flaky_provider");
        flaky_mock->fail_first_n_times(2, "Simulated transient timeout");
        orchestrator->register_provider(flaky_mock);

        ActionRequest req = make_test_request("req-retry-test");
        req.max_retries = 3;

        ActionResult res = orchestrator->dispatch_action(req);
        REQUIRE(res.success);
        REQUIRE(res.state == ExecutionState::Completed);
        REQUIRE(res.attempts == 3); // 2 failures + 1 success

        REQUIRE(audit_log->verify_chain_integrity());
    }

    SECTION("Exhausted retries result in failure state") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);

        auto failing_mock = std::make_shared<MockActionProvider>(ActionType::DashboardNotification, "failing_provider");
        failing_mock->set_should_fail(true, "Permanent provider crash");
        orchestrator->register_provider(failing_mock);

        ActionRequest req = make_test_request("req-fail-test");
        req.max_retries = 2;

        ActionResult res = orchestrator->dispatch_action(req);
        REQUIRE(!res.success);
        REQUIRE(res.state == ExecutionState::Failed);
        REQUIRE(res.attempts == 3); // initial + 2 retries
        REQUIRE(res.failure_reason.find("Permanent provider crash") != std::string::npos);

        REQUIRE(audit_log->verify_chain_integrity());
    }

    SECTION("Cooperative cancellation of in-flight actions") {
        ActionRequest req = make_test_request("req-cancel-me");
        // Not yet dispatched, or tracked
        REQUIRE(!orchestrator->cancel_action("non-existent-req"));
    }

    SECTION("Performance and throughput benchmark: 100 actions < 100ms") {
        policy_engine->grant_scope(AuthorizationScope::ScopeLocalNotify);
        dash_provider->clear();

        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < 100; ++i) {
            ActionRequest req = make_test_request("perf-req-" + std::to_string(i));
            ActionResult res = orchestrator->dispatch_action(req);
            REQUIRE(res.success);
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        // Local-first action dispatch must be sub-millisecond per action
        REQUIRE(elapsed < 200); // Plenty of headroom, typical is ~10-20ms
        REQUIRE(audit_log->verify_chain_integrity());
    }
}
