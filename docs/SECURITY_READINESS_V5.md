# Security Readiness Review — v5 (F22-31)

**Date:** 2026-10-11
**Scope:** Deployment-readiness and security review of the read-only Alexa
integration boundary (`AlexaContract` + `hg_alexa_bridge`), persistence, and
privacy guarantees, building on Runtime Readiness v4.
**Verified baseline:** commit `17b6afc` on `origin/main`, clean working tree.
**Mode:** Simulator-only. No endpoint deployed, no cloud resources, no physical
sensors activated.

> **The local stdio bridge is NOT a deployed HTTPS service.** This review
> determines what a future authenticated HTTPS endpoint would require; it does
> not deploy one.

---

## 1. Threat model and trust boundaries

**Assets:** application lifecycle status, aggregate alert summaries, the alert
audit database, and (prospective) credentials.

**Trust boundaries:**

1. **Untrusted client → bridge.** Any process that can write to the bridge's
   stdin is a potential attacker. The bridge treats every request as
   untrusted: it parses with a strict, bounded schema and authorizes via a
   verifier — it never trusts client-supplied identity or permission claims.
2. **Bridge → contract → data.** The bridge calls only the read-only
   `AlexaContract` (`status`, `alert_summary`). There is no code path to enable
   capture, grant consent, change configuration, or activate a sensor.
3. **Credential store → verifier.** The verifier is the sole authority on
   authorization. Credentials are test-only local fixtures, never real secrets.

**Out of scope (not implemented, not claimed):** network transport, TLS, a
deployed endpoint, real credential issuance/rotation, and rate limiting. These
are proposed in Section 6, not implemented.

---

## 2. Findings ranked by severity

### LOW-1 — Bridge ignored its config argument (FIXED)
**Area:** B (request/process boundaries) / correctness.
**Evidence:** `hg_alexa_bridge.cpp` read `config_path` from `argv[1]` but never
loaded it; `capture_enabled` always reflected the `RuntimeStatus` default
(`false`), even when the config enabled capture. Verified by running the bridge
with a capture-on config: it reported `capture_enabled=false`.
**Disposition:** FIXED. The bridge now loads the real config and reflects the
actual capture kill-switch state; a config load failure fails safe (capture
reported disabled, bridge keeps serving). This is a read-only *reflection* — the
bridge still cannot enable capture.
**Regression tests:** `integration.test.js` "status reflects the real config
capture kill-switch (read-only)" and "config load failure fails safe".

### INFO-1 — stdio boundary has no transport authn/authz (BY DESIGN, documented)
**Area:** A (authentication).
The local bridge authenticates via a test-only file credential table. There is
no TLS, no network listener, and no real credential lifecycle. This is correct
for a local, single-client, process-spawned boundary, but it is NOT sufficient
for a network deployment. Documented as a deployment blocker (Section 6).

### INFO-2 — No rate limiting / concurrency limit (BY DESIGN, documented)
**Area:** B. The bridge is a single-client stdio process with a bounded request
lifetime (`kMaxRequests`) and per-request size bounds. A network service would
need explicit rate limiting and concurrency caps. Documented as a proposed
control.

---

## 3. Security controls VERIFIED (with evidence)

| Control | Status | Evidence |
|---|---|---|
| Fail-closed auth: missing/invalid/expired/unknown credential → `unauthorized` | VERIFIED | `[alexa]` C++ cases + `integration.test.js`; `FileCredentialVerifier::verify` returns unauthenticated on any failure; missing/unreadable table fails closed |
| Authenticated-but-unauthorized → `forbidden` | VERIFIED | `[alexa]` "authenticated but unauthorized operation is forbidden"; tok-status denied alert_summary |
| Client-supplied identity/permission claims ignored | VERIFIED | `[alexa]` "client-supplied identity/authorization claims are ignored" — unknown fields → malformed; no-perm principal → forbidden |
| Read-only: no mutating operation possible | VERIFIED | `[alexa]` "contract exposes no mutating operation" — enable_capture/grant_consent/set_config/activate_sensor all malformed |
| Request size bound (oversized → malformed) | VERIFIED | `[alexa]` oversized + `integration.test.js` oversized (5000-byte cred) |
| Non-object / trailing-data / wrong-type JSON rejected | VERIFIED | new `[alexa][safety]` cases |
| Nesting bomb under size bound rejected, no crash/hang | VERIFIED | manual: 1500-deep nested JSON within 4KB → malformed, exit 0; `[alexa]` case |
| Partial NDJSON line (no trailing newline) handled | VERIFIED | manual: complete JSON without newline → processed (ok); truncated line → malformed |
| No credential leak in output/errors | VERIFIED | manual: secret token value 0 occurrences in stdout/stderr; `[alexa]` "credential value never appears in the response" |
| No network listener | VERIFIED | bridge is stdin/stdout only; code review confirms no socket/bind; no port opened |
| No shell execution / injection | VERIFIED | Node client spawns with argument ARRAY (no shell); bridge uses no `system()`/`exec` |
| Process lifetime bounded; clean exit on EOF/shutdown/bad-usage | VERIFIED | `integration.test.js` clean exit; `restart.test.js` bad-usage exit 2; `kMaxRequests` bound |
| Restart does not restore consent or enable capture | VERIFIED | `[privacy]` restart case; `restart.test.js` fresh bridge reports zero; capture per-process |
| Persistence: historical ≠ current-process alerts | VERIFIED | `[audit]` historical-vs-current; `restart.test.js` historical alert visible to fresh bridge |
| Responses expose summaries only (no raw media/PII) | VERIFIED | `[alexa]` aggregate-only; `[privacy]` metadata-only fields |
| Build hygiene: `-Wall -Wextra -Wpedantic`, C++20, Release default | VERIFIED | `CMakeLists.txt`; clean build, no warnings |
| Supply chain: no install hooks; pinned deps | VERIFIED | `alexa_skill/package.json` — no pre/postinstall; `ask-sdk-core ^2.14.0` |
| No committed secrets/keys/db/binaries | VERIFIED | secret scan clean; `git ls-files` has no `.db/.pem/.key/.so/.apk`; node_modules untracked |
| Config load failure fails safe | VERIFIED | `integration.test.js` "config load failure fails safe" |

---

## 4. Security controls NOT verified / unverified

- **TLS / HTTPS termination** — not implemented (no network service exists).
- **Real credential issuance, rotation, revocation, storage-permissions** — the
  file verifier is test-only; no production secret store, no `chmod 0600` on a
  credential file, no rotation.
- **Rate limiting, concurrency caps, per-connection quotas** — not implemented
  (single-client stdio).
- **Real filesystem exhaustion / disk-full behavior** — write failure is
  simulated via a closed DB, distinct from real disk exhaustion.
- **Real V4L2/ALSA/PipeWire capture and OS permission semantics** —
  hardware-gated, not tested.
- **Live Alexa-to-application integration** — not deployed/tested.

---

## 5. Exact verification results

| Check | Command | Result |
|---|---|---|
| C++ full host suite | `hg_build/homeguardian_tests` | **241 cases / 1960 assertions pass** |
| CTest | `ctest --test-dir hg_build` | **100% (1/1)** |
| ASan+UBSan | `hg_asan/homeguardian_tests` (stale objects removed, clean recompile, sizes verified) | **241 cases pass, zero sanitizer errors** |
| Node (all) | `cd alexa_skill && npm test` | **45/45** (13 handler + 12 contract + 16 integration + 4 restart) |
| Fresh release build | out-of-source, existing dep cache | app + tests + bridge build clean; suite **241/1960**; new `[alexa]` group present |
| Focused alexa group | `homeguardian_tests "[alexa]"` | **20 cases / 102 assertions** |
| Secret/artifact scan | `git grep` + `git ls-files` | clean; no secrets/keys/db/binaries tracked |

**Fixes landed this cycle:** LOW-1 (config reflection) + 6 new C++ `[alexa]`
security-regression cases + 2 new Node integration cases.

---

## 6. Proposed future HTTPS deployment boundary (NOT implemented)

Distinguishing **implemented** from **proposed**:

**Implemented (local stdio bridge):** strict bounded request schema, version
gating, fail-closed authentication, read-only operations, credential-value
non-leakage, no network listener, no shell execution, bounded process lifetime,
fail-safe config handling, restart-safe (no consent/capture restoration).

**Proposed (a future authenticated HTTPS endpoint would additionally require):**

1. **HTTPS termination & certificates:** TLS 1.2+ terminated at a reverse proxy
   or in-process; certificates via an ACME client or a managed cert; HSTS.
2. **Authentication/authorization:** replace the test file verifier with a real
   verifier (e.g. Alexa account-linking token or signed-request validation).
   Authorization must remain fail-closed and server-side; never trust
   client-supplied identity. Per-operation permission checks preserved.
3. **Secret provisioning & rotation:** credentials from a secret manager (never
   in source/git/env-in-logs); owner-only file permissions (`0600`) if a file is
   used; automated rotation and revocation.
4. **Request limits & rate limiting:** body size cap (already ≤4KB at the
   contract), per-IP/per-token rate limits, concurrency caps, request timeouts.
5. **Privacy-safe logging:** structured, redacted logs (no credentials, tokens,
   media, or PII); the existing generic error messages must be preserved.
6. **Health/readiness & degraded mode:** a `/healthz` reflecting persistence
   health; degraded mode returns `unavailable` (already the contract behavior).
7. **Process supervision & graceful shutdown:** a supervisor (systemd/pm2) with
   restart policy; the bridge already exits cleanly on EOF/shutdown/SIGTERM.
8. **Network minimization:** bind loopback or a private interface only; firewall
   to the Alexa service's egress IPs; no public listener.
9. **Rollback/recovery:** immutable deploy artifacts, previous-version rollback,
   DB backup/restore runbook (migrations are additive and roll back cleanly).
10. **Separation:** the local stdio bridge stays a separate, non-network
    artifact; the network service is a distinct deployment that reuses the same
    `AlexaContract` domain logic behind an HTTP adapter.

**Deployment blockers (must be resolved before any HTTPS endpoint):**
- B1: No real authentication mechanism (test verifier only).
- B2: No TLS termination.
- B3: No rate limiting / concurrency control.
- B4: No secret management / rotation.
- B5: No health endpoint or supervision config.

---

## 7. Go / No-Go recommendation

**For a future authenticated HTTPS endpoint: CONDITIONAL GO — blocked pending
B1–B5.**

The **domain contract** (`AlexaContract`) is deployment-ready: its validation,
versioning, fail-closed authorization, read-only guarantee, and non-leaking
errors are verified by regression tests and would sit unchanged behind an HTTP
adapter. The **transport, authentication, and operational controls** (B1–B5) do
not exist and must be implemented and tested before any network exposure.

The **local stdio bridge** is safe for its intended purpose (a local,
single-client, process-spawned boundary driven by tests and the demo) and must
NOT be exposed to a network.

---

## 8. Remaining risks and next milestone

- Implement a real credential verifier + secret management (B1, B4).
- Add an HTTP adapter + TLS termination + rate limiting (B2, B3, B5), each with
  regression tests, still without deploying publicly.
- Then a staged deployment with monitoring, health checks, and a rollback plan.

**No physical sensor, cloud resource, endpoint, or credential was used or
created in this review.**
