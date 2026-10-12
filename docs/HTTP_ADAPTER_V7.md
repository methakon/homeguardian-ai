# HTTP Adapter v7 — Local-Only Loopback Adapter (F22-33)

**Date:** 2026-10-12
**Scope:** Build a local-only HTTP adapter around the existing `AlexaContract`
and `AlexaCredentialStore`, with transport-level protections and regression
tests. **Development/integration milestone — NOT a production deployment.**
**Verified baseline:** commit `7334543` on `origin/main`, clean working tree.
**Mode:** Simulator-only. No public/LAN listener, no cloud, no deployment, no
physical sensors.

---

## 1. What was built

`src/bridge/hg_http_adapter.js` — a local-only HTTP front-end that binds
**exclusively to IPv4 loopback `127.0.0.1`** and forwards each request to the
**real compiled C++ `hg_alexa_bridge`** over its NDJSON stdio boundary. The
bridge runs the real `AlexaContract` + `AlexaCredentialStore`.

**No new dependency was added.** The adapter uses the Node.js **stdlib `http`
module** (a battle-tested HTTP parser) rather than hand-writing a fragile HTTP
parser or pulling in an unreviewed C++ HTTP library. (cpp-httplib was considered
and deliberately NOT added — the milestone prohibits silently adding an
unreviewed dependency.) No authentication or authorization logic is duplicated:
all of it lives in the C++ contract + credential store; the adapter only
enforces transport concerns and maps protocol errors to generic HTTP responses.

The existing local stdio bridge (`hg_alexa_bridge`) and all interfaces are
preserved unchanged.

---

## 2. Transport security controls (implemented + tested)

| Control | Implementation | Test |
|---|---|---|
| Loopback-only bind | `server.listen(port, '127.0.0.1')`; no other host path; asserts bound address at `listening` and exits if not 127.0.0.1 | `listener binds loopback only` (TCP connect to LAN IP must fail) |
| Method allowlist | GET + OPTIONS only; else 405 | `wrong method -> 405` |
| Route allowlist | `/status`, `/alert_summary`, `/health` only; else 404 | `unknown route -> 404` |
| Body size | GET accepts no body; `MAX_BODY_BYTES=4096`; declared body → 400 | oversized/body tests |
| Header size | `maxHeaderSize` (16KB) enforced | `oversized request line/headers` |
| Timeouts | request 5s, headers 5s, keep-alive 5s, bridge call 8s | graceful/bounded tests |
| Concurrency | `MAX_CONCURRENT=8` in-flight; excess → busy → 502 | burst tests stay responsive |
| Rate limiting | fixed 10s window, 30 req/IP, table bounded to 1024 keys | `rate limiting kicks in` |
| Content-Type | non-JSON declared type → 415 | `invalid content type -> 415` |
| Auth parsing | strict single `Bearer <token>`; ambiguous/malformed → generic 401 | `ambiguous/malformed Authorization` |
| Generic auth failures | 401 never reveals missing vs invalid vs expired vs revoked | missing/invalid/expired/revoked all → 401 |
| No token leak | tokens never in responses/health/logs | `no token leakage` |
| Read-only | operation derived from ALLOWLISTED route only, never client input | `unknown route -> 404` for mutating paths |
| Graceful shutdown | SIGINT/SIGTERM close server, bounded force-exit | `graceful shutdown on SIGTERM` |
| No persistence | no request data or raw media written to disk | code review; no disk writes in adapter |
| No shell | bridge spawned with argument ARRAY (no shell) | code review |

**Health endpoint** (`/health`) reveals only `{ok:true,status:"up"}` — no
secrets, paths, token metadata, config, or personal data. It does not call the
bridge or touch credentials.

---

## 3. Credential behavior

- `HG_BRIDGE_SECURE_CREDS=1` (the default in the adapter's secure-mode tests)
  uses the real `AlexaCredentialStore`; a load failure makes the bridge exit 3
  (fail-closed). No silent fallback to development credentials.
- Authentication/authorization separation is preserved (store authenticates;
  contract authorizes per operation → 403 when authenticated but unauthorized).
- All mutating-operation prohibitions are preserved: the adapter only ever
  requests `status` or `alert_summary`, derived from the allowlisted route.
- **Genuine Alexa identity verification is NOT claimed**: the LWA/OAuth provider
  remains unimplemented (see `docs/SECURITY_READINESS_V6.md`). The adapter
  verifies local bearer tokens only.

---

## 4. TLS boundary — do not overclaim

**This adapter does NOT provide TLS.** A loopback HTTP listener is not
end-to-end TLS. Traffic between a local client and `127.0.0.1` is plain HTTP.
This adapter is for **controlled local development use only**. Any future
non-local deployment requires **authenticated HTTPS with an explicitly reviewed
TLS termination design** (reverse proxy or in-process TLS), which is out of
scope for this milestone. No reverse proxy or production TLS was configured.

---

## 5. Exact verification results

| Check | Command | Result |
|---|---|---|
| HTTP adapter tests | `node --test test/http_adapter.test.js` | **18/18** |
| Node full | `cd alexa_skill && npm test` | **68/68** (13 handler + 12 contract + 21 integration + 4 restart + 18 http_adapter) |
| C++ full host suite | `hg_build/homeguardian_tests` | **255 cases / 2004 assertions pass** (unchanged; no new C++) |
| CTest | `ctest --test-dir hg_build` | **100% (1/1)** |
| ASan/UBSan | `hg_asan/homeguardian_tests` | **255 cases, zero sanitizer errors** |
| Fresh release build | out-of-source, existing dep cache | app + tests + bridge clean; suite **255/2004** |

**Listener-binding evidence:** the `listener binds loopback only` test derives
the machine's non-loopback IPv4 address and asserts a TCP connect to
`<lanIp>:<port>` fails, while `127.0.0.1:<port>` succeeds.

---

## 6. Remaining production requirements (NOT implemented)

- **B2 TLS termination** — the adapter is plain HTTP on loopback; no TLS.
- **Real identity validation** — LWA/OAuth provider still unimplemented.
- **Secret rotation/revocation operations** — persistent revocation is an
  out-of-band operational procedure.
- **Rate limiting at production scale** — the in-process fixed-window limiter is
  suitable for local dev, not a distributed deployment.
- **Health checks + supervision** — `/health` exists; no process supervisor.
- **Non-local exposure** — prohibited here; requires reviewed HTTPS design.

---

## 7. Go / No-Go for the next stage

**The local-only HTTP adapter is DONE and tested for controlled development
use.** It is **NOT production-ready** and must not be exposed beyond loopback.
Proceeding to any network-facing deployment is **NO-GO** until B2 (TLS), real
identity validation, and supervision are implemented and reviewed. The adapter
itself is a sound local integration surface: it reuses the real contract +
credential store with no duplicated auth logic and no new dependency.
