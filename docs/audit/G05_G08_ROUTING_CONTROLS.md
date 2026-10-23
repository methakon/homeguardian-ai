# G05 / G08 — Android routing controls review (mission step 6)

**Question:** does private-endpoint validation substitute for verified VPN routing, and what exactly
happens on socket binding, DNS change, network change, reconnect and failure?

---

## 1. The project already made this correction — and documented why

`TransportPolicy` tried to prove "this destination travels over the tunnel" by **subnet containment over
`NetworkInterface`**. That approach fails against the deployed overlay, which addresses each peer with a
**/32 host route**: a tunnel interface's own address then contains only itself, so containment can never
reach the peer. It also could not see per-peer routes at all, because Android's `VpnService` installs
them in the **VPN's own routing table**, not on the interface.

`RouteSecurityPolicy` replaces it and asks the platform, which knows the actual answer: it works from
**per-network routes as Android reports them via `LinkProperties`**, a representation that carries
thin-prefix routes natively. The decision is a pure function over an injected `PlatformRouting` view, so
every branch — including the /32 shape and states a test host cannot produce (no network, VPN down,
overlapping routes) — is testable **off-device**.

**Answer to the question posed:** no, private-endpoint validation is not being used as a substitute.
Reachability is decided from platform routing state, and the /32 case that broke the containment
approach is exactly what the replacement handles.

## 2. Socket binding — what makes the check binding rather than advisory

From `RouteSecurityPolicy.SocketBinder`:

> Evaluating routes proves where a packet **SHOULD** go; binding the socket proves where it **CAN** go,
> because the kernel then uses that network's routing table for this socket. If the VPN has gone away
> the bind fails or the subsequent connect fails — it does not silently fall back to Wi-Fi or cellular,
> which is the requirement. Without this, a routing change between the check and `connect()` would send
> the credential over whatever interface won the lookup.

Two properties fall out of this design:

- a **throwing binder is caught** and becomes a refusal rather than an escape route;
- the app does **not** rely on the route decision alone — the socket is pinned to the VPN network, so a
  race between decision and connect cannot leak the credential onto another interface.

## 3. DNS changes

`TransportPolicy`'s resolver is injectable, and resolution happens on **every** `refusalReason()` call
with **no caching**, so the reconnect loop's repeated checks cannot be satisfied by a stale answer. The
DNS-change case is therefore testable for real instead of being argued from a cached address.

## 4. Network changes, reconnect and fail-closed behaviour

| Behaviour | Mechanism | Fail-closed? |
|---|---|---|
| VPN disappears mid-session | socket bind fails, or the subsequent connect fails | **Yes** — no silent fallback to Wi-Fi/cellular |
| Platform routing unavailable | `AndroidPlatformRouting` reports unavailable and the policy **refuses every non-loopback destination** | **Yes** — refuses rather than falling back to the known-wrong containment logic |
| Coordinator revokes the device | a **4403** response sets `revoked` and **suppresses reconnect** | **Yes** |
| No coordinator configured | bridge start refused; state reads `refused - no coordinator configured` | **Yes** |
| Request flood | at most **8 in-flight**; excess refused, never queued without bound | **Yes** |
| Engine dies / probes fail | `EdgeService` watchdog probes MCP over loopback; bounded restart with exponential backoff (`5s * 2^(n-1)`, capped at 60 s, limited attempts) | Bounded, logged |

## 5. Discrepancy found in the source comments — correct it before release

`RouteSecurityPolicy`'s class comment states:

> `ACCESS_NETWORK_STATE` … is **NOT yet in the manifest**: it takes the operator-approved set from three
> to four … Until it is granted, `AndroidPlatformRouting` reports the platform as unavailable and this
> policy **REFUSES every non-loopback destination**.

That is **stale**. The candidate APK declares **four** permissions including
`android.permission.ACCESS_NETWORK_STATE` (verified by `aapt2 dump badging` on the shipped artifact), and
commit `8e6ee3f` records that permission as operator-approved. A reviewer reading the comment would
wrongly conclude the app is currently refusing all non-loopback traffic. The permission is present, so
route decisions are made from real `LinkProperties` state.

**Smallest correction:** update that paragraph to record that the permission is present and approved, and
that the refusal path is now a fallback for a genuinely unavailable platform, not the current operating
state.

## 6. Untested hardware behaviour — labelled as such

- End-to-end **VPN routing on the physical Mi A3**: untested. No VPN consent granted, no peer enrolled.
- Socket-bind behaviour under **real** network transitions (Wi-Fi↔cellular, VPN teardown) on device: untested.
- Reconnect against a **real** coordinator, and the 4403 revocation path observed on hardware: untested.
- On-device DNS-change behaviour: untested.
- The off-device tests over synthetic network states are **not** physical acceptance and must not be
  reported as such.
