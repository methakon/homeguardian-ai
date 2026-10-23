# Coordinator security corrective batch

Date: 2026-10-23. State: SOURCE-TESTED / NOT DEPLOYED.

## Scope

First authorized corrective batch on main: coordinator malformed-input containment,
credential/connection authority, and bounded client sessions. Work uses the existing
checkout inside the canonical project. No live endpoint fault injection, service
restart, phone change, capture, VPN/cloud/credential modification, or deployment.
Provenance mismatch and detector wiring are subsequent separate batches.

## Changes

- Reject malformed request targets and non-object HTTP/WS JSON before unsafe use.
- Check credential field types before constant-time comparison.
- Contain rejected HTTP ingress promises and asynchronous body-handler errors.
- Stop body processing after a 64-KiB rejection, measured in bytes.
- Invalidate active channel authority synchronously on rotation, revocation or
  supersession, rejecting pending requests; unchanged-token reload keeps the channel.
- Require the current authorized edge socket for authenticated frame processing.
- Cap active plus initializing client sessions at 1,024 by default; idle expiry is
  30 minutes. Validate constructor options and reserve capacity across await.
- Retain/advertise sessions only for successful initialization; reclaim reservations
  after errors and disconnected clients. Reject expired POST and DELETE sessions.

## TDD and executed verification

Regression slices were observed RED before their respective implementation. The
final regression suite has 13 tests, including real loopback HTTP/WS malformed-input
recovery and rotation/reconnect checks. Some lifecycle/fault-injection cases use
controlled transport objects; they are not live VPS attack evidence.

Parent verification command:

    timeout --signal=TERM --kill-after=5s 90s bash scripts/test_outbound_bridge.sh

Observed: 13 security tests passed, zero failed or skipped; existing bridge suite
46 checks passed, zero failed. It exercised the freshly built main-checkout native
MCP executable on workstation loopback, authenticated read-only tools, rejected
mutations/invalid credentials, teardown and reconnect. Node syntax check and
`git diff --check` also passed.

The runner uses hg_build/hg_mcp_server by default and fails when missing; an explicit
HG_MCP_SERVER_BIN override is supported. Temporary fixtures stay under
hg_build/test-tmp. Local fixture identities and credentials are synthetic.

## Remaining gates

No claim that the running VPS contains this revision. Deployment needs a controlled
preservation-first handoff and readback. This does not close native Android session
recovery, sensor integration, alert durability, physical acceptance or sustained
Alexa reliability. Session options are constructor configuration; this batch does
not introduce a new cloud control plane or relax read-only/authentication policy.
