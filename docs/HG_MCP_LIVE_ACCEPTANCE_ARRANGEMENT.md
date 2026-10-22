# Live MCP network acceptance — arranged, awaiting the announced start

Hermes 3's own owner-run script declares the endpoint; nothing here is guessed.

```
endpoint   127.0.0.1:8790/mcp          (HTTP / Streamable HTTP per spec 2025-11-25)
bind       LOOPBACK-ONLY              the script states: "The listener is loopback-only."
auth       Bearer token, ~/.homeguardian/mcp_credentials.json (0600)
start      bash scripts/hg_mcp_owner_endpoint.sh --store <alerts.db>     [OWNER-RUN]
stop       bash scripts/hg_mcp_owner_endpoint.sh --stop
port       default 8790, overridable with --port N  -> use the announced value
```

## Trigger and sequence

The acceptance runs **once the owner starts the endpoint**. Order matters: the bind check comes
before anything else, because a wrong bind is the exposure class already seen on this project's
observation port (`0.0.0.0:8443`).

| # | check | how | expected | what failure means |
|---|---|---|---|---|
| A | **bind address** | `ss -ltn \| grep :8790` | `127.0.0.1:8790` **only** | `0.0.0.0:8790` = immediate exposure FAIL, stop and report |
| B | LAN exposure gate | `verify_lan_exposure.py --host <lan-ip> --observation-port 8443 --mcp-port 8790` | check 4 **PASS** ("closed on the LAN address while listening on loopback"), checks 1-3 **SKIP** | MCP reachable on the LAN = FAIL |
| C | **unauthenticated refusal** | `POST http://127.0.0.1:8790/mcp` with **no** `Authorization` header | refused (401/403) | accepted = unauthenticated MCP = FAIL |
| D | wrong credential | `POST` with a deliberately invalid Bearer token | refused | accepted = FAIL |
| E | authenticated call | `initialize` + `tools/list` **with the owner's token** | succeeds; tool names recorded | requires the owner's credential — see below |
| F | LAN reachability | TCP connect `192.168.1.9:8790` | closed | open = FAIL |

**B passing with 1-3 SKIPPED is the correct verdict for a loopback-only phase, not a shortfall.** It is
not a private-LAN acceptance. A skipped check is never reported as a pass.

## Credential handling (C and D need no secret; E does)

C and D are deliberately designed to need **no credential**, so authentication enforcement can be
verified without touching the owner's token. E needs a real Bearer token, which belongs to the owner:

- I will **not** print, copy, log or commit the token, and I will not read the credential file.
- E is either owner-run with the raw output handed to me, or run by me only if the owner explicitly
  authorises that specific use.
- Evidence from E is the HTTP status plus the returned **tool names** — never the credential, never a
  request header dump.

## Evidence

Raw output plus `sha256` and the gate exit code via
`python3 scripts/mtls_fixtures/capture_acceptance_evidence.py --host <lan-ip> --observation-port 8443 --mcp-port 8790`,
published with honest PASS/FAIL/SKIP counts. Two rules carried over from what this lane got wrong
once each, and both apply directly here:

1. **A completed TLS/HTTP connection is not authentication** — C and D assert a *refusal*, not merely
   that a connection failed; the endpoint's own HTTP status is the measurement.
2. **A skipped check is not a pass.**

## Boundaries

I will not start, stop or restart the endpoint (owner-run), will not read or transmit the credential,
will not change the container, binding, firewall, Oracle, NSG or router, and will not rescan while
nothing is listening. If the store is unopenable, I expect the endpoint to report **UNAVAILABLE** —
and I will treat a "no alerts" answer from an unopenable store as a defect, not as an empty result,
because that is the same false-green shape as the ones already found in this project.

## Current state

No MCP listener exists (`8790` and every candidate port: no listener; no new loopback listener versus
the baseline). Arrangement ready; trigger armed; nothing to accept yet.
