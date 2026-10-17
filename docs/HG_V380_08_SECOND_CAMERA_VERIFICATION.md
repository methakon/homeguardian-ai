# HG-V380-08 — Second Camera Verified on the Airtel Subnet

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-ec39-integration`
**Reference commit:** `ca0ac8c5cac73d53a438ca8d4f6fa2d693d092e9`
**Camera under test:** camera 2, now reported by the V380 Pro app at **`<redacted-lan-host-20>`**
(previous `<redacted-nat-host-164>` superseded)
**Scope note:** this same-subnet connection is a **development arrangement**. The
network-independent multi-site design in `docs/HG_NET_ARCH_01_*.md` is unchanged and does not
depend on this subnet.

---

## 0. RESULT FIRST

| Question | Answer |
|---|---|
| Reachable from the workstation? | **YES** — directly on-link, no NAT in the path, `0 %` packet loss |
| Identity correlated? | **YES** — MAC `<redacted-mac-b>`, OUI **AltoBeam Inc.** (the same Wi-Fi silicon vendor as camera 1), distinct MAC and a different firmware family |
| Standard local interfaces? | **NONE.** Only `tcp/8800` and `tcp/9800` listen — identical to camera 1's as-shipped surface |
| Unauthorized media exposure? | **None found on any documented interface.** RTSP `DISABLED`, ONVIF `DISABLED`; no ONVIF discovery responder; the vendor port yields nothing without a login handshake |
| Secure local media interface for integration? | **DOES NOT EXIST on this firmware** — so real capability measurement (§5) is **not possible**, and no adapter should be built against a non-existent interface |
| Camera 1 kept separate, ONVIF disabled? | **YES** — re-verified `DISABLED`, untouched |

**The verdict is not "insecure" — it is "no standard interface at all".** There is nothing to
authenticate because there is nothing listening, and that is precisely why this camera cannot be
integrated through RTSP/ONVIF today.

---

## 1. Reachability (step 1)

```
ip route get <redacted-lan-host-20>   ->  <redacted-lan-host-20> dev wlp6s0 src <redacted-lan-host-6>     (directly connected)
ping -c 3                   ->  3/3 received, 0% loss, rtt avg 5.2 ms, ttl 255
```

The route names the **local interface**, not the default gateway: `<redacted-lan-host-20>` is on the same
`/24` as the workstation, so there is **no NAT boundary** between them — a complete change from the
`<redacted-nat-host-164>` situation, where every packet left for the ISP.

## 2. Device identity correlation (step 2 — done BEFORE touching services)

| Evidence | Value |
|---|---|
| ARP neighbour | `<redacted-lan-host-20>` → `<redacted-mac-b>` (`REACHABLE`) |
| OUI vendor (IEEE registry) | **AltoBeam Inc.** |
| Camera 1's OUI for contrast | `<redacted-mac-prefix>…` → **also AltoBeam Inc.** |
| MAC comparison | **different** (`<redacted-mac-prefix>…` vs `<redacted-mac-prefix>…`) → two distinct devices, not one camera seen twice |
| Reported network | `<redacted-wifi-ssid>`, Wi-Fi station, DHCP (owner screenshots) |
| Firmware family | `<redacted-cam2-software-family>` / `<redacted-cam2-firmware>` (owner-supplied) vs camera 1's `<redacted-cam1-software-family>` / `<redacted-cam1-firmware>` |

Both cameras using AltoBeam Wi-Fi silicon is expected and is **not** an identity: the OUI
identifies the radio vendor, and it is shared across the family's units. The distinct MAC plus the
owner's app report plus the port signature together identify this device; no single one of them
would.

**Nothing from camera 1 was carried over.** Different firmware family, different port behaviour
(§4), separate evidence.

## 3. Interface inventory (step 3 — documented ports only)

Only the documented RTSP, ONVIF, V380-vendor and HTTP-management candidates were probed, per the
instruction. No full-range sweep was run.

| Port | Documented as | Result |
|---|---|---|
| 554 | RTSP | **refused** (errno 111) |
| 8554 | RTSP alternate | **refused** |
| 8899 | ONVIF | **refused** |
| 3702/TCP | ONVIF discovery (TCP) | **refused** |
| 3702/UDP | ONVIF WS-Discovery | **no responder** (multicast and unicast probe) |
| **8800** | V380 vendor control/media | **OPEN** |
| **9800** | V380 secondary (measured on camera 1) | **OPEN** |
| 80 / 443 | HTTP(S) management candidates | refused |
| 5050 / 5051 | V380S community-reported | refused |
| 34567 | DVR default candidate | refused |

So this unit exposes **exactly the same two vendor ports as camera 1 did before its ONVIF toggle** —
and, unlike camera 1, there is no toggle to enable.

### Residual gap, stated explicitly

A **full-range listener inventory was NOT performed**, because the instruction scoped this step to
the documented ports. Therefore this report can say "no exposure on any documented interface" and
**cannot** say "no listener exists anywhere on the device". An undocumented listener would be an
unknown exposure. Running a full 1–65535 check against this one identified camera is a ~30-second
job and is available on request — it was deliberately not taken beyond the stated scope.

## 4. Exposure test before integration (step 4)

### 4.1 Access-control classification

```
bash scripts/v380_security_check.sh <redacted-lan-host-20> 554 8899
  RTSP/554     DISABLED   no RTSP listener (connection refused)
  ONVIF/8899   DISABLED   no ONVIF listener (connection refused)
```

Classified with the corrected semantics from `HG_V380_06`: **DISABLED is not protection**, and this
report does not claim it is. There is no interface to expose.

### 4.2 Behaviour of the one interface that is live — no credential sent

| Property | Camera 2 (`<redacted-lan-host-20>`) | Camera 1, same probe | Read |
|---|---|---|---|
| 8800 on bare connect | silent, waits for a client hello | silent | same |
| 8800 on HTTP `GET /` | 0 bytes, connection closed | 0 bytes, closed | **not HTTP** |
| 8800 on RTSP `OPTIONS` | 0 bytes, closed | 0 bytes, closed | **not RTSP** |
| 8800 idle | closed after **5.01 / 5.01 / 5.02 s** | ~5.0 s | same idle budget |
| 8800 on 16 junk bytes | closed after **0.20 / 0.21 / 0.21 s**, 0 bytes back | ~0.22 s, 0 bytes | same closed behaviour |
| simultaneous connections | 6 accepted | 6 accepted | same |
| 9800 on bare connect | **silent** (waits for a client hello) | closed immediately, 0 bytes | **differs** — a real firmware difference between the two units |

**What this proves and what it does not.** It proves that an unauthenticated client gets **nothing**
from this device: no banner, no error text, no media, no identity. It does **not** prove the vendor
protocol authenticates — a login handshake was never sent, because constructing one is a
proprietary-protocol action that is not authorized. So the honest statement is:

> No anonymous media or state exposure was found on any documented interface. The authentication
> behaviour of the proprietary protocol on `tcp/8800` is **unverified** — absence of a reply to a
> malformed packet is not the same as enforced authentication.

### 4.3 What was not done

No credential, no password attempt, no login packet, no decryption, no firmware change, no SD-card
configuration, no bypass. No media recorded, stored or published — nothing streamed, so nothing
could be.

## 5. Capability measurement (step 5): NOT POSSIBLE, and why

The instruction is conditional — *"if a secure supported local interface exists"*. It does not:

- **No RTSP** (554/8554 refused) → no stream description, no codec, resolution, FPS, or second
  stream to measure.
- **No ONVIF** (8899/3702 refused, no discovery responder) → no profiles, encoder configuration,
  PTZ model, imaging block, source list, or audio-source report.
- **No HTTP** (80/443 refused) → no management or snapshot surface.

Therefore resolution, frame rate, codec, audio presence, two-way audio and PTZ **remain
unmeasured** for camera 2, and reporting any of them would be fabrication. The only remaining path
to this camera's media is the **encrypted proprietary protocol on `tcp/8800`**, which the standing
rules prohibit without a separate explicit owner decision and security review (`HG_V380_06` §3.4).

## 6. Authentication limitations (step 6)

1. **This firmware has no native ONVIF, and now that is measured rather than inferred:** the
   Advanced-settings row is absent (owner's screenshot — the vendor documents the row as hidden when
   the firmware cannot support it), no ONVIF listener exists on 8899 or 3702, and the device does not
   answer WS-Discovery at all.
2. **There is no standard interface to attach authentication to.** Camera 1's problem was a standard
   interface that enforced nothing; camera 2's is that no standard interface exists. Setting a device
   password cannot fix a port that is not open.
3. **The vendor's device username/password is still worth setting** — it gates the vendor protocol and
   the app's own preview path — but it is **not** a media-interface control here, because there is no
   media interface.
4. **Do not read `8800`/`9800` being present as a usable integration point.** They are a
   login-gated proprietary protocol whose authentication is unverified, and the earlier house rule
   still applies: a camera whose interface cannot be shown to enforce authentication must be reached
   only through a component we control, on a segment whose client population we control.

**Consequence for the architecture:** camera 2 reinforces the `HG_NET_ARCH_01` design rather than
complicating it. A site connector is the *only* way this device can be integrated at all — it is the
component that would speak the local protocol, hold the credential the camera cannot hold, and be the
single authorized client. No adapter is written today, because there is no authorized interface to
write it against, and `HG_V380_03`'s plan already puts the protocol behind a `V380Transport` seam
precisely so that decision stays separable.

## 7. Camera 1 kept separate (step 7)

```
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899
  RTSP/554    DISABLED    ONVIF/8899  DISABLED
ports: 554 refused, 8899 refused, 8800 OPEN, 9800 OPEN
```

Camera 1 remains at `<redacted-lan-host-19>`, MAC `<redacted-mac-a>`, ONVIF **disabled**, unchanged and
untouched by this milestone. Its evidence (`HG_V380_01`/`_02`/`_04`) is not mixed with camera 2's
anywhere in this document.

## 8. What is still true about the network

Nothing here creates a dependency: camera 2 is currently on `<redacted-wifi-ssid>` at `<redacted-lan-host-20>` via DHCP,
and **that address is a DHCP lease, not an identity**. Per `HG_NET_ARCH_01`, the permanent design
keys on `{site_id, device_id}` + a credential, with the address as a refreshable `endpoint`
attribute. This same-subnet arrangement is development-only, as instructed.

## 9. Reproduce

```bash
ip route get <redacted-lan-host-20> && ping -c 3 <redacted-lan-host-20>
ip neighbor | grep '<redacted-lan-host-20>'                      # MAC -> OUI lookup
bash scripts/v380_security_check.sh <redacted-lan-host-20> 554 8899
bash scripts/v380_probe.sh <redacted-lan-host-20>                # port state + listener behaviour
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899   # camera 1: still DISABLED
```

## 10. Status

| Step | Status |
|---|---|
| 1 — reachability | **DONE** — reachable on-link, 0 % loss, no NAT |
| 2 — identity correlation before probing | **DONE** — `<redacted-mac-b>` (AltoBeam), distinct from camera 1, matches the app report and the port signature |
| 3 — documented interface inventory | **DONE** — only 8800/9800; no RTSP, ONVIF, HTTP. Full-range inventory deliberately NOT run (scope); disclosed as a residual gap |
| 4 — exposure test before integration | **DONE** — no anonymous media or state on any documented interface; vendor-protocol authentication left explicitly unverified |
| 5 — capability measurement | **NOT POSSIBLE** — no secure supported local interface exists; no capability figure is claimed for camera 2 |
| 6 — authentication limitations | **DONE** — no native ONVIF (measured), no standard interface to authenticate, proprietary path prohibited without a new decision |
| 7 — camera 1 separation | **DONE** — separate identity/evidence; ONVIF still disabled and re-verified |

**Verdict: camera 2 is identified, reachable and exposure-free on every documented interface — and
has no standard media interface, so it cannot be integrated through RTSP/ONVIF at all. The next
decision is the owner's: whether the proprietary protocol is to be considered (a new, explicitly
approved decision with a security review), or whether this camera is deployed as a sensor-only node
and media is obtained another way. Nothing was altered anywhere.**
