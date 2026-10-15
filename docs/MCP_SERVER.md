# MCP server (HG-MCP-01)

**Status:** implemented and verified locally. Not deployed, not pushed to `main`.
**Specification:** MCP 2025-11-25 (Streamable HTTP). The server also accepts 2025-06-18 and
2025-03-26 clients; anything else is answered with `2025-11-25` so the client can decide for
itself rather than being silently downgraded.
**Branch:** `feature/mcp-streamable-http` (worktree `~/worktrees/hg-mcp`).

## 1. What it is, and what it is not

It **is** an MCP server: it speaks JSON-RPC 2.0 over Streamable HTTP, negotiates the protocol
version, lists tools and executes them, and enforces the transport's session, Origin,
authentication and bound rules.

It is **not** an Alexa+ runtime, and it is **not** an integration with Amazon. There is no
Amazon account linking, no Login with Amazon, no AWS resource, and no live Amazon identity.
It exposes the same read-only view of the local application that the NDJSON bridge exposes.
Every observation behind that view is still synthetic: no physical camera or microphone is
activated by this milestone.

## 2. Architecture (why nothing was rewritten)

    tools/call ──► McpTools ──► AlexaContract ──► IAlexaCredentialVerifier (AlexaCredentialStore)
      (JSON-RPC)                 (existing)   └─► IAlexaStatusProvider   (Application + PersistenceAudit)

The two tools are translated into the same kind of request the existing NDJSON bridge sends,
carrying the caller's bearer token as the credential. Authorization, response shaping and the
fail-closed rules therefore live in **one** place — `AlexaContract` — rather than in a second,
divergent copy inside the MCP layer.

| New file | Role |
|---|---|
| `src/mcp/McpProtocol.{h,cpp}` | JSON-RPC 2.0 framing, version negotiation, method dispatch, error taxonomy, batching. No sockets — the pure rules, so they are testable directly. |
| `src/mcp/McpTools.{h,cpp}` | The two read-only tools, over `AlexaContract`. |
| `src/mcp/McpHttpServer.{h,cpp}` | Streamable HTTP transport: sessions, Origin validation, auth, bounds, timeouts, bounded concurrency, clean shutdown. |
| `src/main_mcp_server.cpp` | Wiring: the same `Application`, `PersistenceAudit` and `AlexaCredentialStore` the bridge builds. |
| `tests/mcp/test_mcp_protocol.cpp` | `[mcp]` protocol/tool/security tests. |
| `scripts/verify_mcp.sh` | End-to-end evidence against the real binary over loopback. |

The engine, the web demo and the NDJSON bridge are unchanged; `CMakeLists.txt` gained one
target and the test sources, additively.

## 3. Tools

| Tool | Arguments | Returns |
|---|---|---|
| `get_homeguardian_status` | none | `{"state": "...", "capture_enabled": bool, "read_only": true}` |
| `get_alert_summary` | none | `{"alert_count": N, "highest_severity": "...", "alert_ids": [...], "read_only": true}` |

Both are reads. There is no operation in the contract that can enable capture, grant consent,
change configuration or activate a sensor, so no such tool can exist — asserted by a test that
tries to call a plausible-sounding mutating tool and gets `-32602`.

## 4. Transport behaviour

| Request | Behaviour |
|---|---|
| `POST /mcp` (JSON-RPC) | `200` + `application/json`, or `text/event-stream` when the client accepts only SSE. `202` with no body when the payload carried no request (notifications). |
| `GET /mcp` | `405` + `Allow: POST, DELETE`. This server offers no server-initiated stream — permitted by the specification, and stated rather than faked. |
| `DELETE /mcp` | Terminates the session (`200`), `400` without a session id, `404` for an unknown one. |
| Sessions | `Mcp-Session-Id` minted on `initialize`, required afterwards, idle-expired (1800 s), capped (64). An unknown or expired session is `404`; a non-`initialize` request with no session is `400`. |
| `MCP-Protocol-Version` | If present and unsupported → `400`. |
| Origin | If an `Origin` is present and not exactly allowlisted (loopback origins by default, plus `HG_MCP_ORIGINS`) → `403`. Absent Origin is allowed for non-browser clients. |
| Auth | `Authorization: Bearer <token>` verified by `AlexaCredentialStore`. Missing/invalid → `401` + `WWW-Authenticate: Bearer`. The store must be a 0600 owner-only file; a load failure exits non-zero with no fallback. |
| Bounds | header ≤ 16 KiB, body ≤ 64 KiB (`413`), `Content-Length` required (`411`), JSON content type (`415`), 10 s read/write timeouts (`408`), bounded workers (4) and in-flight cap (16 → `503` + `Retry-After`). |
| Shutdown | `SIGINT`/`SIGTERM` set a flag; `serve()` returns within ~200 ms, workers are joined, and the request counters are logged. |

Bind: `127.0.0.1` by default. A non-loopback bind is **refused** unless `HG_MCP_ALLOW_REMOTE`
is set explicitly, and the server refuses to start at all with authentication disabled.

## 5. Running it (demo)

```bash
cmake -S . -B hg_mcp_build && cmake --build hg_mcp_build -j4 --target hg_mcp_server

# 1. Provision a credential file: 0600, owned by you, JSON keyed by token.
umask 077
cat > /tmp/mcp_creds.json <<'JSON'
{"<your-token>":{"principal":"demo","allow_status":true,"allow_alert_summary":true}}
JSON
chmod 600 /tmp/mcp_creds.json

# 2. Run. Logs go to stderr; the port is loopback-only.
HG_MCP_PORT=8790 ./hg_mcp_build/hg_mcp_server config/homeguardian.demo.json /tmp/mcp_creds.json

# 3. Drive it (or point an MCP client at http://127.0.0.1:8790/mcp).
curl -sS -D- -X POST http://127.0.0.1:8790/mcp \
  -H 'Authorization: Bearer <your-token>' \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"curl","version":"1"}}}'
# then reuse the Mcp-Session-Id header on tools/list and tools/call.
```

Environment: `HG_MCP_PORT`, `HG_MCP_BIND`, `HG_MCP_ALLOW_REMOTE`, `HG_MCP_ORIGINS`,
`HG_MCP_AUDIT_DB`. Arguments: `[config.json] [credentials.json]`.

## 6. Evidence

```bash
./hg_mcp_build/homeguardian_tests "[mcp]"   # 19 cases / 89 assertions
bash scripts/verify_mcp.sh                  # 43 end-to-end checks: see §7
```

## 7. Security limitations and unresolved issues (stated, not hidden)

1. **No TLS.** The transport is plain HTTP and loopback-only. Anything remote needs a
   reversing proxy with TLS, or the D-03 mutual-TLS ingress — deliberately not done here.
2. **Static bearer tokens.** `AlexaCredentialStore` is the existing credential mechanism; the
   MCP specification's OAuth 2.1 model (dynamic client registration, resource indicators) is
   **not** implemented. A token with `allow_status`/`allow_alert_summary` grants both tools.
3. **No client-initiated cancellation, no progress notifications, no `resources/*` or
   `prompts/*`.** Only the `tools` capability is advertised, and `listChanged` is false.
4. **No server-initiated SSE stream.** `GET /mcp` is `405` by design; a client that requires a
   server-push channel cannot use this server, and is told so rather than given a stub.
5. **Sessions are in-memory.** A restart invalidates them; there is no resumability and no
   event store.
6. **Rate limiting is global, not per token.** The in-flight cap shapes load; it is not a
   per-caller quota.
7. **The status provider is duplicated** from the bridge's file-local `AppStatusProvider`
   (same semantics, pinned by tests). Extracting a shared unit would mean editing verified
   code, so the duplication is deliberate and documented.
8. **The `option_chain`-style depth of the contract is shallow by design:** neither tool
   returns raw alerts or media, so an MCP client cannot be used to exfiltrate household data.

## 8. Handoff for independent verification

To verify without trusting this document:

1. `git checkout feature/mcp-streamable-http` (or use the worktree) and confirm `main` is not
   an ancestor of the new commits' target branch beyond the recorded baseline.
2. `cmake -S . -B hg_mcp_build && cmake --build hg_mcp_build -j4` — expect zero errors and no
   new warnings from `src/mcp/*` or `src/main_mcp_server.cpp`.
3. `./hg_mcp_build/homeguardian_tests` — expect **510 cases / 5,787 assertions**, and
   `"[mcp]"` → **19 cases / 89 assertions**. The baseline before this work was 491 / 5,698.
4. `bash scripts/verify_mcp.sh` — expect **43 passed, 0 failed**.
5. Try to falsify, specifically: call a tool with no token (expect `401`), with a wrong token
   (expect `401`, and confirm the body carries no diagnostic), from a disallowed `Origin`
   (expect `403`), with a 70 KB body (expect `413`), with `GET` (expect `405`), after
   `DELETE` (expect `404`), and with `HG_MCP_BIND=0.0.0.0` and no opt-in (expect exit 4).
6. Confirm the web demo still runs (`bash demo.sh`) — the MCP target is a separate executable
   and no existing target was modified.
