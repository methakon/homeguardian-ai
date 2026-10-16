# HomeGuardian AI — Security Readiness v8 (H-04)

**Scope:** roadmap H-04 ("Security review — input validation, injection
prevention, access control") applied to the new **native C++ HTTP demo
transport** (`src/bridge/NativeHttp.h`) and its integration.
**Date:** 2026-10-13 · **Branch:** `h04/native-http-low-footprint`

> **What this is:** a loopback-only, GET-only, **simulator-only** local demo
> interface with a limited HTTP/1.1 subset. **What this is NOT:** a production or
> public-network HTTP service. Handwritten parsing is acceptable here *because*
> the surface is tiny, the transport is loopback-only, and it is explicitly a
> demo — but it is **not** a substitute for a reviewed HTTP stack. See
> "Simulator-only vs production-ready" below.

## 1. Input validation and malformed-request handling

`dispatch()` is a strict, allowlist-driven state machine. Every request must
satisfy all of the following or it is rejected with a generic JSON error (no
internal detail):

| Check | Behaviour | Test |
|---|---|---|
| Request line framing `METHOD SP TARGET SP HTTP/1.1` | malformed → 400 | `[nativehttp]` |
| Exact `HTTP/1.1` version (no 1.0) | otherwise 400 | ✔ |
| Method allowlist: **GET only** | others → 405 (handler never runs) | ✔ |
| Path: non-empty, ≤128 bytes, leading `/`, no `%` `\` `#` NUL | else 400 | ✔ |
| Absolute-form target (`GET http://…`) | 400 | ✔ |
| Exactly one `Host`, value must be a **loopback authority** | missing/duplicate/non-loopback → 400/403 | ✔ |
| `Origin`, if present, must be a **loopback origin** | else 403 | ✔ |
| Reject `Content-Length`, `Transfer-Encoding`, `Authorization`, `Expect` | 400 (no bodies, no smuggling, no auth on this interface) | ✔ |
| Header lines well-formed, no empty name, no ob-fold | 400 | ✔ |
| No body: request size must equal `header_end + 4` | else 400 | ✔ |
| `cycles` query: digits only, ≤3 chars, ≤100 | else 400 | ✔ |

Headers are matched case-insensitively; only the `Host`/`Origin` values are
inspected. The handler is invoked **only** for `/api/status` and
`/api/run-scenario` after every check passes; tests assert the handler's call
count for every rejection path (i.e. rejection = handler never reached).

## 2. Injection prevention

- **No shell, no `exec`, no path interpolation.** The transport makes no
  filesystem calls per request; the dashboard is **embedded in the binary** at
  build time. There is therefore no request-controlled path → no path traversal
  surface (traversal inputs are rejected at validation and, being unknown routes,
  would 404 regardless).
- **No SQL from requests.** The only DB access is the existing
  `PersistenceAudit` (fixed statements, no request-derived SQL).
- **No response injection.** The dashboard is a fixed embedded asset; API bodies
  are produced by `nlohmann::json` serialization (escaping handled) and the
  contract layer. No user string is concatenated into a response header.
- **No credential echo.** Auth/token handling is unchanged from the existing
  bridge; the HTTP interface itself requires no `Authorization` header
  (header rejected outright) and never reads or logs one. Tokens are never
  placed in responses, headers, or errors.

## 3. Access-control matrix

| Route | Method(s) | Auth | Effect | Notes |
|---|---|---|---|---|
| `/` , `/index.html` | GET | none (loopback) | Serve embedded dashboard | read-only static asset |
| `/api/health` | GET | none (loopback) | Static `{ok,up,simulated}` | reveals no state |
| `/api/status` | GET | demo token (contract) | Read lifecycle status + alert summary | read-only |
| `/api/run-scenario` | GET | demo token (contract) | Run the synthetic simulator scenario | the ONLY state-advancing route; bounded to ≤100 cycles |
| anything else | GET | n/a | 404 | |

- **Loopback-only bind** to `INADDR_LOOPBACK` (127.0.0.1); a non-loopback bind is
  impossible by construction (not merely a default).
- **Cross-site protection** for the state-advancing route: a non-loopback
  `Origin` is rejected (403). Combined with the loopback `Host` requirement this
  defends against DNS-rebinding and cross-site fire-and-forget requests.
- **No mutating operations** are exposed: the contract has only read operations
  (`status`, `alert_summary`) plus the bounded scenario runner; it can never
  enable capture, grant consent, change config, or touch sensors.
- The handler runs on the caller thread; requests are processed **one at a
  time** (no concurrency), so there is no shared-state race in the request path.

## 4. Resource limits and fail-closed behaviour

| Limit | Value | Notes |
|---|---|---|
| Max request (headers) bytes | 8192 | exceeded → 413 |
| Max path bytes | 128 | exceeded → 400 |
| Client recv/send timeout | 3 s | slow clients cannot pin a slot |
| Listener accept timeout | 1 s | enables prompt clean shutdown |
| `cycles` | 0..100 | server-side clamp; invalid → 400 |
| Concurrency | 1 at a time | `listen` backlog 8 |
| Audit log | bounded ring (256) | cannot grow without limit |

Fail-closed: unknown/malformed/oversized/forbidden requests never reach the
handler; a scenario that cannot start is reported (not thrown) so the server
stays up; with capture disabled the scenario delivers nothing (consent gate).

## 5. Clean shutdown and lifecycle

`SIGINT`/`SIGTERM` set an atomic stop flag (async-signal-safe); the accept loop
poll-wakes on its 1 s listener timeout and exits, closing the socket. Verified:
`kill -TERM` → process exits cleanly, socket released, empty stderr.

## 6. Defects found and fixed during integration

1. **Unvalidated `Host` → DNS-rebinding exposure.** The foundation only required
   *a* `Host` header. Fixed: the value must be a loopback authority.
2. **No cross-site protection on the state-advancing route.** Fixed: non-loopback
   `Origin` → 403.
3. **No CSP / frame / referrer headers.** Fixed: the HTML response now carries a
   restrictive `Content-Security-Policy` (plus `X-Frame-Options: DENY`,
   `Referrer-Policy: no-referrer`, `Cross-Origin-Resource-Policy: same-origin`).
4. **Clean-shutdown couldn't unblock `accept()`** (SA_RESTART). Fixed with a
   listener timeout + atomic stop flag.
5. **Repeat scenario run delivered nothing** (see FRICTION_LOG): `AudioBroker`
   and `SimulatorRuntime` shutdown latches, plus non-idempotent `grant_consent`
   producing an `ambiguous` (fail-closed) consent state. Fixed with
   `reset_for_reuse()` and an idempotent `grant_consent`.

## 7. Simulator-only vs production-ready (explicit)

**Implemented here (simulator-only local demo):** loopback-only bind, GET-only,
strict validation, bounded sizes/timeouts, Host/Origin enforcement, CSP, clean
shutdown, read-only contract.

**NOT implemented / required before ANY non-local or production use:**
- TLS termination and authenticated HTTPS (this is plain HTTP on loopback).
- Real caller authentication (the demo token is a local test credential; the
  LWA/OAuth provider is not implemented).
- A **reviewed** HTTP/1.1 parser (or adoption of a maintained server library)
  rather than a handwritten subset; HTTP/2, chunked bodies, and full
  header/connection semantics are out of scope.
- Reverse-proxy/hardening review, request-size policy at the edge, rate limiting
  at the edge if exposed, and an access log review.

**Do not claim public-network readiness.** The adapter is for controlled local
development demonstration only.

## Addendum (v15): profiling instrumentation

`HG_PROFILE=1` adds bounded phase timings (steady-clock, numeric only) to the
scenario/response JSON. It is **off by default**; when unset the response carries
no timing fields and behaviour is unchanged (verified). It exposes **no**
credentials, tokens, filesystem paths, personal data, or media — only numeric
millisecond durations and the existing route/op names already in the contract.
Not a change to any access-control or validation behaviour.


## v21 addendum — native HTTP contract and fail-closed hardening (2026-10-13)

Re-verified against the v21 release candidate (fresh out-of-source Release build).

**Contract change (client-visible, intentional).** An unsupported but well-formed
`scenario` value now answers **`400 Bad Request`** with the unchanged generic JSON
body (`{"ok":false,"error":"unknown_scenario","scenario_id":"…"}`). The handler
reports the business status; the transport still rejects framing, header, method,
path and query problems itself (413/403/405/404/400) and a handler status can
never override one of those.

**Adversarial matrix — 51/51 pass.** Missing / duplicate / non-loopback `Host`
(400/403), cross-site `Origin` (403), non-GET methods including lowercase `get`
(405), `Authorization` / `Content-Length` / `Transfer-Encoding` / `Expect` /
obs-fold / body-on-GET (400), unknown or duplicated query keys, empty, malformed,
over-length or out-of-range `cycles`, malformed `scenario` charset (400), traversal
and unknown paths (404), oversized header block (413), truncated and partial
requests (no response, no crash), a 20× malformed flood followed by a valid
request (200). The listener binds IPv4 loopback only and the interface is
unauthenticated **by design** (this hand-written demo server is not claimed to be
production-ready).

**Two fail-closed defects were found and fixed here.**

1. *Abort on an unwritable working directory.* `Logger::initialize` threw
   `spdlog::spdlog_ex` when it could not create `homeguardian.log`; the exception
   escaped `main()` and the process died with SIGABRT (exit 134) — a
   denial-of-service on a read-only or full filesystem. The file sink is now
   best-effort (console-only fallback, reason reported) and registration is
   guarded; `[logger]` covers it.
2. *The bridge's alert store ignored `alert_retention_days`*, so durable audit
   metadata accumulated without bound. The bridge now prunes its own store at
   startup and, at most once per interval, after a scenario write;
   `scripts/verify_retention.sh` covers the seed -> restart -> pruned path.

**Degraded mode stays fail-closed.** With a corrupt or unwritable audit database
the bridge keeps serving but reports `{"error":"unavailable","ok":false}` on the
status route, read routes fail closed, and a scenario claims no persisted alerts.
No authentication or consent path was relaxed in v21.


## D-03 addendum — network-facing device ingress

| Requirement | Implementation | How it is checked |
|---|---|---|
| Genuine peer authentication | OpenSSL mutual TLS, client certificate required (`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`), chain verified against the private CA, verify result compared to `X509_V_OK` explicitly | Acceptance: handshake succeeds for a provisioned device; a connection without a certificate fails |
| Per-device credentials | One certificate per device from the private CA; CN is the device id | `tools/provision_ca.sh issue`; fingerprint printed for registration |
| Identity cannot be spoofed | Device id for authorization comes from the certificate; the message field is only a claim, checked against the certificate | Unit + acceptance: a device presenting another device certificate is refused (`identity_mismatch`) |
| Credential revocation | Registry revocation plus a revocation file re-read on every connection | Acceptance: revoked device refused, un-revoking restores service without a restart |
| Versioning | `protocol_version_major/minor` on every message; mismatch refused | Unit tests on the protocol |
| Message size limits | 4-byte length header validated against a 4 KiB cap BEFORE allocation | Acceptance: oversized frame refused |
| Sequence numbers / replay | Strictly increasing per-device watermark, committed only after every other check passes; bounded duplicate-observation window | Acceptance: replayed sequence refused; unit tests for the watermark-not-consumed-on-refusal property |
| Timestamps | Positive, inside a +120 s / -24 h window, and not more than 60 s behind the device high-water mark | Acceptance: future and ancient timestamps refused |
| Timeouts | Handshake, idle (socket-level `SO_RCVTIMEO`/`SO_SNDTIMEO`) | Config-validated at startup |
| Connection quotas | `max_connections`, `max_queued_connections`, `max_workers`, `max_pending_observations`, all validated to stay inside hard ceilings | Config validation unit tests |
| Bounded buffers | Frame cap, bounded queue, bounded registry, bounded per-device dedup window and sequence history | Unit tests for the bounds |
| Reconnect behaviour | Explicit; a reconnect does not replay accepted history (sequence watermark + duplicate window) | Acceptance: reconnect continues without replay |
| Secure credential storage | Server key mounted read-only `0600`; device keys `0600` in an operator-chosen tree; PKCS#12 in app-private storage with a separate password file; nothing baked into an image | Provisioning script + `.gitignore` |
| No custom cryptography | Only OpenSSL primitives; no home-grown cipher, KDF or MAC anywhere in `src/net/` | Code review: `src/net/TlsTransport.cpp` is the only TLS surface |
| TLS verification never disabled | There is no "insecure" mode in the transport, and the acceptance script uses a real CA | Code review + acceptance |
| Least privilege at runtime | Ingress container: non-root, `cap_drop: ALL`, `no-new-privileges`, no Docker socket, no privileged mode, 128 MB / 0.5 CPU / 128 pids ceilings, own volume, certs mounted `:ro`, `restart: unless-stopped`, 20 s stop grace for a clean SQLite close | `docker-compose.yml`; clean SIGTERM asserted in acceptance |
| No secret in JSON or logs | Observations have no credential field; the ingress logs ids and reason codes | Acceptance log inspection |
| Fail closed on misconfiguration | Missing certificate/key/CA/registry or an out-of-range limit refuses startup (exit 78) | Acceptance: the ingress refuses to start with a missing certificate |

## D-03.1 hardening (delta)

New guarantees, each covered by a test that fails if the behaviour regresses:

1. **Endpoint identity.** A chain-valid certificate is not authorization to serve; the
   peer must match the dialled address's SAN. Cases: matching DNS SAN, matching IP SAN,
   correct CA with wrong SNI/address, missing SAN with matching CN, untrusted CA, expired
   server certificate, expired client certificate.
2. **Offline CA signing key.** The signing key is absent from the runtime image and from
   every running container; probes assert this against both containers and the image.
3. **Delivery-time authorization.** Revocation, consent withdrawal and capture disable
   reach an OPEN connection; a malformed authorization update fails closed; other devices
   stay operational.
4. **Durable replay protection.** An accepted observation is not acceptable again after a
   restart; the state is a bounded counter; an unreadable state fails closed; an
   interrupted write leaves the previous state intact; a legitimate restart resumes.
5. **Honest acknowledgment.** A downstream failure is `processing_failed`, never an
   acceptance, and the watermark does not advance.

Residual risk (stated, not hidden): physical Android capture is unverified; the Android
keystore is not used; the hub's own dashboard remains loopback-only inside the container;
live Home Assistant still needs TLS.

## D-03.2 corrections to the D-03.1 guarantees

* **Honest acknowledgment, corrected.** D-03.1 claimed "a processing failure is never
  reported as an acceptance" and it was true - but the durable watermark still advanced
  first, so the failure was reported as a REFUSAL the device could not recover from. The
  corrected guarantee: a processing failure never consumes a sequence, never reports an
  acceptance, and the device's retry is always accepted (bounded at-least-once).
* **Durability claim, corrected.** The documented fsync-before-rename is now performed:
  file fsync, rename, directory fsync, each with a reported failure (Incident 50).
* **Replay protection, now mandatory.** An unset `replay_state_path` refuses startup rather
  than warning; the isolated-test opt-out is refused on any non-loopback bind (ADR-019).
* **Uncertain outcomes are explicit.** In-flight work at shutdown is discarded, reported at
  startup, and never used to refuse a retry.

Unchanged: exactly-once is NOT claimed anywhere, and the residual duplicate window is
documented with the crash point that causes it.

## H-03 addendum (Android node)

The node's security posture is unchanged and re-asserted in the new wiring: the consent gate runs
BEFORE feature extraction, so an unauthorized payload is not even measured; the wire carries scalars,
never media; the credential files are app-private and only their existence is inspected; mutual TLS
with server-identity verification is mandatory and has no disable switch; and the node still cannot
reach the hub's dashboard, credentials or database - it can only present observations on the one
authenticated port. No privileged container is introduced to reach the phone's camera or microphone.


## HG-SEC-01 - unauthenticated state-advancing route (fixed; independent review pending)

Finding: the demonstration scenario route advanced application state without caller
authorization, selected by a launch secret carried in the URL.

Controls now in force:
- POST /api/session issues a session-bound, single-use CSRF token once the operator
  supplies the launch password in the dashboard. The secret never enters a URL, cookie,
  browser storage, static asset or log, and the entry field is cleared after use.
- The state route is POST-only; a GET is refused by the method rule and never reaches
  the handler.
- Every callback validates scheme, host and port against the listener (loopback only),
  requires a JSON Content-Type, and frames the body explicitly: a body with no declared
  length, a declared length that disagrees with the bytes received, a declared length
  over the cap, a GET carrying a body, and duplicate credential headers are all refused.
- Tokens expire and are single-use; issuance is bounded per window; repeated bad
  credentials trigger a time-bounded lockout; with no operator secret configured the
  state route is disabled (503 state_route_disabled).

Evidence:
- complete C++ suite: 503 test cases, 5817 assertions, exit code 0
- native_http_security: 11 cases, 74 assertions, exit code 0
- nativehttp: 25 cases, 143 assertions, exit code 0
- native_http_persist: 1 case, 48 assertions, exit code 0 - fresh temporary SQLite
  database: rejected requests invoke no handler, advance no state and write no alert
  row (confirmed by reopening the file); two authorized controls do write
- Node suite: 76 tests, 76 passed, exit code 0

Limitations: loopback Origin and Host checks are defence-in-depth and do NOT establish
the identity of an arbitrary local process; the operator launch secret is the
authorization credential. The demonstration remains loopback-only.

Status: HG-SEC-01 IMPLEMENTED - INDEPENDENT QA PENDING
