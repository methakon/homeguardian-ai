# Security Readiness v6 — Authentication & Secret Management (F22-32)

**Date:** 2026-10-12
**Scope:** Implement security blockers **B1 (real credential verification)** and
**B4 (secret management)** from `docs/SECURITY_READINESS_V5.md`, as an isolated
milestone, before any HTTP endpoint.
**Verified baseline:** commit `6025910` on `origin/main`, clean working tree.
**Mode:** Simulator-only. No endpoint deployed, no cloud, no production
credentials, no physical sensors.

---

## 1. Identity-flow decision (Section 1 requirement)

The repository documents the intended production flow as **Alexa custom skill →
authenticated HTTPS endpoint → C++ service, with account linking via OAuth 2.0 /
Login with Amazon (LWA)** (`docs/ARCHITECTURE.md`, "Alexa+ integration
findings"). Validating a real LWA access token requires an LWA client id/secret
(Amazon credentials), which this milestone does not have and must not fabricate.

**Decision:** the exact production identity provider (LWA) is NOT implemented —
it cannot be without Amazon credentials, which are prohibited here. Instead this
milestone implements the **provider-independent verification/security layer**:

- A hardened `AlexaCredentialStore` that verifies opaque bearer tokens with
  expiry, revocation, secure-file enforcement, constant-time comparison, and
  fail-closed behavior.
- A narrow verifier interface (`IAlexaCredentialVerifier`, unchanged) so a
  future LWA/signed-request verifier slots in at the wiring point **without
  changing `AlexaContract` semantics**.

Authentication and authorization remain **separate**: the store authenticates
(returns a principal + permissions); the contract authorizes per operation.

---

## 2. What was implemented

### `AlexaCredentialStore` (`src/core/AlexaCredentialStore.{h,cpp}`)
A real bearer-token verifier + secure secret loader.

**Verification (B1):**
- Validates an opaque token against a provisioned table; returns a principal
  and per-operation permissions only on success.
- **Fail-closed** on: missing/unreadable/malformed/insecurely-permissioned file,
  unknown token, **expired** token, **revoked** token. All failure modes are
  indistinguishable to the caller (no validity-detail leakage).
- **Constant-time comparison** over token bytes (accumulate XOR across all
  bytes; no early exit on first mismatch) so timing does not reveal match
  position. Length mismatch fails immediately (length is not secret).
- **No custom crypto / no new dependency:** the milestone prohibits both, and
  OpenSSL dev headers are absent on this host. Constant-time byte equality is
  implemented directly (documented, not a cryptographic hash).
- Runtime in-memory `revoke()` for operational control; persistent revocation is
  a provisioning procedure (see §3).
- `add_for_testing()` for synthetic fixtures (bypasses the secure-file path;
  never for production secrets).

**Secret handling (B4):**
- The credential file must be a **regular file, owner-only (0600), owned by the
  current effective uid**. Any group/other bit, a foreign owner, a non-regular
  file, or unreadable/malformed content → **load fails, store stays empty, every
  verify unauthenticated** (fail closed).
- Secret values are **never logged, never in error messages, never returned**.
  The load-failure reason is generic (no path, no value).
- **No permissive fallback:** if the secure store fails to load, the bridge
  exits (code 3) rather than serving with a weaker verifier.

### Bridge wiring (`src/bridge/hg_alexa_bridge.cpp`)
- `HG_BRIDGE_SECURE_CREDS=1` → use `AlexaCredentialStore` (secure file enforced;
  load failure → exit 3, no serve). This is the production-style path.
- Default → the existing local `FileCredentialVerifier` (no file-permission
  enforcement) for development/tests whose fixtures are not 0600.
- Both use the same JSON schema; the mode is chosen **explicitly** (env var),
  never silently. The local stdio bridge is preserved for regression testing.

---

## 3. Credential lifecycle — real vs operational

| Operation | Status |
|---|---|
| Verify token (valid/expired/revoked/unknown) | **REAL** — implemented + tested |
| Constant-time comparison | **REAL** — implemented + tested |
| Secure file load (0600, owner, regular, parse) | **REAL** — implemented + tested |
| Fail-closed on any load/verify failure | **REAL** — implemented + tested |
| No secret in logs/errors/responses | **REAL** — implemented + tested |
| Runtime in-memory revocation | **REAL** — implemented + tested |
| Persistent revocation / rotation | **OPERATIONAL PROCEDURE** — re-provision the file out-of-band (documented); not a runtime feature |
| Secret generation / issuance | **OPERATIONAL PROCEDURE** — an admin generates tokens out-of-band; the app only verifies |
| LWA / OAuth token validation | **NOT IMPLEMENTED** — needs Amazon credentials (prohibited); a future verifier implements `IAlexaCredentialVerifier` |

**How local tests differ from production:** tests use synthetic tokens in
isolated temp files (0600 in secure-mode tests) that are created and removed
per-run. Production secrets live outside Git and ordinary config, at a
runtime-supplied path, 0600 owner-only. No test asserts a real identity
provider.

---

## 4. Remaining prerequisites (from v5) — still open

- **B2 TLS termination** — not implemented (no HTTP service).
- **B3 rate limiting / concurrency control** — not implemented.
- **B5 health endpoint / supervision** — not implemented.
- Real identity provider (LWA) integration — needs Amazon credentials.
- Real disk-full, real hardware capture, live Alexa integration — unverified.

**Is the next (local-only HTTP-adapter) stage ready?** **CONDITIONAL.** The
authentication/secret layer (B1, B4) is now real and tested, so an HTTP adapter
can be built that reuses `AlexaContract` + `AlexaCredentialStore` behind a
loopback-only listener. It remains **blocked on B2/B3/B5** before any non-local
exposure, and the adapter must not be described as production-auth until a real
identity provider replaces the local token store.

---

## 5. Exact verification results

| Check | Command | Result |
|---|---|---|
| C++ full host suite | `hg_build/homeguardian_tests` | **255 cases / 2004 assertions pass** |
| CTest | `ctest --test-dir hg_build` | **100% (1/1)** |
| ASan+UBSan | `hg_asan/homeguardian_tests` (cmake reconfigure, stale objects removed, clean recompile, sizes verified) | **255 cases, zero sanitizer errors** |
| Node full | `cd alexa_skill && npm test` | **50/50** (13 handler + 12 contract + 21 integration + 4 restart) |
| Secure-mode integration | `integration.test.js` | 5 new cases: 0600 auth, revoked→unauthorized, expired→unauthorized, 0644→exit 3 no-serve, no token in stderr |
| Creds unit group | `homeguardian_tests "[creds]"` | **14 cases / 44 assertions** |
| Fresh release build | out-of-source, existing dep cache | app + tests + bridge clean; suite **255/2004**; `[creds]` present in fresh binary |
| Secret/artifact scan | `git grep` + `git ls-files` | clean; no secrets/keys/db/binaries tracked |

---

## 6. Boundaries confirmed

No public/network endpoint; no cloud/deployment/production credentials; no repo
visibility change; no force-push; no physical sensor activation; no device
permission changes; no Trading Agent / my-job-agent changes; no new dependency;
no custom crypto; no global Git/config changes. Android/Linux backends,
simulator, consent enforcement, and the Alexa skill preserved. Build directories
(`hg_build`, `hg_asan`, `build`, `build_final`) intact; temp release dir removed;
no stray credential/db files left in the repo.
