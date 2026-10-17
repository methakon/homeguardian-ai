# HomeGuardian AI — Security Audit & Release Candidate Status

**Last Updated:** 2026-10-17  
**Active Working Branch:** `fix/hg-sec-06-device-secrets`  

---

## 1. Audit Verdict History

| Candidate SHA | Auditor | Scope | Result | Notes / Deficiencies |
|---|---|---|---|---|
| `d19d230d10baf135c6c22ccadbd988ec5a055598` | Hermes Session 1 | HG-SEC-03 / HG-SEC-04 | **FAIL** | WebRTC keepalive CSRF method mismatch, unhandled device exception crashes. |
| `11ad94548f65bfe22a0db4297a6e92218448d1f4` | Hermes Session 1 | HG-SEC-05 | **FAIL** | Localhost port mismatch CORS vulnerability, simulator forgery, unauthenticated physical device defaults. |
| `52d3d1436e115fe9061127a35f33b35d10c8dd73` | Hermes Session 1 | HG-SEC-05 / HG-SEC-06 | **FAIL (P0-3)** | **P0-1 PASS** (Browser origin exact-port enforcement)<br>**P0-2 PASS** (Simulator identity restricted to authoritative provider)<br>**P0-3 FAIL** (Physical device heartbeat authentication secrets hardcoded in publicly reviewable source, unauthenticated re-pairing, sequence replay vulnerability).<br>*Status: Frozen for audit records.* |
| `[PENDING_AUDIT]` (on `fix/hg-sec-06-device-secrets`) | Hermes Session 1 | HG-SEC-06 Comprehensive Remediation | **AWAITING VERDICT** | Complete physical device authentication hardening, CSPRNG secret provisioning, POSIX 0700/0600 storage protection, authenticated re-pairing, sequence replay protection across restarts. |

---

## 2. HG-SEC-06 Remediation Architecture

### Fix 1 — Elimination of Published Secrets & CSPRNG Credential Generation
- All hardcoded physical-device secrets (`sec_cam1_...`, etc.) removed from C++ source files and default inventory (`config/devices.json`).
- Physical devices in default inventory initialize as candidate devices without pre-planted credentials (`paired: true`, `auth_secret: ""`, `credential_id: ""`).
- Explicit denylist (`is_blacklisted_secret`) rejects all 18 previously published/known secrets, cert CNs, and weak test tokens.
- Cryptographically secure 256-bit secrets (`sec_live_<64-hex>`) minted via `/dev/urandom` during authorized pairing.

### Fix 2 — Protected Credential Storage
- POSIX directory permissions enforced at `0700` (`S_IRWXU`).
- POSIX registry file permissions enforced at `0600` (`S_IRUSR | S_IWUSR`) on both temp file creation (`::open` with atomic `0600` flags) and post-atomic-rename.
- Compatible with Android internal app storage models.

### Fix 3 — Authenticated Pairing and Re-Pairing
- Client-supplied `auth_secret` during pairing is strictly rejected (code 2); host CSPRNG provisions all credentials.
- Initial enrollment requires no pre-existing secret (unpaired bootstrap).
- Re-pairing an already enrolled device requires either proof of current credential (`HG_DEVICE_SECRET` / `existing_auth_secret`) or explicit operator recovery authorization (`operator_recovery: true`).

### Fix 4 — Heartbeat Replay Protection & Sequence Persistence
- Strictly increasing per-device sequence watermark enforced on every heartbeat.
- Duplicate sequence numbers rejected (code 3).
- Older sequence numbers rejected (code 3).
- Malformed sequence numbers rejected (code 3).
- Watermark persisted in `devices.json` across process restart.
- Offline devices never transition to online upon failed authentication.

---

## 3. Independent Verification Instructions

To reproduce the security verification suite:
```bash
# 1. Build C++ tools and test suites
cmake --build build --target homeguardian_tests hg_device_registry_cli hg_mcp_server hg_alexa_bridge -j4

# 2. Run full C++ regression test suite (38,926 assertions in 526 test cases)
./build/homeguardian_tests

# 3. Run HG-UI-03 & HG-SEC-06 Node & C++ integration suite (132 assertions)
node web_mcp/test_devices_dashboard.js

# 4. Run native MCP streamable HTTP suite (71 assertions)
bash scripts/verify_mcp.sh

# 5. Run independent security gate verification (15 assertions)
bash scripts/test_hg_sec_01_independent.sh
```
