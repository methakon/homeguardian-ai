# HG-NET-02B — Revised route-security policy (design and off-device verification only)

Status: PROPOSAL. Not implemented. Nothing installed, no VPN activated, no physical-device claim.
This document contains no endpoint addresses, peer keys or overlay subnets.

The trigger: `docs/HG_SEC_07C_CONTAINMENT_FAILURE.md`. Route containment inferred overlay membership
from interface-subnet arithmetic, which fails for a thin-prefix (`/32`) overlay and is therefore
unverifiable against the deployed addressing pattern.

---

## 1. Proposed policy

Replace inference-from-interfaces with a query to the platform, which knows the actual routing
decision. A destination is acceptable only if all of the following hold, evaluated fresh on every
dial attempt and once per heartbeat:

**P1 — an active VPN network exists.**
`ConnectivityManager.getActiveNetwork()` (or the VPN network from `getAllNetworks()`) whose
`NetworkCapabilities.hasTransport(TRANSPORT_VPN)` is true. No VPN transport ⇒ refuse. This replaces
"some interface named tun/wg is up", which was only ever a proxy.

**P2 — the destination is routed by that VPN network.**
Read the VPN network's `LinkProperties.getRoutes()`. Require at least one route whose prefix covers
the destination. This is the platform's own statement that the address is reachable through the
tunnel, and it handles per-peer `/32` routes natively — the exact case containment could not express.

**P3 — no non-VPN network covers the same destination (LAN-egress guard, retained).**
For every other `Network` without `TRANSPORT_VPN`, require that none of its routes covers the
destination. This keeps the one property containment got right: the address must not be directly
reachable on an unprotected link. Using the platform's routes here is strictly better than the
interface-subnet arithmetic, because it reflects the real forwarding table.

**P4 — the app participates in that VPN.**
Confirm this process is actually covered: check the VPN network's `LinkProperties` includes an
address family we use, and that the app is not excluded. Per-app VPN is a real hazard — an operator
can configure the VPN to include selected apps only, in which case our traffic bypasses the tunnel
while the VPN is up. `ConnectivityManager` cannot enumerate another app's exclusion list, so this is
detected indirectly by P2 failing (our traffic is not covered by a VPN route) and by an explicit
diagnostic in the refusal reason. This is a known limitation, not a solved problem.

**P5 — DNS is not a bypass.**
Resolution happens on every check with no caching, so a name re-pointed from the overlay to a LAN or
public address is re-evaluated (already implemented and tested). Additionally, resolve through the
VPN network's own resolver (`Network.getPrivateDnsBootstrapServers`/socket factory bound to that
network) so a split-DNS change cannot silently redirect us. If binding a resolver to the network is
not available on the target API level, fall back to resolving normally and relying on P2/P3 to
reject whatever address comes back — which is the safe direction.

**P6 — socket network selection (defence in depth).**
Dial the WebSocket with the socket explicitly bound to the VPN `Network`
(`network.bindSocket(socket)` or `network.getSocketFactory()`). Then the kernel cannot pick another
interface even if routing changes between the check and the connect. This closes the check-then-connect
race that any policy evaluated before `connect()` otherwise leaves open.

**P7 — tunnel loss (retained, already implemented).**
Re-evaluate before every dial attempt and inside the heartbeat; hold rather than dial while refused,
and tear the session down on mid-session loss. P1–P3 now feed this with real data.

Ordering matters: P3 (LAN egress) is checked before P2 (on-overlay), so an ambiguous overlap is
refused rather than trusted.

---

## 2. Why an accepted destination uses the approved VPN — and what cannot be proven off-device

**Argument.** P1 establishes that a VPN network exists and is active. P2 establishes that the
platform itself claims a route for the destination via that VPN. P6 makes the kernel use that network
for this socket, so the choice is not left to a routing lookup that could change underneath us. P3
establishes that no other attached network claims the same destination, so there is no competing
route to win. Together these mean the only path offered to this socket is the tunnel.

**Limits — stated because they are real:**

* **P2/P3 are the platform's claims, not the kernel's final decision.** They are as authoritative as
  Android exposes to a sandboxed app, but they are not a packet-level guarantee.
* **P4 cannot be fully verified.** Per-app VPN inclusion/exclusion is not enumerable from inside
  another app. A misconfiguration is caught only indirectly.
* **P6 is the strongest single mitigation** and is what actually removes the race; the route checks
  are what make the destination legitimate in the first place. Neither substitutes for the other.
* **None of this is proven on hardware.** There is no VPN on the Mi A3, the Oracle host has no
  running coordinator listener, and nothing will be installed. Off-device work can verify the
  decision logic against synthetic `Network`/`LinkProperties` states; it cannot verify that Android
  behaves as modelled. That step requires the device, the tunnel and owner consent.

---

## 3. Regression cases to write (off-device, synthetic platform state)

Extends the existing 37-case suite; the current suite is retained, since the LAN-egress and
fail-closed cases remain valid.

| Group | Cases |
|---|---|
| **Thin-prefix overlay (the regression that motivated this)** | tun `/32` peer + VPN route `<hub>/32` covering the coordinator ⇒ **ALLOW** (containment refused this); tun `/32` with no covering route ⇒ REFUSE; coordinator equal to the peer's own `/32` ⇒ ALLOW only if a route covers it |
| **Normal** | distinct `/24` overlay ⇒ ALLOW (must keep working — the old fixtures' case); loopback ⇒ ALLOW without VPN; VPN up and route covering ⇒ ALLOW |
| **Adversarial** | VPN route and LAN route both cover the destination ⇒ **REFUSE** (P3 before P2); VPN network present but our destination covered by no VPN route ⇒ REFUSE (P4 signal); public destination with a VPN route present ⇒ REFUSE; tunnel sharing the LAN prefix ⇒ REFUSE; dual-homed DNS name ⇒ REFUSE; DNS re-pointed overlay→LAN and overlay→public between calls ⇒ REFUSE |
| **Disconnected** | no active network ⇒ REFUSE; VPN network present but not `TRANSPORT_VPN` ⇒ REFUSE; VPN network torn down between check and connect ⇒ socket bind fails ⇒ session refused, no fallback to another interface; tunnel loss mid-session ⇒ session closed, no redial while refused, resumes when restored |
| **Boundary** | `/32` covers exactly one address; `/31`, `/30`, `/0`; v4/v6 family mismatch; route present with a different family |

All fixtures use RFC 5737 / RFC 3849 documentation ranges and synthetic `Network` objects. **No
deployment address, endpoint or key is committed**, and the real-config harness
(`scripts/bridge_ws_compat/RealOverlayCheck.java`) takes its values as arguments.

---

## 4. Risk matrix

| # | Risk | Likelihood | Impact | Mitigation | Residual |
|---|---|---|---|---|---|
| R1 | Policy accepts a destination that actually egresses unprotected | Low | High — credential on an open link | P6 binds the socket to the VPN network, closing the check-then-connect race; P3 rejects LAN-covered destinations | Low; not packet-proven on hardware |
| R2 | Per-app VPN excludes our app, so traffic bypasses the tunnel while "VPN up" | Medium | High | P2 fails when no VPN route covers us; explicit diagnostic; P6 bind fails when the app is not in the VPN | **Open.** Not fully detectable from the sandbox |
| R3 | False refusal blocks a legitimate coordinator (the current defect) | **Certain today** | Medium — Phase 3 blocked | P2 accepts `/32` peer routes, which containment could not | Low once implemented |
| R4 | `ACCESS_NETWORK_STATE` widens the approved permission set | Certain | Low–Medium — operator-visible change | Single normal (non-dangerous, no runtime prompt) permission; declared and justified; approval sought before adding | Accepted by decision, not by default |
| R5 | Check-then-connect race | Medium | High | P6 network-bound socket | Low |
| R6 | DNS change redirects to a LAN or public address | Low | High | No caching; re-resolve per check; P3 rejects; resolver bound to VPN where available | Low |
| R7 | Implementation drift between model and Android behaviour | Medium | Medium | Off-device tests model the API contract; device validation required before any PASS claim | **Open until hardware** |

---

## 5. Owner approvals required

1. **Add `ACCESS_NETWORK_STATE` to the Android manifest.** This takes the approved permission set
   from three to four. It is a normal permission (no runtime prompt, no dangerous capability), but it
   is a visible change to what the app requests, so it is not taken unilaterally. **Blocking the
   whole fix** — without it, P1–P3 are not implementable as proposed.
2. **Rotate the control-plane WireGuard keypair and/or re-address the endpoint**, following the
   exposure recorded in `docs/HG_SEC_07C_CONTAINMENT_FAILURE.md`. Infrastructure action, outside this
   branch.
3. **Schedule the same sanitisation** for `docs/ORACLE_RELAY_H04_PLAN.md`,
   `scripts/oracle_preflight_readonly.sh` and the published branch
   `experiment/hg-cam-08d-evidence`, which carry the endpoint and were not authored by this work.
4. **Confirmation of the real coordinator destination.** The Oracle host currently has **no running
   HomeGuardian coordinator listener**, so there is no endpoint to point the bridge at and no way to
   test P2/P3 against reality. Needs reconciliation with Hermes 1 and Antigravity.
5. **Installation approval**, still withheld: no APK install, no app-data change, no VPN activation.

---

## 6. Explicitly not claimed

No Android routing proof. No physical-device PASS. No install. No VPN connection. No remote MCP. The
native C++ application and its validated MCP evidence are untouched by this proposal and remain as
recorded in `docs/HG_NET_02B_TRANSPORT_AND_MCP_DEMO.md`.
