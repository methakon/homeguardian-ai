# HG-PANASONIC-SERVICE-TRANSPORT-06 — Stage 4: local Wi-Fi reachability plan

**Sanitized.** No live LAN addresses, MACs, SSIDs-as-identity or device serials appear here. The
observed values are recorded privately; this document is the reasoning and the plan.

## 1. What was observed (and how)

Measured with read-only ADB and host DNS tooling. No router, DNS, gateway, VPN or Wi-Fi setting was
changed.

| observation | result |
|---|---|
| node wlan0 subnet | `<NODE_SUBNET>/24`, gateway `<NODE_GW>` |
| hub wlan0 subnet | `<HUB_SUBNET>/24` |
| node -> hub ICMP | 2 sent, **0 received, 100% loss** |
| hub -> node ICMP | 2 sent, 2 received, **rtt 983–2049 ms** |
| node route table | only `<NODE_SUBNET>/24 dev wlan0` — no route toward the hub subnet |
| hub route table | only `<HUB_SUBNET>/24 dev wlan0` — no route toward the node subnet |

## 2. Interpretation

**The two handsets are not on one LAN.** They hold addresses in different /24s with different
gateways, and neither has a route toward the other's subnet, so any packet between them must leave
through its default gateway.

That single fact explains the asymmetry:
- **hub -> node works, but at ~1.5 s RTT.** Sub-millisecond is what a shared LAN gives. A 1.5-second
  round trip means the traffic is NOT staying on a LAN — it is being hairpinned through upstream
  routing. The path exists; it is not local.
- **node -> hub fails outright.** One direction resolving and the other not is the classic signature
  of upstream NAT or a filtering policy: the hub's gateway can reach the node's subnet, the node's
  gateway cannot reach (or will not forward to) the hub's subnet.

**Therefore the direction the architecture requires — the node reporting TO the hub — does not work
today.** That is a reachability failure, not an application failure. No amount of work inside the
transport changes it.

### Is it guest isolation, mobile data or a routing policy?

- **Not mobile data.** Both report `wlan0` with an IPv4 address and a default route via their gateway.
- **Guest/client isolation is consistent with the data** but is NOT proven. Isolation usually blocks
  both directions; here one direction works, which points more to an upstream routing rule than to
  per-AP client isolation. **Recorded as UNKNOWN rather than guessed.**
- **A routing policy (separate routers, or one subnet behind a second NAT) fits best**, and would
  produce exactly this asymmetric result.

The honest position: **cause UNKNOWN, effect measured.** Naming the cause needs router access, which
this milestone does not have and must not take.

## 3. Can both join one trusted LAN?

**Almost certainly yes, and it is the intended fix.** Both are ordinary Wi-Fi clients; nothing about
either device forces the split. Options, in preference order:

1. **Join both to one access point / one SSID on one subnet.** Simplest, no new trust surface, and it
   is what the contract assumes. This is the recommended path.
2. **One handset as a hotspot, the other as its client.** Works without touching the router, but makes
   the node's address unstable across reboots — acceptable for a test, poor as a permanent identity.
   Note the brief's rule: a development LAN address must never become device identity; the identity
   is the certificate, so a changing address is survivable.
3. **Add a route between the two subnets.** Requires router configuration — explicitly out of scope
   here.

**Not an option, under any circumstance:** port forwarding, a public endpoint, or a cloud relay.
§2.3 of the integration contract forbids them, and this design keeps the node dialling out with
nothing listening.

## 4. Can the Mi A3 accept the intended authenticated node connection?

Design-wise **yes**, with one caveat that must be resolved before that claim is tested:

- The hub side has an ingestion listener (`net/DeviceIngressServer`, mutual TLS via `net/TlsTransport`,
  client-certificate verification against the private CA, hard frame cap, bounded workers). The
  transport-level machinery exists.
- **`TlsServerConfig.bind_address` defaults to `0.0.0.0`.** For this deployment it must be bound to
  the specific LAN interface (or an explicit allowlist), never left at the wildcard default. That is
  a configuration decision to make deliberately, not a default to inherit.
- **The hub currently has no media session at all** — the integration contract records D-04
  (multi-node media) as TODO. So the hub can accept an authenticated *observation* connection today;
  accepting authenticated *media* needs D-04.

## 5. How the design avoids public access and the Oracle media path

Structural, not policy:

1. **The node dials out; nothing listens.** No `ServerSocket` on the node. Verified by test: the
   transport seams expose no `listen`/`bind`/`accept` method (isolated test 10).
2. **The only listener in the feature is loopback-only.** `MediaSink` binds `127.0.0.1:0` and is
   explicitly a local test double, not a network service.
3. **The transport is observations-only by construction.** The queue holds numbers and short strings;
   the serializer writes those fields and has no path for a frame, a sample or a transcript. Media
   therefore cannot reach Oracle because it cannot enter the transport.
4. **No router, VPS, Oracle, Cloudflare or DNS change** is needed for any of the above.

## 6. What this stage does NOT do

- No Wi-Fi, gateway, DHCP, DNS or VPN-profile change. None was authorised.
- No router access, no switch configuration, no firewall rule.
- No claim that wireless operation works. **It does not, yet** — see §2.

## 7. The unblocking action (owner)

Put both handsets on one LAN. Nothing else in this stage is actionable before that, and no code
change can substitute for it.
