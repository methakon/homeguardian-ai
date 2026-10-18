# HG-SEC-07 Rev C follow-up — containment FAILS against the deployed overlay

Status: **FAIL.** The route-containment check does not work against the deployed overlay addressing
pattern. It is not verified, and it must not be represented as verified.

> Privacy note: this revision deliberately contains no endpoint addresses, peer keys or overlay
> subnets. Concrete deployment values are supplied to the test at run time and never committed. An
> earlier revision of this file published infrastructure identifiers; see "Historical exposure" at
> the end, which a later commit does not undo.

## What was tested

The reviewer asked for verification against the actual deployed overlay rather than synthetic
interface topologies. The deployed peer configuration was read from its local path outside the
repository (never committed, mode 0600); only its **addressing shape** is described here:

```
[Interface]
Address = <peer>/32          # ONE address per peer - a host route, not a shared subnet
MTU     = 1420
[Peer]
AllowedIPs = <hub>/32        # the control-plane peer
```

The overlay is addressed with **/32 host routes** — one address per peer — rather than a shared
prefix. That property is the whole point, and it is what the synthetic tests got wrong.

## What happened

Evaluating that addressing pattern against `TransportPolicy` (reproducible with
`scripts/bridge_ws_compat/RealOverlayCheck.java`, which takes the addresses as command-line
arguments):

```
tun0 = <peer>/32  + wlan0 = <lan>/24          <- the DEPLOYED pattern
  ws://<hub>:<port>       REFUSE  "not inside any tunnel interface's subnet"
  ws://<peer>:<port>      REFUSE  "not inside any tunnel interface's subnet"
  ws://<lan-host>:<port>  REFUSE  (correct)

tun0 = <peer>/24  + wlan0 = <lan>/24          <- what the synthetic tests used
  ws://<hub>:<port>       ALLOW   <- false confidence
  ws://<peer>:<port>      ALLOW
  ws://<lan-host>:<port>  REFUSE  (correct)
```

So the policy **refuses the coordinator it is supposed to reach** under the addressing scheme
actually deployed. A correctly deployed coordinator would be unreachable, and the operator would see
a refusal reason blaming the overlay.

## Root cause

`NetworkInterface` is the wrong source of truth for "does this destination travel over the tunnel".

* With a `/32` peer address, a tunnel interface's own subnet contains only itself, so subnet
  containment can match nothing else on the overlay.
* Android's `VpnService` installs **per-peer `/32` routes** in the VPN's own routing table. Those
  routes are the actual reachability statement, and `NetworkInterface` does not expose them.

Containment-by-subnet happens to work for a tunnel configured with a shared prefix — which is what
the synthetic fixtures used, so they could never have caught this. That is precisely the
synthetic-fixture hazard the reviewer raised: a fixture encoding an assumption the deployment does
not share, reported as passing.

## What is still believed sound

* The **LAN-egress** rule (a destination must not sit inside a non-tunnel interface's subnet) is
  unaffected. It refused the LAN host correctly in both runs, and it depends only on the LAN
  interface's own prefix, which is real and genuinely shared.
* **Fail-closed reconnection** is unaffected: it re-evaluates whatever the policy returns and is
  independent of how the policy decides.

## Correct fix — proposed, not implemented

Ask the platform instead of inferring. Full proposal:
`docs/HG_NET_02B_ROUTING_POLICY_PROPOSAL.md`. In outline: require the active `Network` to carry
`TRANSPORT_VPN`, read that network's `LinkProperties` routes and require a route covering the
destination, and require that no non-VPN network covers it. That is the platform's own routing
statement rather than an inference from interface addresses, and it handles `/32` peer routes
correctly.

It costs `ACCESS_NETWORK_STATE` — a normal, non-dangerous permission — which **changes the
operator-approved permission set from three to four**. That is an operator decision, so it is
proposed rather than taken.

Until it lands the containment check is **unverified against the deployed overlay**, and the bridge
would refuse a legitimate coordinator. It fails closed (refuses rather than leaks), so this is a
functional defect and a Phase 3 blocker, not a security regression.

## Historical exposure — not undone by any later commit

An earlier revision of this file (commit `65248a1`) included, in a public repository:

* the Oracle control-plane endpoint host and port;
* the WireGuard control-plane **public** key;
* the overlay `/32` addresses for the hub, the workstation peer and the intended device peer.

Assessment:

* **No private key was exposed.** Verified: neither the real peer private key nor the generated
  device private key occurs in any commit or tracked file, and no tracked file contains a non-empty
  `PrivateKey =` assignment. There is no credential compromise to remediate.
* The WireGuard public key is shareable by design, but publishing it alongside the endpoint and the
  overlay addressing discloses **topology**, which is what makes this worth correcting.
* **Rewriting history does not recall it.** `65248a1` was pushed; the identifiers remain in the
  repository's history and in any clone or fork taken before any rewrite. Removing the file in a new
  commit cleans the current tree only. Force-pushing is not used here — it is prohibited on this
  project without explicit owner approval — so the exposure is reported rather than erased.
* Also observed and **not authored by this work**: the same endpoint appears in
  `docs/ORACLE_RELAY_H04_PLAN.md` and `scripts/oracle_preflight_readonly.sh` on this branch, and on
  the published evidence branch `experiment/hg-cam-08d-evidence`. Those are separate files with
  separate authorship; they are reported for the owner to schedule and are not edited here.

Owner decision required: whether to rotate the control-plane WireGuard keypair, and whether to treat
the endpoint address as compromised. Both are infrastructure actions outside this branch.

## Not claimed

No Android routing proof exists. No tunnel is up on any device or host in this session, the Mi A3
has no VPN configured, and the Oracle host currently has **no running HomeGuardian coordinator
listener**. Nothing has been installed, no app data changed, and VPN and remote MCP are not working.
