# Alert-to-Action Integration Contract
**Contract Version:** `hg-alert-action/1.0.0`  
**Status:** Approved & Verified (Roadmap R2)  
**Authors:** Antigravity Engineering (Pair Programming with Hermes 3 & Hermes 1)  
**Target Systems:** HomeGuardian AI Detection Engine $\leftrightarrow$ Action Orchestrator  

---

## 1. Overview and Intent

The **Alert-to-Action Contract** defines the versioned, deterministic protocol connecting HomeGuardian AI's core detection and sensor fusion pipeline (`homeguardian::engine::Alert`) to the local-first authorized action orchestrator (`homeguardian::action::ActionOrchestrator`).

This contract establishes:
1. Exact mapping of detection alerts to authorized action requests.
2. Invariants ensuring safety-critical actions can never be triggered by unvetted or low-severity events.
3. Fail-closed consent propagation from physical/synthetic sensing devices.
4. Deterministic idempotency, retry handling, and audit chaining.

---

## 2. Structural Alignment with Hermes 3 Alert Contract

As verified in Hermes 3 commit `73a5790b2b09d9ecb3b2549cb4b9cd1e2228967c`, the canonical `Alert` structure produced by `DetectionPipeline` and `FusionEngine` possesses the following fields:

```cpp
struct Alert {
    std::string alert_id;                          // Unique UUID / identifier
    std::string rule_name;                         // Triggering heuristic or ML rule
    AlertSeverity severity;                        // Info, Low, Medium, High, Critical
    std::chrono::system_clock::time_point timestamp;
    std::string summary;                           // Human-readable summary
    std::vector<std::string> causal_event_ids;     // Linked observation/detection events
    std::unordered_map<std::string, std::string> details; // Metadata (device_id, score, etc.)
};
```

### Source Identity Extraction
Source identity represents the physical or synthetic sensor originating the observation. In accordance with privacy and consent boundaries:
1. `alert.details["device_id"]` (if populated by sensor driver or simulator).
2. `alert.details["source"]` (e.g. `"sim-sensor-01"` or `"rtsp-cam-livingroom"`).
3. First entry in `alert.causal_event_ids` (if matching device naming convention).
4. Fallback default: `"unspecified-sensor"`.

Consent is evaluated strictly on the **source identity**; output notification channels (e.g. `"local-dashboard"`) are consumers of actions, not consent-gated sensors.

---

## 3. Severity-to-Action Mapping Matrix

The adapter implements strict deterministic mappings between alert severities and action types:

| Alert Severity | Primary Action Type | Required Owner Scope | Min Confidence | Action Permissibility Invariants |
| :--- | :--- | :--- | :--- | :--- |
| **Info** | `AuditEventGeneration` | `ScopeLocalAudit` | `0.0` | **Safety Invariant:** Never triggers voice, webhooks, or emergency services. Quarantined to local audit records. |
| **Low** | `DashboardNotification` | `ScopeLocalNotify` | `0.50` | In-app/dashboard visual banner only. |
| **Medium** | `DashboardNotification` | `ScopeLocalNotify` | `0.65` | In-app/dashboard visual banner with elevated visual priority. |
| **High** | `DashboardNotification` *(Primary)*<br>`SimulatedVoiceAnnouncement` *(Secondary)* | `ScopeLocalNotify`<br>`ScopeLocalVoice` | `0.75` *(Dashboard)*<br>`0.85` *(Voice)* | Voice announcement dispatched only if `ScopeLocalVoice` is explicitly authorized and confidence $\ge 0.85$. |
| **Critical** | `DashboardNotification` *(Primary)*<br>`SimulatedVoiceAnnouncement` *(Secondary)* | `ScopeLocalNotify`<br>`ScopeLocalVoice` | `0.80` *(Dashboard)*<br>`0.85` *(Voice)* | High-urgency alert. External emergency dispatch is strictly mocked/refused unless physical life-safety integration is certified. |

### Quarantining Safety-Critical Actions on INFO Alerts
If a custom policy or malicious caller attempts to map an `AlertSeverity::Info` alert to `SimulatedVoiceAnnouncement`, `WebhookDispatch`, or `MockEmergencyServices`, `AlertActionAdapter::is_action_permissible_for_severity` returns `false`. The adapter rejects the request immediately with `ExecutionState::RefusedUnauthorized` and records a security audit anomaly.

---

## 4. Idempotency Key Derivation and TTL

To prevent alert flooding, replay attacks, and duplicate sound alarms:
- **Idempotency Key Format:**  
  `"act:" + alert.alert_id + ":" + action_type_to_string(action_type)`
- **Default TTL:**  
  120 seconds (`AlertActionMappingPolicy::default_ttl`).
- **Cooldown Cache:**  
  `ActionPolicyEngine` maintains a sliding cooldown window preventing re-execution of identical idempotency keys within the configured cooldown period (5,000 ms default).

---

## 5. Security & Verification Invariants

1. **Default-Deny:** Any action type without an explicitly registered `PolicyRule` and corresponding owner scope grant is refused (`RefusedUnauthorized`).
2. **Fail-Closed Consent Gate:** If the sensor device identified in `request.source_identity` has its consent revoked in `ConsentGate`, action evaluation immediately halts with `RefusedConsentRevoked`.
3. **Fail-Closed Device Status:** Disconnecting or offline devices result in immediate refusal (`RefusedOffline`). Offline state never implies safety or automatic authorization.
4. **Audit Immutability:** Every transition (`PendingAuthorization` $\to$ `Authorized` / `Refused*` $\to$ `Completed` / `Failed`) is cryptographically appended to the local SHA-256 hash-chained ledger.
