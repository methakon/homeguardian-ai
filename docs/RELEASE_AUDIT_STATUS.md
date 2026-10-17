# HomeGuardian AI — Security Audit & Release Candidate Status

**Last Updated:** 2026-10-17  
**Active Working Branch:** `fix/hg-sec-06b-recovery-auth`  

---

## 1. Audit Verdict History

| Candidate SHA | Auditor | Scope | Result | Notes / Deficiencies |
|---|---|---|---|---|
| `d19d230d10baf135c6c22ccadbd988ec5a055598` | Hermes Session 1 | HG-SEC-03 / HG-SEC-04 | **FAIL** | WebRTC keepalive CSRF method mismatch, unhandled device exception crashes. |
| `11ad94548f65bfe22a0db4297a6e92218448d1f4` | Hermes Session 1 | HG-SEC-05 | **FAIL** | Localhost port mismatch CORS vulnerability, simulator forgery, unauthenticated physical device defaults. |
| `52d3d1436e115fe9061127a35f33b35d10c8dd73` | Hermes Session 1 | HG-SEC-05 / HG-SEC-06 | **FAIL (P0-3)** | **P0-1 PASS** (Browser origin exact-port enforcement)<br>**P0-2 PASS** (Simulator identity restricted to authoritative provider)<br>**P0-3 FAIL** (Physical device heartbeat authentication secrets hardcoded in publicly reviewable source, unauthenticated re-pairing, sequence replay vulnerability).<br>*Status: Frozen for audit records.* |
| `05e130395b1d3244bb38202c929902b6e1d93f28` | Hermes Session 1 | HG-SEC-06 Independent Audit | **Gate 0 PASS / HG-SEC-06B REVIEW** | Passed provenance and environment gate. Review identified operator recovery caller JSON boolean bypass. Candidate frozen under audit. |
| Current (on `fix/hg-sec-06b-recovery-auth`) | Antigravity / Independent | HG-SEC-06B Operator Recovery Authorization Review | **READY FOR AUDIT** | Complete remediation: caller JSON boolean rejection, trusted operator secret authentication, reconciliation of 5 historical denylist literals, fail-closed CSPRNG entropy. |

---

## 2. HG-SEC-06 & HG-SEC-06B Remediation Architecture

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
- Re-pairing an already enrolled device strictly requires valid proof of current credential (`HG_DEVICE_SECRET` / `existing_auth_secret`).

### Fix 4 — Heartbeat Replay Protection & Sequence Persistence
- Strictly increasing per-device sequence watermark enforced on every heartbeat.
- Duplicate sequence numbers rejected (code 3).
- Older sequence numbers rejected (code 3).
- Malformed sequence numbers rejected (code 3).
- Watermark persisted in `devices.json` across process restart.
- Offline devices never transition to online upon failed authentication.

### Fix 5 (HG-SEC-06B) — Trusted Operator Recovery Authorization
- **Caller JSON Boolean Rejection:** Rejects any caller attempting to bypass proof by setting `"operator_recovery": true` or `"force_re-pair": true` inside device JSON without trusted operator authorization.
- **Trusted Operator Channel:** Operator recovery strictly requires `--operator-recovery` CLI argument and verified operator credentials (`HG_OPERATOR_SECRET` / `HG_OPERATOR_PASSWORD` / `--operator-secret`). Fails closed if operator secret is unconfigured or invalid.
- **Fail-Closed Offline State:** Operator recovery forces the recovered device offline (`online = false`, `last_heartbeat_ms = 0`), records `recovered by authorized operator` note, and requires an authenticated heartbeat before the device is marked online.
- **CSPRNG Fail-Closed Hardening:** Secret generation strictly requires `/dev/urandom` and fails closed if cryptographic entropy cannot be read, eliminating any silent fallback to pseudo-random generators of unspecified security strength.
- **Reconciliation of Published Literals:** Confirmed that the 5 literals (`sec_cam1_...`, `sec_syn_...`, `sec_mic1_...`, `sec_spk1_...`, `sec_env1_...`) are historical denylist entries solely and are rejected across all authentication paths.

---

## 3. Independent Verification Instructions

To reproduce the security verification suite:
```bash
# 1. Build C++ tools and test suites
cmake --build build --target homeguardian_tests hg_device_registry_cli hg_mcp_server hg_alexa_bridge -j4

# 2. Run full C++ regression test suite (38,926 assertions in 526 test cases)
./build/homeguardian_tests

# 3. Run HG-UI-03 & HG-SEC-06/06B Node & C++ integration suite (144 assertions)
node web_mcp/test_devices_dashboard.js

# 4. Run native MCP streamable HTTP suite (71 assertions)
bash scripts/verify_mcp.sh

# 5. Run independent security gate verification (15 assertions)
bash scripts/test_hg_sec_01_independent.sh
```
