# Live Alexa → mailbox → native C++ path: verification record

Date: 2026-10-23 21:43 IST. Status: **DEPLOYED PATH RE-VERIFIED (native + mailbox)**.
Supersedes the earlier "empty `{}` bridge reply" and "no accepted mailbox round
trip" checkpoints. Owner-reported Alexa voice replies remain owner-attested.

Nothing was redeployed, restarted, reinstalled or reconfigured for this check, and
capture stayed disabled. The working Alexa lane was left exactly as it was.

## 1. Native JSON-RPC through the deployed private path — proof

Query performed on the coordinator host against the deployed private MCP endpoint
(VPN address, coordinator port), reusing the deployed worker's own client settings
and protected application token. Token, session ID and endpoints are not reproduced
here; only the protocol envelope and read-only aggregates are.

```
initialize  -> http 200, jsonrpc "2.0", id 1
               protocolVersion "2025-11-25"
               serverInfo {name: "homeguardian-mcp", version: "1.0.0"}
               capabilities.tools present, Mcp-Session-Id issued (75 chars)
tools/list  -> ["get_alert_summary", "get_homeguardian_status"]   (exactly two, read-only)
tools/call  -> get_homeguardian_status {"state":"Running","read_only":true,"capture_enabled":false}
tools/call  -> get_alert_summary {"alert_count":0,"alert_ids":[],"highest_severity":"","read_only":true}
DELETE      -> http 200, session torn down
```

This is a valid JSON-RPC result with matching ids and a real tool payload — not an
HTTP 200 carrying `{}`. The earlier empty-body behaviour belonged to the stale
native session retained by the installed bridge; reconnecting only that bridge
cleared it, and the defect is recorded as a bridge-side recovery gap.

## 2. One real request/reply through the deployed mailbox — proof

Operator-origin IAM identity (never root, never the hosted role) sent correctly
formed requests to the deployed request queue and received validated, correlated
replies from the deployed worker. Correlation required matching `version`,
`request_id`, `tool` and the exact unexpired `expires_at`; other callers' messages
were made visible again rather than deleted.

```
get_homeguardian_status  correlated=True  latency 2.634 s  {"state":"Running","read_only":true,"capture_enabled":false}
get_alert_summary        correlated=True  latency 1.190 s  {"alert_count":0,"read_only":true,"highest_severity":""}
MAILBOX PASS: 2 of 2 valid correlated replies
```

Vantage point matters: this proves AWS → VPS worker → phone C++ → AWS with the
deployed components. The earlier owner-reported Alexa simulator replies prove the
hosted Lambda side separately. Neither substitutes for the other, and this run did
not re-deploy the hosted skill.

## 3. Deployed code and permissions, without secrets

Worker service: `active`, `enabled`, one supervised process. `NRestarts=3` reflects
bounded one-hour process lifetimes, not crash-looping; no restart was issued for
this verification.

SHA-256 of deployed files compared with `main` (`abaf5ea`):

| Deployed file | Deployed | Matches main HEAD |
|---|---|---|
| `deploy/coordinator/alexa_mailbox_worker.py` | `ce2f2bd7debf…` | yes |
| `deploy/coordinator/homeguardian-alexa-mailbox.service` | `8e3f39a85626…` | yes |
| `deploy/coordinator/service.js` | `6c0e49739458…` | yes |
| `web_mcp/outbound_bridge/coordinator.js` | `216f9d375bc5…` | **no** — deployed `216f9d37`, current `214e9cee` |

The deployed coordinator therefore **predates** the committed coordinator
hardening (`8458cd8`). That fix is source-tested and pushed, not deployed; the
running coordinator still has the malformed-input/Lifecycle exposure documented in
`docs/COORDINATOR_SECURITY_FIXES.md`. Deploying it is a separate approved change and
was deliberately not performed while the Alexa lane is working.

Worker IAM identity `homeguardian-alexa-worker`: one Active key created 2026-10-23,
no groups, no attached managed policies, and exactly two inline statements —
`ConsumeRequestsOnly` (`sqs:ReceiveMessage`, `sqs:DeleteMessage`) and
`SendRepliesOnly` (`sqs:SendMessage`), each restricted to the matching queue ARN with
`aws:SecureTransport=true`. Wrong-direction calls were previously rejected by AWS.

VPS listeners are unchanged: public 22/25/80/465/587/993/995 plus mail; the bridge
and coordinator ports bind only on the tunnel address `192.168.77.1`. No MCP port is
public. Capture remains disabled.

## 4. What this does not prove

- Alexa-Lambda-origin execution was not re-run here; the owner's simulator replies
  stay owner-attested, and the hosted Code package is unchanged since deployment.
- Sustained reliability is still open: one earlier launch answered unavailable, and
  the worker journal contains a `ReadTimeoutError` that is not yet correlated to it.
- The installed Android bridge still lacks permanent stale-session recovery, and
  reboot/changed-network recovery remain unverified.
- The deployed coordinator does not include the hardening commit.
- No physical sensor-to-alert acceptance is claimed; status/alerts reflect the
  current read-only stores, and zero alerts is a reported count, not a safety claim.
