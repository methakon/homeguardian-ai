# Roaming phone access to private MCP

## Decision and scope

The owner requires operation across dynamic home/public IPv4 addresses without
manual source-IP updates. Source-IP-specific cloud rules caused a real outage:
phone handshake packets were discarded before reaching the VPN server.

Use the existing WireGuard gateway with cloud ingress for **UDP 51820 only from
0.0.0.0/0**. Authenticate enrolled devices by WireGuard keys, not their current
public address. WireGuard supports peer endpoint roaming; changing a client's
public address must not require editing cloud rules. Assigned private overlay
addresses identify authorized tunnel roles and are not fixed home/public addresses.

The latest owner instruction authorizes this narrowly scoped roaming-policy
change. It supersedes the proposed one-home-/32 workaround. Do not add public TCP
MCP, broaden another port, change keys/routes, add peers or modify mail services.
Old restricted-source rules can remain unchanged; they are redundant once the
roaming rule exists and are no longer an availability dependency.

## Security and Cloudflare

Dynamic IPs are not an authentication mechanism. The controls remain independent:
WireGuard peer authentication/encryption; private coordinator listeners and role
restrictions; separate bridge/reader credentials; phone-local MCP authentication;
read-only method/tool allowlists. The phone MCP remains loopback-only.

Ordinary Cloudflare proxy/Tunnel configuration does not proxy native WireGuard UDP.
The existing Cloudflare HTTPS portal remains unchanged. This repair does not route
camera media through the VPS, deploy a public WSS alternative, enroll this
workstation as a peer, or promise operation on networks that block UDP entirely.
A genuine UDP-blocked-network fallback requires a separate reviewed transport.
Server endpoint/DNS migration and IPv6-only-network recovery are not established
by accepting roaming IPv4 sources.

## Implementation and rollout

- `deploy/coordinator/roaming_ingress_policy.py` produces an additive, idempotent
  exact-port UDP policy, without any client public IP parameter.
- `deploy/coordinator/apply_roaming_ingress.py` uses the operator's existing OCI
  SDK environment. Required `--vnic-id` and `--nsg-id` are resolved privately from
  the verified target, never hardcoded in source. Default is dry-run. Apply needs
  explicit `--apply --snapshot <private-new-file>` and prior owner authorization.
- The apply utility refuses NSGs not attached to the expected VNIC or attached to
  any additional VNIC. It snapshots existing rules with mode 0600 and fsync, adds
  the single policy, reads back the NSG and verifies every prior rule is unchanged.
  No host firewall mutation or service restart is required when the existing host
  already accepts this UDP port. Concurrent unexpected changes fail verification
  and require inspecting the private snapshot, not blindly restoring a shared NSG.
- Rollback removes ONLY the newly recorded rule ID from that NSG after comparing
  the before/after snapshot. Never replace the whole NSG or touch unrelated rules.
- Run with the existing operator OCI Python environment. The application runtime
  and phone do not need the OCI SDK or cloud credentials.

## Acceptance

Run `python3 tests/portal/test_roaming_ingress_policy.py` for roaming source
coverage, exact UDP port restriction, idempotency, malformed-inventory refusal and
preservation of unrelated rules. Run the apply utility in dry-run, explicitly
apply the approved policy, then re-run to prove idempotency on the actual NSG.

Read back the exact new rule, require a fresh enrolled-phone handshake and actual
worker-origin results from both native tools. Missing/wrong authentication and
mutating-tool refusal must still hold. Verify fresh camera observations separately
from connectivity. No host test or rule-list entry alone proves those live checks.

A second public-network change and phone reboot remain NOT VERIFIED until tested.
The static policy removes the identified source-/32 restriction but does not prove
all future network conditions or platform lifecycle recovery.

## Applied checkpoint — 2026-10-25

Source `7d8e0ebcad49ef3cf917c4dd4943e0809014af50` was pushed before application.
Seven policy tests passed. Live dry-run found two existing rules and planned one
addition. Apply verified exclusive target membership, wrote a private before/after
snapshot, added the roaming rule and read back both original rules unchanged.
A repeat dry-run found three rules and planned zero additions. A synchronized
capture then observed the external workstation's actual 14-byte UDP probe at the
VPS. An earlier fixed-delay capture was inconclusive; readiness must precede send.

The reader rejects missing and wrong authorization with 401. Public TCP probes to
8790, 18994 and 18995 did not connect. Coordinator and mailbox remain active.
The phone subsequently was absent from ADB and its last handshake remained stale;
both worker-origin tool calls still returned 503 at initialize. The cloud roaming
policy is applied, but phone/MCP recovery is NOT COMPLETE and the observer gate
remains CLOSED. A fresh phone connection is required for the remaining checks.
