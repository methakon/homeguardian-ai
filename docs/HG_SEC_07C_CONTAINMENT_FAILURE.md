# HG-SEC-07 Rev C follow-up — containment FAILS against the deployed overlay

Status: **FAIL.** The route-containment check does not work against the real deployed overlay
configuration. It is not verified, and it must not be represented as verified.

## What was tested

The reviewer asked for verification against the actual deployed overlay rather than synthetic
interface topologies. The deployed peer config is `~/.wireguard/hg-net-02a/wg0.conf`:

```
[Interface]
Address = 192.168.77.2/32          # peer
MTU = 1420
[Peer]
PublicKey = LHFiWRdBXc8fIXMh3aqgtcxbSBpgGneVYMkZLqIbi0s=
Endpoint = 161.33.146.238:51820
AllowedIPs = 192.168.77.1/32       # hub
PersistentKeepalive = 25
```

The overlay is addressed with **/32 host routes** — one address per peer — not a shared subnet.

## What happened

Evaluating the real values against `TransportPolicy` (measured, not inferred):

```
tun0 = 192.168.77.3/32  + wlan0 = 192.168.1.9/24        <- the REAL addressing pattern
  ws://192.168.77.1:18994/   REFUSE  "not inside any tunnel interface's subnet"
  ws://192.168.77.2:18994/   REFUSE  "not inside any tunnel interface's subnet"
  ws://192.168.1.50:18994/   REFUSE  (correct)

tun0 = 192.168.77.3/24  + wlan0 = 192.168.1.9/24        <- what my synthetic tests used
  ws://192.168.77.1:18994/   ALLOW   <- false confidence
  ws://192.168.77.2:18994/   ALLOW
  ws://192.168.1.50:18994/   REFUSE  (correct)
```

So the policy **refuses the hub it is supposed to reach** under the real addressing scheme. A
correctly deployed coordinator would be unreachable, and the operator would see a refusal reason
blaming the overlay.

## Root cause

`NetworkInterface` is the wrong source of truth for "does this destination travel over the tunnel".

* The deployed overlay gives each peer a `/32`, so a tunnel interface's own address contains only
  itself. Subnet containment can match nothing else on the overlay.
* Android's `VpnService` installs **per-peer `/32` routes** in the VPN's own routing table. Those
  routes are the actual reachability statement, and `NetworkInterface` does not expose them.

Containment-by-subnet was a reasonable inference from the interface list, and it happens to be
correct for a tunnel configured with a shared prefix — but that is not how the deployed overlay is
addressed. The synthetic tests used `/24` and therefore could never have caught this. That is
precisely the failure mode the reviewer warned about: a synthetic fixture that encodes an
assumption the deployment does not share, reported as passing.

## What is still believed sound

The LAN-egress rule (destination must not sit inside a non-tunnel interface's subnet) is unaffected:
it refused `192.168.1.50` correctly in both runs, and it depends only on the LAN interfaces' own
prefixes, which are real and shared. Defect 2 (reconnect and heartbeat fail-closed) is also
unaffected — it re-evaluates whatever the policy returns, and is now independent of how the policy
decides.

## Correct fix, not yet implemented

Ask the platform instead of inferring. On Android:

* obtain the active `Network` and require `NetworkCapabilities.hasTransport(TRANSPORT_VPN)`;
* read that network's `LinkProperties.getRoutes()` and require a route covering the destination;
* additionally require that the destination is **not** covered by a route on any non-VPN network.

That is the platform's own routing statement rather than an inference from interface addresses, and
it handles `/32` peer routes correctly. It costs `ACCESS_NETWORK_STATE` — a normal, non-dangerous
permission — which **changes the permission set the operator approves from three to four**. That is
an operator decision, not one to take unilaterally, so it is not implemented here.

Until that lands: the containment check is **unverified against the real deployment** and the bridge
would refuse a legitimate overlay coordinator. It fails closed (it refuses rather than leaking), so
it is not a security regression, but it is a functional defect and a blocker for Phase 3.

## Not claimed

No Android routing proof exists. No tunnel is up on any device or host in this session (host tunnel
interfaces: 0; Mi A3 has no VPN configured and is currently detached from USB). Nothing has been
installed, no app data changed, and VPN/remote MCP is not working.
