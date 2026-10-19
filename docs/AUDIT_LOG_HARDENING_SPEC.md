# Action Audit Log Hardening Specification
**Specification Version:** `hg-audit-hardening/1.0.0`  
**Status:** Approved & Verified (Roadmap R2)  
**Authors:** Antigravity Engineering  
**Scope:** Cryptographic verification, threat model of local hash-chaining, external checkpoint witness architecture.

---

## 1. Threat Model & Theoretical Limits of Local SHA-256 Chaining

In the initial R1 architecture, `ActionAuditLog` implemented an in-process, append-only ledger chained via SHA-256 hashes:
$$\text{hash}_n = \text{SHA-256}(\text{seq}_n \parallel \text{timestamp}_n \parallel \text{request\_id}_n \parallel \text{event\_name}_n \parallel \text{state}_n \parallel \text{reason}_n \parallel \text{hash}_{n-1})$$

### Vulnerabilities Inherent to Purely Local Chaining
1. **Local State Replacement / History Rewriting:**
   An adversary with host file or memory access who can alter an audit entry at sequence $k$ can recompute all subsequent hashes $k, k+1, \dots, N$ from that point forward. Because the hash algorithm is deterministic and holds no external secrets, an internally valid chain is recreated.
2. **Trailing Truncation:**
   An attacker who wishes to conceal recent actions can truncate the log from entry $k$ to $N$. The remaining prefix $1 \dots k-1$ remains internally consistent and passes simple sequential hash checks.
3. **Internal Consistency Verification:**
   Purely local verification (`verify_chain_integrity()`) proves that the log has not suffered casual payload corruption, but *cannot* prove against an attacker who has rewritten or truncated the ledger without an external source of truth.

---

## 2. Hardening Architecture: Protected Checkpoints & External Anchors

To mitigate the limitations above, Roadmap R2 introduces **Protected Checkpoints** and the **External Anchor Abstraction**:

```
+-----------------------------------------------------------------------------------------+
|                               HomeGuardian Host                                         |
|                                                                                         |
|   +---------------------------------------------------------------------------------+   |
|   | ActionAuditLog (Local Append-Only Ledger)                                       |   |
|   |   Entry #1 -> Entry #2 -> ... -> Entry #N (Tip Hash: 0x8a3f...)                |   |
|   +---------------------------------------------------------------------------------+   |
|                                    |                                                    |
|                                    v [create_checkpoint()]                              |
|                       +---------------------------+                                     |
|                       | AuditCheckpoint           |                                     |
|                       |   seq: N                  |                                     |
|                       |   tip_hash: 0x8a3f...     |                                     |
|                       |   sig: 0x51c2...          |                                     |
|                       +---------------------------+                                     |
|                                    |                                                    |
+------------------------------------|----------------------------------------------------+
                                     | [anchor_to(anchor)]
                                     v
                 +---------------------------------------+
                 | IExternalCheckpointAnchor             |
                 | (Remote Witness / TPM / WORM Storage) |
                 +---------------------------------------+
```

### 2.1 AuditCheckpoint Data Contract
```json
{
  "checkpoint_id": "cp-42-8a3f120e",
  "sequence_num": 42,
  "timestamp_ms": 1729384910243,
  "tip_hash": "8a3f120e98c764a...",
  "anchor_id": "remote-witness-01",
  "anchor_signature": "51c2f90ab81..."
}
```

### 2.2 Verification Against Anchored Checkpoints
When auditing system history, `ActionAuditLog::verify_against_checkpoint(const AuditCheckpoint& checkpoint)` executes a three-stage defense:

1. **Truncation Detection:**
   ```cpp
   if (entries_.size() < checkpoint.sequence_num) {
       return {CheckpointVerificationStatus::LogTruncated, false, "Log truncated"};
   }
   ```
2. **Payload Modification / Broken Link Detection:**
   Iterates through all entries from $i = 0$ to $\text{checkpoint.sequence\_num}-1$, validating that:
   - `entry.prev_hash == expected_prev`
   - `compute_hash(...) == entry.hash`
   If any discrepancy is found, returns `CheckpointVerificationStatus::LogModified`.
3. **Unauthorized History Fork / Replacement Detection:**
   Compares the entry hash at the checkpoint boundary with the signed tip:
   ```cpp
   if (entries_[checkpoint.sequence_num - 1].hash != checkpoint.tip_hash) {
       return {CheckpointVerificationStatus::UnauthorizedForkOrReplacement, false, "Fork detected"};
   }
   ```
   If all three checks pass, status is `CheckpointVerificationStatus::Valid`.

---

## 3. Formal Attack Verification Suite

The hardening implementation is validated in `tests/action/test_alert_action_integration.cpp`:

| Attack Scenario | Injected Fault | Detected Status | Result |
| :--- | :--- | :--- | :--- |
| **Attack 1: Payload Tampering** | Mutate `reason` field in entry #2 from `"Normal"` to `"Covert Bypass"` | `LogModified` | Blocked & Logged |
| **Attack 2: Chain Truncation** | Delete last 2 entries from local storage | `LogTruncated` | Blocked & Logged |
| **Attack 3: Fork / History Rewriting** | Replace entire chain with synthetic re-hashed history | `UnauthorizedForkOrReplacement` | Blocked & Logged |

All attacks are detected deterministically and fail closed.
