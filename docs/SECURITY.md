# Security – Approval Store & Authentication

## Purpose
The **approval store** provides *independent* verification of any privileged action that a task requests.

- It lives outside the task payload (in a local SQLite database), so a malicious author cannot forge a record.
- Each entry is an atomic, single-use approval record with expiration, revocation, and replay protection.

## Authentication Requirement for Protected Operations

> **Any task requesting a protected operation MUST be dispatched with an
> authenticated `TrustedDispatchContext`.** A plain `task.author` field alone
> is **never** sufficient to authorize privileged execution.

Protected operations are defined in `PROTECTED_OPERATIONS` within
`scripts/worker/task_guard.py` and include actions such as `oracle_change`,
`handset_change`, `router_change`, `firewall_change`, `production_deploy`,
`live_credential_issuance`, `pki_rotation`, `apk_install`, `device_pairing`,
`sensor_activation`, `camera_activation`, `mic_activation`, and
`cross_worker_edit`.

### Why `task.author` is not enough

Prior to the HG-AG-P0-DISP-FIX commit, the dispatcher would construct a
`TrustedDispatchContext` from `task.author` alone using a `test_fixture`
auth method. This allowed any forged `task.author` field to escalate to
privileged execution without real authentication.

The fix enforces that:

1. If a task requests a protected operation and **no dispatch context** is
   supplied, the guard raises `AUTHENTICATION_REQUIRED`.
2. If a dispatch context is supplied but the **author does not match** the
   authenticated actor, the guard raises `ACTOR_MISMATCH`.
3. The `test_fixture` auth method is **intentionally excluded** from
   `VALID_AUTH_METHODS` — tests must supply a genuine auth context.

### Accepted authentication methods

Only these `auth_method` values are recognized:

- `unix_socket_peercred`
- `mtls_san`
- `local_cli_authenticated`
- `github_issue_comment_verified_author`

## File location
The SQLite approval ledger defaults to `~/.hermes/cache/scratch/hg-worker/approvals.db`
and is loaded on each validation. During test runs, a temporary file-based DB is used.

## Record format (SQLite)
Each approval row contains:

| Column | Description |
|--------|-------------|
| `approval_id` | Primary key (e.g. `owner:oracle_change:approval-1`) |
| `actor_id` | Actor the approval applies to (`*` = any) |
| `approved_by` | Who issued the approval (must be in `identity.json` `approvers`) |
| `operation_kind` | The protected operation being approved |
| `target_resource` | Scoped resource (`*` = any) |
| `canonical_request_digest` | SHA-256 digest for tamper detection (optional) |
| `worker_id` | Worker scope (`*` = any) |
| `not_before` | Approval is invalid before this timestamp |
| `expires_at` | Approval is invalid after this timestamp |
| `max_uses` | Maximum number of times the approval can be consumed |
| `use_count` | Current consumption count |
| `used_at` | Timestamp of last consumption |
| `revoked_at` | Revocation timestamp (if revoked) |

## Guard checks (in `task_guard.validate`)
1. **Required fields** – task must include all fields in `REQUIRED_FIELDS`.
2. **Author existence** – if `author` key is present, its value must not be `None`.
3. **Trusted author** – author must be in the `approvers` list or a known identity.
4. **Authentication context** – protected operations require a `TrustedDispatchContext`.
5. **Actor match** – when both author and context are present, they must agree.
6. **Self-approval** – the authenticated actor cannot approve their own action.
7. **Unauthorized approver** – the `approved_by` field must be in `approvers`.
8. **Store lookup** – the exact `approval_id` must exist in the SQLite store.
9. **Not-yet-valid** – if `not_before` is in the future, raises `APPROVAL_NOT_YET_VALID`.
10. **Expiration** – if `expires_at` is in the past, raises `APPROVAL_EXPIRED`.
11. **Revocation** – if `revoked_at` is set, raises `APPROVAL_REVOKED`.
12. **Replay protection** – if `use_count >= max_uses`, raises `APPROVAL_REPLAYED`.
13. **Operation match** – approval `operation_kind` must match the requested operation.
14. **Target match** – approval `target_resource` must match (or be `*`).
15. **Worker match** – approval `worker_id` must match (or be `*`).
16. **Digest match** – if `canonical_request_digest` is set, it must match the operation digest.
17. **Atomic consumption** – on success, `use_count` is incremented and `used_at` recorded atomically via `BEGIN IMMEDIATE`.

## Hard-prohibited operations
The following operations can **never** be authorized, regardless of approvals:

- `unattended_execution`
- `main_write`
- `gateway_change`
- `second_watcher`
- `second_database`

## Auditing
Because approvals are stored in a durable SQLite database, auditors can query
the table to see which approvals have been consumed, which have expired, which
were revoked, and whether any unauthorized approval attempts were made.

---
*Updated as part of Issue #26 – HG-AG-P0-DISP-FIX: authentication requirement for protected operations.*
