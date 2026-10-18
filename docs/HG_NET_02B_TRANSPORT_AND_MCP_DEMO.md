# HG-NET-02B — Bridge transport confinement and the judge-ready MCP demo

This document covers two things: the two Android transport defects HG-RELEASE-08 identified and how
they are fixed and tested, and the reproducible demonstration procedure with the native C++ MCP
engine for hackathon submission Issue #8.

---

## Part 1 — The two transport defects

### Defect 1: "private destination + a tunnel is up" does not mean the traffic uses the tunnel

The earlier policy accepted an endpoint when the destination was private AND some tunnel interface
was up. Both can hold while the packet leaves over Wi-Fi:

* the coordinator address can be a **LAN** address (192.168.1.50 is private);
* a `tun0` belonging to an unrelated VPN can be **up** at the same time.

In that state the bridge would present the device auth token on an unprotected link. "The tunnel is
up" is a statement about the device, not about the path to this destination.

**Fix — containment, not a flag.** A non-root app cannot read the kernel routing table and cannot
bind a socket to an interface by name (`SO_BINDTODEVICE` requires `CAP_NET_RAW`). So the property is
established from the interface list alone:

1. the destination must lie **inside a tunnel interface's own subnet**, i.e. it is on the overlay;
2. it must lie **outside every non-tunnel interface's subnet**, i.e. no LAN egress.

An address that is on the overlay and on no directly-attached network can only be reached through the
tunnel. The overlay prefix is read from the live interface configuration on every check, so the
approved deployment is the source of truth — **nothing is hardcoded**, and in particular no example
range such as 10.42.0.0/24 appears in the code or the tests except as test data.

Rule 2 is evaluated **before** rule 1 on purpose. If a tunnel shares the LAN's prefix the overlay is
ambiguous (a packet to the tunnel's own address may leave by either interface, and nothing available
to the app can tell which), so that configuration is refused rather than trusted.

### Defect 2: the reconnection loop did not repeat the policy check

The policy was evaluated once in `start()`. The bridge could be started with the tunnel up, lose it,
and then keep redialling on every backoff — leaking on each attempt.

**Fix — the check is now continuous:**

* `loop()` re-validates **before every dial attempt** and, while refused, waits instead of dialling,
  so the bridge resumes by itself when the tunnel returns and never leaks in the meantime;
* the heartbeat re-validates **inside a live session** and tears it down if the transport stops
  being acceptable, because a connect-time check says nothing about the path minutes later.

### Where it is enforced

`OutboundMcpBridge.start()` and `loop()` and the heartbeat — not only the activity. A UI-only check
would be advisory; any caller reaching the service directly would bypass it. The UI check exists to
produce a readable reason.

---

## Part 2 — Test evidence

`scripts/bridge_ws_compat/TransportPolicyNegativeTest.java` — **37/37 PASS**, run against the real
`TransportPolicy` class on the JVM with a `Log` stub. The network topology and the hostname resolver
are injectable, so the negative cases are exercised for real instead of asserted.

Every topology asserts **both** directions: a policy that refused everything would pass a
refusals-only suite, so the ALLOW cases are load-bearing.

| Topology | Cases |
|---|---|
| A — healthy: `tun0 10.42.0.1/24` + `wlan0 192.168.1.9/24` | overlay address ALLOW; loopback ALLOW (v4 and v6); **LAN egress REFUSE**; off-overlay private REFUSE; CGNAT off-overlay REFUSE; public v4 REFUSE; public DNS name REFUSE; `wss://` REFUSE; `http://` REFUSE; empty REFUSE |
| B — tunnel interface DOWN | overlay REFUSE; LAN REFUSE; loopback ALLOW |
| C — no tunnel at all (tunnel loss) | previously-valid overlay REFUSE; LAN REFUSE |
| D — **adversarial**: tunnel shares the LAN prefix | LAN egress REFUSE; **ambiguous overlay address also REFUSE** (rule 2 precedes rule 1) |
| E — different overlay prefix (`wg0 172.30.7.1/30`) | /30 peer ALLOW; outside /30 REFUSE; `10.42.0.1` REFUSE (proves nothing is hardcoded) |
| DNS changes | name → overlay ALLOW; **same name re-pointed to LAN REFUSE**; **re-pointed to public REFUSE**; back to overlay ALLOW; dual-homed name REFUSE; resolution failure REFUSE (fails closed) |
| Subnet arithmetic | /24 last host, /24 next subnet excluded, /32 host route, /30 boundary, /0, cross-family v4-v6, unknown prefix |

One expectation in an earlier draft of topology D was wrong and is corrected in place: it expected an
ALLOW for a destination inside a tunnel prefix that overlaps the LAN. The policy's refusal is the
safer reading, so the assertion was corrected, not the behaviour.

---

## Part 3 — Reproducible demo with the native C++ MCP engine (Issue #8)

The engine is a real C++ binary built from the repository, not a mock.

```bash
# 1. Clean checkout of the branch that carries the engine (isolated worktree)
cd ~/projects/homeguardian-ai
git worktree add -f ~/projects/homeguardian-ai-worktrees/hermes-mcp-demo \
    feature/hg-net-02f-common-mcp-bridge

# 2. Build it (CMake target hg_mcp_server; ~4 min at -j4)
cd ~/projects/homeguardian-ai-worktrees/hermes-mcp-demo
mkdir -p build_mcp && cd build_mcp
cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --target hg_mcp_server -j 4
# -> build_mcp/hg_mcp_server, 3,380,904 bytes

# 3. Provision a 0600 credential file (token -> principal + per-tool allow)
printf '{ "%s": { "principal": "demo-viewer", "allow_status": true, "allow_alert_summary": true } }\n' "$TOK" > creds.json
chmod 600 creds.json

# 4. Launch, loopback only
HG_MCP_PORT=8790 HG_MCP_BIND=127.0.0.1 HG_MCP_ALLOW_REMOTE=0 \
    ./hg_mcp_server ../config/homeguardian.demo.json creds.json

# 5. Drive the protocol (note: /mcp path AND the MCP Accept header are both required)
curl -X POST http://127.0.0.1:8790/mcp \
  -H "Authorization: Bearer $TOK" -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}}'
# capture Mcp-Session-Id from the response headers, then send it on every later call
curl -X POST http://127.0.0.1:8790/mcp $HDRS -H "Mcp-Session-Id: $SID" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

### Measured transcript

```
initialize  -> {"result":{"capabilities":{"tools":{"listChanged":false}},
                "protocolVersion":"2025-11-25",
                "serverInfo":{"name":"homeguardian-mcp","version":"1.0.0"}}}
              Mcp-Session-Id minted (64 hex)

tools/list  -> get_homeguardian_status : "Read-only lifecycle status of the HomeGuardian
                                          application: lifecycle state and whether synthetic
                                          capture is enabled. Takes no arguments. Cannot change state."
               get_alert_summary       : "Read-only aggregate alert summary: total alert count,
                                          highest severity, and a ..."

tools/call  -> get_homeguardian_status : {"capture_enabled":true,"read_only":true,"state":"Running"}
tools/call  -> get_alert_summary       : {"alert_count":0,"alert_ids":[],"highest_severity":"",
                                          "read_only":true}

negative    -> no session header            HTTP 400
negative    -> wrong bearer token           HTTP 401
negative    -> missing Accept header        "{}" (HTTP 200, empty)  <-- silent trap, see below
```

The last negative case is the reason the Accept header and session continuity matter: the engine
answers `{}` with **HTTP 200** rather than an error when the MCP `Accept` header is absent, and
rejects later calls on a fresh connection with "initialize must be called first" when the session id
is not carried. Both were found by testing against this real binary; a stub MCP cannot falsify
either assumption because it answers regardless of headers and session state.

### Server-side corroboration

The engine's own accounting at shutdown, independent of anything the client reported:

```
[info]    hg_mcp_server listening on 127.0.0.1:8790 (loopback=true, auth=required, tools=2)
[warning] mcp: rejected an unauthenticated request
[info]    mcp: server stopped
[info]    mcp: requests=7 rejected_origin=0 rejected_auth=1 oversize=0 shed=0 errors=0
```

Seven requests matches the transcript exactly (initialize, tools/list, two tools/call, three
negative probes); exactly one auth rejection matches the wrong-token probe; zero oversize, zero
shed and zero errors means no request was dropped or malformed. This is the server counting its own
traffic, so it is evidence rather than a restatement of the client's claims.

---

## Part 4 — Synthetic versus physical-device inputs

Stated plainly, because the difference is the difference between evidence and decoration.

**Real (C++ origin, executed here):** the `hg_mcp_server` binary; its MCP protocol handling,
session minting, auth and tool allow-list; both tool implementations and the state they serialise;
the loopback HTTP transport.

**Real (Android origin, executed here):** `TransportPolicy` and `OutboundMcpBridge` Java classes,
run against the real classes with only `android.util.Log` stubbed.

**Synthetic:** the HomeGuardian *application state* the tools report (`capture_enabled`,
`state:"Running"`, `alert_count:0`) comes from the demo config and simulator, which is what
synthetic capture means here — no household camera or microphone data is involved. The credential
and session values are throwaway test values in a temp directory.

**Not yet real:** the bridge has never executed on the Mi A3. The APK has not been installed and the
production EdgeService has not been touched. No VPN tunnel exists yet, so no case here has been
exercised over a real WireGuard path. Physical-device acceptance remains outstanding and is not
claimed.

---

## Part 5 — Video storyboard (~2 min 45 s)

English, screen-recorded, no household media.

| Time | Scene | On screen | Narration |
|---|---|---|---|
| 0:00–0:20 | Problem | Architecture diagram: phone (C++ engine + MCP) — VPN — coordinator | "HomeGuardian keeps its intelligence on the device. A Xiaomi Mi A3 runs the native C++ engine and an MCP server. Nothing is reachable from the public internet." |
| 0:20–0:45 | Build from source | `git worktree add`, `cmake`, `cmake --build --target hg_mcp_server` | "The engine is a real C++ binary built from a clean checkout — not a mock." |
| 0:45–1:00 | Loopback only | `ss -ltn` showing `127.0.0.1:8790`; the credential file at mode 0600 | "The MCP server binds loopback only. Credentials are a 0600 file mapping a token to a principal and a per-tool allow-list." |
| 1:00–1:30 | initialize + tools/list | terminal running the two calls; the JSON responses | "A client initialises, receives a session id, and lists the tools. Both tools are read-only by construction." |
| 1:30–2:00 | tools/call | both calls, showing `{"capture_enabled":true,"read_only":true,"state":"Running"}` and the alert summary | "These are values from the running engine: lifecycle state and an aggregate alert summary." |
| 2:00–2:20 | Refusals | the three negative cases: 400, 401, and the `{}` Accept trap | "A missing session is refused, a wrong token is refused with 401. Missing the MCP Accept header returns an empty 200 — a silent trap we found by testing the real binary." |
| 2:20–2:45 | Confinement | the topology test output: LAN egress refused, off-overlay refused, tunnel loss refused | "The bridge refuses to send its credentials unless the destination is on the VPN overlay and reachable by no other link — and it re-checks on every attempt, so a tunnel that drops cannot cause a leak." |

---

## Part 6 — Evidence register and checklist

| # | Item | Status | Evidence |
|---|---|---|---|
| 1 | Native C++ MCP builds from clean checkout | PASS | `hg_mcp_server` 3,380,904 B, sha256 `1656fd2b182e56097536cee6f4acc5ba54eba4d0dd213431dff707b160e6b989`; built from `feature/hg-net-02f-common-mcp-bridge` @ `dd9ee00fc7a6294ca7d5d21b3f9d416038df0968` (working tree 0 files dirty); cmake 3.28.3 / g++ 13.3.0 |
| 2 | `initialize` | PASS | `serverInfo` + protocolVersion + session id in transcript above |
| 3 | `tools/list` | PASS | 2 genuine tool definitions |
| 4 | `tools/call` read-only status | PASS | `{"capture_enabled":true,"read_only":true,"state":"Running"}` |
| 5 | `tools/call` read-only alerts | PASS | `{"alert_count":0,...,"read_only":true}` |
| 6 | Auth negatives | PASS | 401 wrong token, 400 missing session |
| 7 | Transport policy negatives | PASS | 37/37, five topologies plus DNS and arithmetic |
| 8 | Bridge ↔ coordinator interop | PASS (off-device) | 15/15 vs the real coordinator + native engine |
| 9 | Bridge on physical Mi A3 | BLOCKED | no APK install; production EdgeService untouched |
| 10 | VPN handshake | BLOCKED | needs phone key + Hermes 1's deployment values + owner consent |
| 11 | Remote MCP over VPN | BLOCKED | depends on 9 and 10 |

Not claimed: hardware install, VPN connectivity, 24/7 reliability, any camera or microphone path.
