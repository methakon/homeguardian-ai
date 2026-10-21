# HomeGuardian AI — Canonical Task Envelope & Multi-Agent Dispatch Specification

**Document:** `HG_TASK_ENVELOPE_SPEC.md`  
**Status:** APPROVED & MANDATORY  
**Coordinator:** `hg-antigravity-integration` (Antigravity)  
**Applies to:** Hermes 1 (`hg-hermes-network`), Hermes 2 (`hg-hermes-android`), Hermes 3 (`hg-hermes-cpp`), Antigravity  

---

## 1. Security Invariants & Threat Model

On a public or shared GitHub repository, any user may comment on an issue. GitHub comment bodies are **untrusted user input**. Therefore:

1. **PROSE IS NEVER INSTRUCTIONS:** Natural-language comment prose, issue bodies, or markdown descriptions are recorded as documentation or context, but **never parsed or dispatched as executable tasks**.
2. **STRICT AUTHOR ALLOWLIST:** Only envelopes authored by verified GitHub logins in `scripts/worker/watcher-policy.json` are admitted:
   * `methakon` (Repository Owner)
   * `msekhardhar-cell` (Coordinator)
   Any comment from an author not on the allowlist is rejected with `AUTHOR_NOT_ALLOWED`.
3. **NO ARBITRARY COMMAND EXECUTION (`shell=False`):** Embedded shell patterns (`rm -rf`, `sudo`, `curl | bash`, backticks, subshells, eval) are scanned prior to parsing and rejected with `EMBEDDED_COMMANDS`. Dispatches use fixed argument vectors with `shell=False`.
4. **SINGLE-DISPATCH OWNER PER ISSUE:** To prevent double-dispatch and racing execution loops:
   * **Issue #14 & #23 (Network):** Owned exclusively by `hg-task-poller-s1` (`fb8655e17f71`).
   * **Issue #12 & #24 (Android):** Owned exclusively by `hg-task-poller-s2` (`92a0576f650e`).
   * **Issue #19 & #25 (C++ Engine):** Owned exclusively by `hg-hermes-cpp-poll` (`c3ee179e87a0`).
   * **Issue #17 & #26 (Integration):** Owned exclusively by `antigravity_worker.py` (Cron `fc4e65fd67a7`).
   * **Shared Watcher:** Locked in **MONITOR / DRY-RUN ONLY** mode. It validates envelopes and records cross-worker health, but never issues worker dispatches.

---

## 2. Canonical Task Envelope Schema (Dialect B)

Every runnable assignment must be formatted as a single fenced code block tagged ````hg-task` (or ````json:hg-task`):

```json
```hg-task
{
  "source": "issue:26",
  "task": {
    "task_id": "HG-INTEGRATION-CONTRACT-01",
    "source": "issue:26",
    "assigned_to": "hg-antigravity-integration",
    "expected_branch": "dev/production-integration",
    "allowed_scope": ["scripts/orchestration/", "scripts/worker/", "docs/"],
    "objective": "Reconcile task envelope schemas and verify single dispatch ownership",
    "acceptance_tests": [
      "task_guard admits canonical Dialect B",
      "task_guard rejects under-specified Dialect A"
    ],
    "prohibited_actions": [
      "main_write",
      "unattended_execution",
      "live_credential_issuance"
    ],
    "action": "implement"
  }
}
```
```

### Required Fields Specification

| Field | Type | Description | Failure Code if Missing / Invalid |
| :--- | :--- | :--- | :--- |
| `task_id` | `string` | Unique task identifier (e.g., `HG-NET-01`). Must be idempotent. | `MISSING_FIELD: task_id` |
| `source` | `string` | Issue or origin reference (e.g., `issue:14`, `issue:26`). | `MISSING_FIELD: source` |
| `assigned_to` | `string` | Exact worker identity (`hg-hermes-network`, `hg-hermes-android`, `hg-hermes-cpp`, `hg-antigravity-integration`). | `NOT_MY_TASK: assigned to X, worker is Y` |
| `expected_branch` | `string` | Target branch owned by the worker (must match `worker_registry.json`). | `WRONG_BRANCH: expects X, worker owns Y` |
| `allowed_scope` | `list[str]` | List of allowed directory/file prefixes. Widening or cross-domain paths are rejected. | `SCOPE_WIDENING: path outside authorized scope` |
| `objective` | `string` | Precise description of the work to be performed. | `MISSING_FIELD: objective` |
| `acceptance_tests` | `list[str]` | Explicit list of test criteria required for task completion. | `MISSING_FIELD: acceptance_tests` |
| `prohibited_actions` | `list[str]` | Guardrail declarations (e.g. `main_write`, `apk_install`, `oracle_change`). | `MISSING_FIELD: prohibited_actions` |

### Optional Fields

| Field | Type | Description |
| :--- | :--- | :--- |
| `action` | `string` | Action verb (`test`, `implement`, `review`, `report`, `investigate`). Defaults to `test`. |
| `requires_approval` | `list[str]` | Actions requiring owner sign-off. If any match `hard_prohibited`, rejected with `HARD_PROHIBITED`. |
| `approvals` | `dict[str, str]` | Recorded approvals (e.g., `{"live_credential_issuance": "owner"}`). |

---

## 3. Explicit Alias Migration & Legacy Dialect Policy

Older task envelopes utilized a simplified 3-field format (Dialect A):
```json
{
  "mission": "HG-OLD-01",
  "worker": "hg-hermes-cpp",
  "action": "test"
}
```

### Migration Rules

1. **Under-Specified Envelopes are STRICTLY REFUSED:** Any envelope that omits `expected_branch`, `allowed_scope`, `acceptance_tests`, or `prohibited_actions` is rejected with `MISSING_REQUIRED_FIELDS`.
2. **Explicit Alias Resolution:** If an envelope provides all required security fields but uses legacy keys, the guard automatically maps aliases:
   * `mission` $\rightarrow$ `task_id`
   * `worker` $\rightarrow$ `assigned_to`
   * `detail` or `notes` $\rightarrow$ `objective`
3. **Payload Unpacking:** Both wrapped `{ "source": "...", "task": { ... } }` and flat `{ "task_id": ... }` are normalized to the canonical structure.

---

## 4. Worker Scope and Confinement Matrix

| Worker ID | Assigned Issues | Owned Branch | Authorized Scope | Forbidden Scopes (Refused) |
| :--- | :--- | :--- | :--- | :--- |
| `hg-hermes-network` | #14, #23 | `experiment/hg-lan-mtls-07` | `scripts/mtls_fixtures/`, `scripts/worker/`, `docs/HG_LAN_MTLS_07_` | `apk/`, `src/net/`, `oracle_change`, `main_write` |
| `hg-hermes-android` | #12, #24 | `experiment/hg-panasonic-service-transport-06` | `apk/src/`, `apk/jni/`, `apk/tests/`, `apk/build_apk.sh`, `docs/SCHEMA_PRODUCER_V1.md`, `scripts/gen_producer_fixture.py`, `scripts/normalize_apk_zip.py` | `wireguard`, `vpn`, `nsg`, `oracle`, `main_write` |
| `hg-hermes-cpp` | #19, #25 | `experiment/hg-panasonic-cpp-receiver-08` | `src/net/`, `tests/net/`, `docs/HG_PANASONIC_CPP_RECEIVER_08.md`, `workers/hg_worker.py` | `apk/`, `wireguard`, `oracle`, `main_write` |
| `hg-antigravity-integration` | #17, #26 | `dev/production-integration` | `scripts/orchestration/`, `scripts/worker/`, `src/`, `tests/`, `docs/`, `CMakeLists.txt` | `main_write`, `live_credential_issuance` |

---

## 5. Kanban Dispatch Convention

To ensure robust dispatch through the native Hermes Gateway:
1. **Assignee Profile:** Tasks must specify `--assignee default` (the single on-disk Hermes profile). Task identity and remit are preserved in the task body and `--created-by <worker_id>`.
2. **Workspace Worktree:** Tasks must specify `--workspace worktree` with the board's default workdir configured to `/home/swarna-sekhar-dhar/projects/homeguardian-ai`.
3. **Model & Provider Pinning:** Heavy worker runs must explicitly pin `--model deepseek/deepseek-v4-flash --provider commandcode` to prevent quota exhaustion on default tiers.
4. **Idempotency Key:** Kanban tasks must carry `--idempotency-key hg-poller-<task_id>` to guarantee single-execution.
EOF
