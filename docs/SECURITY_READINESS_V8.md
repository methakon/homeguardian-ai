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
