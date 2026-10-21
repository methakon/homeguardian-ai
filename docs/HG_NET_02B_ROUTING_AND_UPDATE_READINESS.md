# HG-NET-02B — Platform routing policy, operator permission review, safe update, MCP health

Contains no endpoint addresses, peer keys or overlay subnets; all test values are RFC 5737 ranges.

---

## 1. Platform routing policy (designed and independently tested off-device)

`apk_service/src/org/homeguardian/edgeservice/RouteSecurityPolicy.java`

Replaces inference-from-interfaces with a query to the platform, which actually knows the routing
decision. A destination is acceptable only when all of these hold, re-evaluated on every dial
attempt and once per heartbeat:

| Clause | Requirement | Why |
|---|---|---|
| P1 | at least one network has `TRANSPORT_VPN` | the former "a tun/wg interface is up" was only a proxy for this |
| P3 | **no non-VPN network's routes cover the destination** | checked before P2, so an overlap is refused rather than trusted |
| P2 | a VPN network's routes cover the destination | the platform's own statement that this address is reachable through the tunnel; **this is the clause that handles thin-prefix routes**, which subnet containment could not |

Loopback is exempt from all of it, including availability: it never leaves the device.

**Why an accepted destination uses the approved VPN.** P1 says a VPN exists, P2 says the platform
routes this destination through it, and P3 says no other attached network claims the destination, so
there is no competing route. The remaining gap is the check-then-connect race, closed by binding the
socket to the VPN `Network` (P6 in the proposal) — the strongest single mitigation and the one that
makes the route checks meaningful rather than advisory.

**Limitations that cannot be settled without the device:** P2/P3 are the platform's claims, not a
packet-level guarantee; per-app VPN exclusion is not enumerable from inside a sandbox and is only
detected indirectly; and nothing here is verified on hardware, which remains outstanding.

### Test results — `scripts/bridge_ws_compat/RouteSecurityPolicyTest.java`

**38/38 PASS** against the real class, driven by synthetic platform states. Synthetic because the
states that matter cannot be produced on a test host at all (no active network, VPN and LAN routing
the same destination, thin-prefix overlay). Every group asserts both directions, since a policy that
refused everything would pass a refusals-only suite.

| Group | Result |
|---|---|
| A. **thin-prefix overlay — the regression** | hub on a `/32` route now **ALLOWED** (the old rule refused it); unrouted neighbour refused; LAN host refused |
| B. normal shared prefix | overlay `/24` allowed; outside refused; loopback v4/v6 allowed |
| C. adversarial | VPN+LAN both covering ⇒ **refused, LAN checked first**; public destination not VPN-routed ⇒ refused; `wss://`, `http://`, empty ⇒ refused; DNS re-pointed overlay→LAN and overlay→public between calls ⇒ refused; dual-homed ⇒ refused; resolution failure ⇒ fails closed |
| D. disconnected/degraded | no networks ⇒ refused; non-VPN only ⇒ refused; loopback still allowed offline; **recovers when the VPN network returns**; permission absent ⇒ **fails closed**; no platform wired ⇒ fails closed |
| E. full tunnel | VPN with a default route covers public addresses; LAN still refused |
| F. prefix arithmetic | `/32`, `/24`, `/31`, `/30`, `/0`, family mismatch |

Writing these found a **real bug in the new policy**, now fixed: the platform-availability check ran
before the loopback exemption, so loopback was refused whenever the permission was absent — breaking
local use for no security gain. The test asserted loopback must stay allowed and caught it.

---

## 2. Operator permission review — ACCESS_NETWORK_STATE

**NOT YET ADDED.** Verified absent: `grep -c ACCESS_NETWORK_STATE AndroidManifest.xml` returns 0, and
the manifest still declares exactly three permissions.

The policy reads `ConnectivityManager`, which requires this permission. Until it is granted,
`AndroidPlatformRouting.available()` reports false and the policy **refuses every non-loopback
destination** — deliberately, rather than silently falling back to the containment logic known to be
wrong. No fallback path exists.

The change is one line:

```xml
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
```

Review points:

| Question | Answer |
|---|---|
| Permission class | normal — **no runtime prompt**, no user dialog |
| Capabilities granted | read the device's network topology and routing tables only |
| What it does NOT grant | no network access beyond the existing `INTERNET`; no ability to change networks; no location |
| Set change | three permissions → four |
| Operator-visible effect | the app can see which network routes where — the whole point |
| Reversible? | yes — remove the line and behaviour reverts to fail-closed refusal |

Per the standing instruction this is proposed, not applied. **It blocks the fix**: without it the
bridge cannot dial anything but loopback.

---

## 3. Safe APK update procedure

Verified against the packaged artifact, not the source:

```
currently installed ..... org.homeguardian.edgeservice  1.0.0 (versionCode 1)
candidate .............. 1.0.1 (versionCode 2)
candidate sha256 ....... 5471b296c1d5e2e803af005a207d621783bf82858d75d3e2e7494aa57855a362
signer ................. 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
installed app signer ... identical  -> in-place -r update is signature-compatible
debuggable ............. absent      permissions ... unchanged (3)
EdgeService ............ exported=false in the packaged manifest
production APK preserved 2d6018e0afa4c7250b621efcb4194dc3d964c4895e18cc043738644d03a47246
```

Sequence, when approved: (1) confirm the phone is idle and `pid` unchanged; (2) `adb install -r` the
candidate; (3) verify `versionName=1.0.1`, `pid` changed, service foreground, MCP still bound to
loopback; (4) rollback plan is the preserved `2d6018e0…` APK — note **downgrading 1.0.1 → 1.0.0 needs
`adb install -r -d`**, because Android refuses a versionCode decrease without it.

Data preservation is by mechanism (same signature, same package, higher versionCode), not by
execution: no install has been performed, so it is not empirically demonstrated.

---

## 4. Local C++ MCP health verification

The genuine engine (`hg_mcp_server`, sha256 `1656fd2b182e…`), launched on loopback and exercised:
loopback-only binding; `initialize` returning `serverInfo {homeguardian-mcp, 1.0.0}`; `tools/list`
returning 2 tools; `get_homeguardian_status` → `{"capture_enabled":true,"read_only":true,
"state":"Running"}`; `get_alert_summary` → `{"alert_count":0,...,"read_only":true}`; negatives
**400** (no session) and **401** (wrong token); clean shutdown, port released. All PASS, and the
production service was not touched.

---

## 5. Owner decisions required

1. **ACCESS_NETWORK_STATE** — the single blocking approval (section 2).
2. **Rotate/re-address** following the `65248a1` disclosure; sanitise
   `docs/ORACLE_RELAY_H04_PLAN.md`, `scripts/oracle_preflight_readonly.sh` and
   `experiment/hg-cam-08d-evidence` (separate authorship, not edited here).
3. **Reconcile the coordinator destination with Hermes 1 and Antigravity** — the Oracle host has no
   running HomeGuardian coordinator listener, so P2/P3 cannot be tested against reality and Phase 3
   has nothing to dial.
4. **Installation approval** — still withheld.

## 6. Not claimed

No device install, no app-data change, no VPN activation, no Android routing proof, no physical-device
PASS, no remote MCP. No new WireGuard phone key was generated. The native C++ application and its
validated MCP evidence are preserved and re-verified above.
