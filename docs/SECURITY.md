# Security – Approval Store

## Purpose
The **approval store** provides *independent* verification of any privileged action that a task requests.

- It lives outside the task payload, so a malicious author cannot forge a record.
- Each entry is a JSON object that can contain optional metadata:
  - `approved` (bool) – must be true.
  - `expires_at` (ISO‑8601 timestamp) – after this time the approval is invalid.
  - `used` (bool) – set to true once the approval has been consumed, preventing replay attacks.

## File location
`scripts/worker/approvals_store.json` sits alongside the guard implementation and is loaded on each validation.

## Record format
```
"<approver>:<action>:<id>": {"approved": true, "expires_at": "2026-12-31T23:59:59", "used": false}
```
- `<approver>` must be listed in `identity.json` under `approvers`.
- `<action>` must match a prohibited action declared in the task.
- `<id>` is an opaque identifier (e.g. a UUID or issue reference).

## Guard checks (in `task_guard.validate`)
1. **Author existence** – task must include an `author` field.
2. **Trusted author** – author must be in the `approvers` list.
3. **Self‑approval** – the author cannot approve their own action.
4. **Store lookup** – the exact record string must exist in the store and have `approved: true`.
5. **Expiration** – if `expires_at` is present and in the past, the guard raises `APPROVAL_EXPIRED`.
6. **Replay protection** – if `used` is true, the guard raises `APPROVAL_REPLAYED`.
7. **Mark used** – on successful validation the guard writes back `used: true`.

## Updating the store
The store is a simple JSON file; adding a new approval is a matter of inserting a new key with the object described above.  The guard will persist the `used` flag automatically.

## Auditing
Because approvals are stored centrally, auditors can inspect the file to see which approvals have been consumed, which have expired, and whether any unauthorized approvals were attempted.

---
*Generated as part of Issue #26 – HG‑AG‑P0‑SEC.*