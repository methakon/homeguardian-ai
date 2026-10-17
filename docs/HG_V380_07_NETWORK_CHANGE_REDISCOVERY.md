# HG-V380-07 — Post-Network-Change Rediscovery and Permanent Multi-Network Design

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Milestone base commit named in the brief:** `2784d770b475ae50d34e27e8c3cf7909322c3f5e`
**Actual HEAD when this ran:** `28c249d7282bec05cc9b5b3166998a79abdb0d6b` (the brief's base is one commit behind; nothing in that commit touches networking)
**Camera under test:** second camera only, reported at `<redacted-nat-host-164>`

---

## 0. ANSWER FIRST

**The owner's network change is not visible from this workstation, and the second camera is
still not reachable.**

Measured, not assumed:

| Question | Answer |
|---|---|
| Is the workstation on Tenda's `kusankurdhar` network? | **No — it remains on Airtel** (`<redacted-lan-host-6>/24`, default via `<redacted-lan-host-1>`, SSID `<redacted-wifi-ssid>`, BSSID `<redacted-mac-g>`) |
| Is the second camera reachable at `<redacted-nat-host-164>`? | **No** — 100 % ICMP loss, every probed port filtered (timeout, not refusal) |
| Is the extender still NAT'ing? | **Yes, as far as anything here can tell** — `<redacted-nat-host-0>/24` is still not on-link, still not routed, and the extender still broadcasts `kusankurdhar` from `<redacted-lan-host-3>` |
| Did any new LAN client appear? | **No** — the neighbour table holds exactly the same six entries as before the change |
| Did the workstation gain a second path? | **No** — `enp5s0` is still `NO-CARRIER`, no cable |

So **Stage 2 is BLOCKED on reachability, not on capability**, and I could not confirm the
reported change took effect on any path I can observe. Two owner-side facts are needed to
proceed (§6): what was actually changed, and the address the camera now holds.

Nothing was reconfigured. The first camera's ONVIF state was re-verified as still disabled.

---

## 1. Stage 1 — the actual current network state (measured 2026-10-17 20:04–20:12 IST)

### 1.1 Interfaces, routes, subnets

```
wlp6s0   UP    <redacted-lan-host-6>/24      SSID <redacted-wifi-ssid>, BSSID <redacted-mac-g> (2.4 GHz, ch 6)
enp5s0   DOWN  <NO-CARRIER>        MAC <redacted-mac-h>   (no cable)
docker0 / br-412d2fc93c32          local container bridges, unrelated

default via <redacted-lan-host-1> dev wlp6s0 proto dhcp src <redacted-lan-host-6> metric 600
directly connected: <redacted-lan-host-0>/24 (wlp6s0), 172.17.0.0/16, 172.18.0.0/16
nothing on <redacted-nat-host-0>/24
```

### 1.2 The workstation did **not** move to the Tenda network

`nmcli dev status` → `wlp6s0:wifi:connected:<redacted-wifi-ssid>`. The DHCP lease is unchanged
(`<redacted-lan-host-6>/24`, gateway and DNS `<redacted-lan-host-1>`).

### 1.3 The extender and its network are still there

- `kusankurdhar` is still broadcast by BSSID **`<redacted-mac-c>`** on channel 6
  (RSSI 27). Note it took **three rescans** to appear again — the same flakiness recorded in
  `HG_V380_05`; one scan is not evidence of absence, from either the laptop or the phone.
- The Tenda host `<redacted-lan-host-3>` (same MAC) is still on the Airtel LAN: it does not answer ICMP,
  but its ARP entry resolves `REACHABLE`, so the device is alive and powered.
- It still exposes **no** TCP port from the Airtel side (checked 80/443/53/8080/8443), exactly
  as before.

### 1.4 Neighbour table — no new client

```
<redacted-lan-host-1>   <redacted-mac-g>   ZTE F670L V9.0 gateway
<redacted-lan-host-3>   <redacted-mac-c>   Tenda extender (kusankurdhar)
<redacted-lan-host-4>   <redacted-mac-f>   unidentified (no open ports, replies to ping)
<redacted-lan-host-14>  <redacted-mac-e>   Mi A3 (randomised MAC)
<redacted-lan-host-16>  <redacted-mac-d>   Panasonic ELUGA I7
<redacted-lan-host-19>  <redacted-mac-a>   camera 1
```

Identical to the set recorded in `HG_V380_05`. **A note on what this proves:** an ARP table
only lists hosts this machine has exchanged traffic with, so it cannot prove a newly connected
camera is absent — only that nothing has announced itself and no new address was observed.

### 1.5 Protocol-level discovery found nothing (and this was not a host sweep)

Every probe below is a single-cast/multicast protocol query, not a subnet scan:

| Discovery | Result |
|---|---|
| SSDP `M-SEARCH ssdp:all` → 239.255.255.250:1900 | **no responder** |
| mDNS PTR `_services._dns-sd._udp.local` → 224.0.0.251:5353 | **no responder** |
| ONVIF WS-Discovery `Probe`/`NetworkVideoTransmitter` → 239.255.255.250:3702 + broadcast | **no responder** |
| Targeted probe of `<redacted-nat-host-164>` (ICMP + 11 camera ports) | **all filtered, no answer** |
| Targeted probe of the two unexplained hosts (`.3`, `.4`) on 554/8899/8800/9800/80/443 | **no open ports on either** |

**Conclusion for Stage 1: the topology I recorded in `HG_V380_05` still holds.** The extender
is still a NAT'ing router at `<redacted-lan-host-3>` serving its own `<redacted-nat-host-0>/24`, and `<redacted-nat-host-164>`
is still behind that boundary. **If the owner changed something, it was not on the path this
workstation can see** — see §6 for what would make the change observable.

## 2. Stage 2 — second camera inspection: BLOCKED

Not performed, because there is no reachability to inspect. Stated plainly so it is not
mistaken for a negative result:

| Requested check | Status |
|---|---|
| Confirm network-level camera identity | **NOT POSSIBLE** — no path |
| Standard RTSP (554) / ONVIF (8899) interfaces | **NOT TESTED** |
| Whether firmware supports authenticated local media access | **NOT TESTED** |
| Absent ONVIF setting vs disabled interface | **CANNOT BE DISTINGUISHED REMOTELY** — see below |
| Anonymous private video or control exposure | **NOT TESTED** — and per the rule below, this is the first thing tested once reachable |

Two honest notes on the firmware question:

- **"Absent setting" and "disabled interface" are only distinguishable from the device.**
  A missing *switch* in the app is documented by the vendor as meaning the firmware cannot
  support the feature; a present-but-off switch means the firmware supports it and it is
  simply not enabled. From the network side both look identical — nothing listening — which is
  why the app screenshot is the decisive evidence and why `HG_V380_06` insists on classifying
  rather than declaring.
- **The `<redacted-cam2-software-family>` family is not the first camera's `<redacted-sw-token>` family** and nothing measured
  on camera 1 is carried over. The only existing data point for this family is a public
  reverse-engineering project whose premise is that these cameras encrypt their stream and
  expose no native RTSP/ONVIF — a **hypothesis to test**, not a finding.

**Rule for when it becomes reachable (unchanged from `HG_V380_06`):** run the access-control
check **before** any media work. If any interface is EXPOSED, stop media integration and report
it. No decryption, no bypass, no password guessing — those remain prohibited.

## 3. Stage 3 — permanent multi-network design (no changes applied)

### Option A — Tenda in bridge/AP mode

The camera keeps associating wirelessly through the Tenda but receives an address on the Airtel
LAN (`<redacted-lan-host-0>/24`).

- **Routing behaviour:** confirmed simple — one broadcast domain, camera gets a DHCP lease from
  the Airtel router; reachable by every authorised node with no NAT traversal.
- **Disruption:** highest of the three. Every current client of `kusankurdhar` is re-addressed
  and drops for the duration; the extender's own `<redacted-nat-host-1>` admin plane disappears; anything
  that has pinned a `<redacted-nat-subnet>.x` address breaks until it re-leases. Requires admin access and
  **owner approval**, ideally with the owner present.
- **Security consequence, stated plainly:** it puts the camera **on the same subnet as every
  other household device**, which is the opposite of what `HG_V380_06` recommends, because this
  camera family offers no verified way to make its media interfaces require authentication. If
  Option A is chosen, isolation must be reintroduced another way (a camera-only VLAN, or a
  dedicated SSID with client isolation plus firewall rules).

### Option B — keep the Tenda in router mode, reach it through a local dual-network adapter (RECOMMENDED)

The Tenda already provides isolation for free: its `<redacted-nat-host-0>/24` is unreachable from the
Airtel LAN, and nothing can reach in from upstream. Give **one** trusted host a second interface
on that segment:

- **Mechanism on this hardware:** one Ethernet cable from a LAN port of the Tenda to the
  workstation's idle `enp5s0`. This is a **zero-configuration** change — no router, extender or
  firewall setting is touched, and Wi-Fi keeps the Airtel address and default route, so both
  networks are reachable simultaneously and routing is unambiguous (a directly connected
  `<redacted-nat-host-0>/24` route via `enp5s0`, everything else via Airtel).
- **Routing behaviour:** confirmed by construction, and verifiable in one command
  (`ip route get <redacted-nat-host-164>` must resolve via `enp5s0`, not the default gateway).
- **Disruption:** none to any other device. The extender is not reconfigured.
- **Security:** this *is* the isolated-segment design from `HG_V380_06` §4 — the camera is
  reachable by exactly one host, which then enforces consent, per-payload authorization and a
  default-DROP firewall on the camera-facing interface, with no port forwarding and no
  internet route. The camera's own missing authentication stops mattering because the client
  population is one, and it is ours.

### Option C — future remote viewing

Displaced in time and unchanged in shape: authenticated private connectivity (the approved
VPN/HTTPS gateway) with **direct, authorised media paths** to the HomeGuardian node. The Oracle
VPS remains a remote-access gateway only — no default video processing, no transcoding, no
storage, no application-level media relay. Nothing in this milestone enables it, and the
release-security gate for real-camera remote access remains mandatory.

### Recommendation

**Option B now, verified by measurement, with Option A available later as a consolidation the
owner may prefer.** Option B is the lowest-disruption change (one cable, nothing reconfigured),
it does not disturb a single other device, and it is simultaneously the security architecture —
whereas Option A, chosen for convenience, would put an unauthenticatable camera on the same
subnet as the whole household.

## 4. Stage 4 — first camera state preserved

Re-verified during this milestone, configuration untouched:

```
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899
  RTSP/554    DISABLED   no RTSP listener (connection refused)
  ONVIF/8899  DISABLED   no ONVIF listener (connection refused)
```

**The disabled ONVIF state is preserved**, exactly as the brief requires. Nothing was enabled,
disabled or altered on the first camera.

## 5. Nothing was changed anywhere

| System | Action |
|---|---|
| Airtel router | untouched — no configuration read or written |
| Tenda extender | untouched — no mode change, no admin access attempted |
| Both cameras | untouched — no setting changed |
| Firewall / any host rules | untouched |
| Oracle, Cloudflare, either VPS, mail | untouched |
| Security workstream / audit branch | untouched |
| Android devices | read-only adb queries only |

## 6. What is needed to proceed (owner-side)

I can only see the path this workstation is on, and that path is unchanged. To make the change
observable, **any one** of the following closes it:

1. **Tell me what changed and where the camera now is** — specifically the LAN address the
   **V380 Pro app now shows for the second camera** (the app's device detail page carries it).
   That is the single most useful piece of information and it costs you one screen.
2. **Cable the Tenda's LAN port to this workstation's Ethernet port** (`enp5s0`,
   `<redacted-mac-h>`, currently NO-CARRIER). No configuration changes anywhere; the camera
   becomes directly reachable and Stage 2 can run immediately.
3. **Or read the camera's address off the Airtel router's DHCP client list** and pass it on
   (the router admin needs your password; I did not access it).

If instead the change was to put the extender into bridge mode, note that the camera would then
appear on `<redacted-lan-host-0>/24` — and `kusankurdhar` would normally still broadcast, as it does now,
so I would need its new Airtel-assigned address from one of the three sources above.

## 7. Evidence paths

| Artifact | Path / command |
|---|---|
| Reachability verdict for the second camera | `bash scripts/hg_second_camera_check.sh <redacted-nat-host-164>` |
| Access-control classification (camera 1, preserved state) | `bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899` |
| Checker self-test (proves the classifier can fail) | `bash scripts/v380_security_check_selftest.sh` |
| Prior topology evidence | `docs/HG_V380_05_SECOND_CAMERA_EXTENDER_TOPOLOGY.md` |
| Exposure closure + auth options | `docs/HG_V380_06_EXPOSURE_CLOSURE_AND_AUTH_CONTROLS.md` |
| Wi-Fi environment | `nmcli -t -f BSSID,SSID,CHAN,FREQ,SIGNAL dev wifi list` (after ≥3 rescans) |

## 8. Status

| Stage | Status |
|---|---|
| 1 — actual new network state | **COMPLETE.** Workstation still on Airtel; extender still NAT'ing at `<redacted-lan-host-3>` broadcasting `kusankurdhar`; `<redacted-nat-host-164>` still unreachable; no new client; no second interface. **The reported change is not observable from here** |
| 2 — inspect the second camera | **BLOCKED — no reachability.** Nothing tested, nothing assumed |
| 3 — permanent design | **COMPLETE (analysis only).** Option B recommended: dual-network via the idle Ethernet port — zero-configuration, lowest disruption, and it doubles as the `HG_V380_06` isolated-segment design. Option A needs owner approval plus a re-introduced isolation mechanism. Option C deferred and unchanged |
| 4 — report + preserve | **DONE.** First camera's disabled ONVIF state re-verified and preserved; no system touched |

**Verdict: no new information about the second camera is obtainable until reachability changes
on a path I can see. The recommended next step remains a single Ethernet cable into `enp5s0`
(no reconfiguration, no approval needed beyond plugging it in), or the camera's current address
from the app / router DHCP list.**
