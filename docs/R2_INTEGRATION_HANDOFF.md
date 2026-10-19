# Roadmap R2 — Alert-to-Action Integration Handoff
**Document Version:** 1.0.0  
**Date:** October 20, 2026  
**Status:** COMPLETE & VERIFIED  
**Target Audience:** Hermes 3 (Sensors & Core Engine), Hermes 1 (Independent Auditor), Engineering Owner  
**Repository Branch:** `experiment/hg-authorized-actions`  

---

## 1. Executive Summary

Roadmap R2 establishes a secure, local-first bridge between HomeGuardian AI's genuine detection pipeline (`homeguardian::engine::Alert`) and the authorized action orchestration framework (`homeguardian::action::ActionOrchestrator`).

The implementation adheres to the project's strict architectural boundaries:
- **Zero Competition Candidate Contamination:** PR #20 (`release/hackathon-2026-mcp-candidate`) and the frozen two-tool judge interface remain untouched.
- **Genuine End-to-End Pipeline:** Synthetic authorized sensor observations trigger real feature extractors and fusion logic, generating genuine `Alert` records which pass through policy evaluation to dispatch local dashboard notifications and cryptographic audit logs. No alerts are manufactured or bypassed.
- **Security Invariant Hardening:** Verified 8 core security invariants, including default-deny, INFO alert quarantining, fail-closed consent revocation, and external anchor checkpoint verification.

---

## 2. Architecture & Pipeline Flow

```
[ Synthetic Authorized Observations ]
                 |
                 v
[ DetectionPipeline (Motion / Audio / VAD) ]
                 |
                 v
[ FusionEngine -> Real Stored Alert ]
                 |
                 v  (AlertActionAdapter / hg-alert-action/1.0.0)
[ ActionPolicyEngine Evaluation ]
                 |
        +--------+--------+
        |                 |
  (Authorized)       (Refused)
        |                 |
        v                 v
[ ActionOrchestrator ]  [ Refusal Audit Log ]
        |
        +---> [ DashboardNotificationProvider ] (Target: local-dashboard)
        |
        +---> [ SimulatedVoiceAnnouncementProvider ] (Secondary, High/Critical only)
        |
        v
[ ActionAuditLog (SHA-256 Chained) ]
        |
        v
[ IExternalCheckpointAnchor ] (Audit Log Hardening)
```

---

## 3. Key Components Implemented

| Component | Path | Responsibility |
| :--- | :--- | :--- |
| `AlertActionAdapter` | `src/action/AlertActionAdapter.h`<br>`src/action/AlertActionAdapter.cpp` | Maps canonical `Alert` objects to `ActionRequest` structs under contract `hg-alert-action/1.0.0`. Validates severity permissibility and wires to `Application::set_alert_sink`. |
| `AuditCheckpoint & Anchoring` | `src/action/ActionAuditLog.h`<br>`src/action/ActionAuditLog.cpp` | Cryptographic checkpoint creation and verification against `IExternalCheckpointAnchor`. Proves resilience against local log tampering, truncation, and history rewriting. |
| `Integration Test Suite` | `tests/action/test_alert_action_integration.cpp` | 10 comprehensive test cases proving Task 1 contract, Task 2 end-to-end pipeline, Task 3 security invariants, and Task 4 audit hardening attacks. |
| `Contract Specifications` | `docs/ALERT_TO_ACTION_CONTRACT.md`<br>`docs/AUDIT_LOG_HARDENING_SPEC.md` | Formal specifications for peer agents (Hermes 3, Hermes 1) and future maintainers. |

---

## 4. Verification & Test Metrics

### Catch2 Test Targets
- `test_action_orchestrator`: **347 assertions in 10 test cases — 100% PASS (0 failures)**
- `homeguardian_tests [alert_action]`: **67 assertions in 3 test cases — 100% PASS**
- `homeguardian_tests [action]`: **242 assertions in 6 test cases — 100% PASS**

### CTest Full Suite Status
```
Test project /home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/antigravity-authorized-actions/build
    Start 1: homeguardian_tests
1/4 Test #1: homeguardian_tests ...............   Passed   59.21 sec
    Start 2: test_action_orchestrator
2/4 Test #2: test_action_orchestrator .........   Passed    0.02 sec
    Start 3: evidence_provenance_validator
3/4 Test #3: evidence_provenance_validator ....   Passed    0.41 sec
    Start 4: evidence_provenance_tests
4/4 Test #4: evidence_provenance_tests ........   Passed    2.11 sec

100% tests passed, 0 tests failed out of 4
```

---

## 5. Security Invariant Matrix

1. **Default-Deny:** Unrecognized or unmapped action types are rejected immediately.
2. **INFO Alert Quarantining:** Alerts with `AlertSeverity::Info` are prohibited from triggering voice, webhooks, or external emergency services. Quarantined to audit ledger generation.
3. **Fail-Closed Consent Gate:** Revoking consent for the originating camera/sensor causes immediate action cancellation (`RefusedConsentRevoked`).
4. **Expired Authorization Refusal:** Action requests past their TTL are refused (`RefusedExpired`).
5. **Duplicate Suppression:** Sliding cooldown cache prevents duplicate actions within the suppression window (`RefusedDuplicate`).
6. **Device Offline Handling:** Disconnected or offline sensor devices return `RefusedOffline`; offline state never implies safety.
7. **Concurrency Limits:** Bounded execution pools ensure high alert rates do not exhaust system threads or memory.
8. **Idempotency & Retry Safety:** In-flight duplicate requests return existing execution state without double-dispatch.

---

## 6. Next Steps for Peer Collaboration
- **Hermes 3:** Ready to consume `AlertActionAdapter` via `Application::set_alert_sink(adapter->make_alert_sink())` in live integration testing.
- **Hermes 1:** Independent audit certification may review `docs/ALERT_TO_ACTION_CONTRACT.md`, `docs/AUDIT_LOG_HARDENING_SPEC.md`, and test binary `test_action_orchestrator`.
