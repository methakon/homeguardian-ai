# HG-V380-09 — Vendor-Protocol Feasibility and Alexa Camera Readiness

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Scope:** task 2 (vendor-protocol feasibility) and task 4 (per-camera Alexa readiness).
**Companion:** the boundary contract is in `docs/HG_V380_09_ADAPTER_BOUNDARY_SPEC.md`.

---

## 0. Headline

- **Neither camera can be integrated today, for two different reasons, and neither reason is a bug in
  HomeGuardian.** Camera 1 has a standard interface that *enforces nothing*; camera 2 has *no standard
  interface at all*.
- **No manufacturer-published developer SDK or public camera API was found.** What the vendor does
  publish is a **partner programme** offering OEM/ODM, system integration, secondary development, API
  integration and private-cloud deployment — a commercial route, not a download. That is the
  legitimate escalation path for camera 2, and it is recorded as such rather than as a missing file.
- **Nothing here reactivates ONVIF, reverses anything, or touches a vendor protocol.** Camera 1's ONVIF
  remains disabled and must stay disabled.
- **Camera 2 must NOT be described as a working sensor-only node.** No independent sensor output has
  been demonstrated from it, and nothing in this milestone changes that.

---

## 1. Task 2 — what the vendor actually supports (all pages retrieved 2026-10-17)

| Source | HTTP | What it establishes |
|---|---|---|
| `v380camera.com/en/article/how-to-turn-onoff-onvif-function-of-v380-camera` | 200 | The **ONVIF switch** lives at *Settings → Advanced settings → ONVIF setting* and "needs to be supported by the device firmware, and the switch will not be displayed on the APP if the device can not support" |
| `v380camera.com/en/article/how-to-set-or-modify-the-camera-password` | 200 | A settable **device username/password** exists ([Settings] → [Password] → [Change Device Password]) — separate from the V380 cloud account login |
| `v380camera.com/en/article/device-username-or-password-error` | 200 | The device credential is genuinely required by the app's own path: "it is necessary to fill in the correct [Device username] and [Device password]" |
| `v380.net/docs/user-manual/network-connection` | 200 | The vendor's own **LAN discovery**: tap **"Lan Search"**, or type the **Device ID** ("printed on the bottom of the camera, in the form `MV********`") — plus an AP-hotspot fallback |
| `v380camera.com/en/brand/join` | 200 | **The partner/cooperation programme**: "supporting **OEM/ODM, system integration, and secondary development**"; "full stack solutions such as **APP customization, private cloud deployment, API integration**, and one-stop technical support"; joining is by application form |
| `macro-video.com/en/support/support-download` | 200 | Corporate support surface = app + Windows client + manuals. Not a developer portal |
| `macro-video.com/en-us/1/1/service.html` | **404** | The `/service` path is not a developer page |
| Alexa+ add-ons home — `developer.amazon.com/docs/alexaplus/add-ons/home.html` | 200 | "Category SDK and MCP Toolkit are available to **select partners only**" (the gate recorded in friction-log Incident 75) |

### 1.1 The three legitimate paths, ranked

| # | Path | Applies to | Status |
|---|---|---|---|
| 1 | **App-documented standard interface** — the ONVIF switch + a device username/password | firmware-dependent | Camera 1 only, and it fails the security test (below). Camera 2's firmware does not offer the switch |
| 2 | **Vendor partner programme** (OEM/ODM · system integration · secondary development · API integration · private cloud) | both, commercially | **Not pursued** — it is a commercial agreement with the vendor, outside this milestone's authority. This is the correct escalation for camera 2 |
| 3 | **NVR-style integration the vendor documents** (connect a V380 camera to an NVR) | firmware-dependent | Same dependency as path 1: it needs the standard interface to exist and be enabled |

### 1.2 What is explicitly NOT a path here

| Excluded | Why |
|---|---|
| Reverse-engineered SDKs extracted from the vendor app (a `com.macrovideo.sdk` artefact circulates from an APK disassembly) | derived from reverse engineering, not authorized |
| Any proprietary login handshake on the vendor ports | task 2 forbids it; the brief also requires escalating anything needing the proprietary protocol for a separate owner decision |
| Stream decryption | forbidden — and the public projects that do it exist precisely because these units encrypt |
| Reactivating ONVIF, or the SD-card configuration route | Camera 1's ONVIF exposed anonymous video; the SD-card route was already declined in `HG_V380_06` §4 |
| Any third-party "default admin/admin" credential | unverified claim about a broad OEM family; attempting it is credential guessing |

**The one thing that would change the picture** is path 2: a vendor agreement that supplies either a
supported SDK/API or a camera whose firmware offers an *authenticated* standard interface. That needs
the owner's commercial decision, not an engineering workaround.

---

## 2. Camera-by-camera feasibility matrix

Legend: **VERIFIED** = demonstrated by us on the device · **ABSENT** = demonstrated not to exist ·
**UNVERIFIED** = not demonstrated either way.

| Dimension | Camera 1 — `cam.v380.ec39.01` | Camera 2 — `cam.v380.ec39.02` |
|---|---|---|
| Logical id (assigned) | `site.home.dev/cam.v380.ec39.01` | `site.home.dev/cam.v380.ec39.02` |
| Dev address | `<redacted-lan-host-19>` (an ATTRIBUTE, not identity) | `<redacted-lan-host-20>` (ditto; previously `<redacted-nat-host-164>`) |
| MAC / OUI | `<redacted-mac-a>` · AltoBeam | `<redacted-mac-b>` · AltoBeam |
| Firmware family | `<redacted-cam1-firmware>` / `<redacted-cam1-software-family>` | `<redacted-cam2-firmware>` / `<redacted-cam2-software-family>` |
| **Discovery readiness** | Reachable on link; ONVIF WS-Discovery answers **only when the toggle is on** — now off, so nothing advertises it | Reachable on link; **no discovery responder at all** (no WS-Discovery, no SSDP, no mDNS). The vendor's own app finds it by "Lan Search"/Device ID, which is *not* a HomeGuardian-usable discovery method |
| **Authentication status** | **FAILED, and closed by disabling.** With ONVIF on, **no** credential was demanded for anything | **Unverified.** The vendor ports return nothing without a login handshake, which we did not construct. No standard interface exists to authenticate to |
| **Video capability evidence** | **VERIFIED — and it is the problem.** Anonymous `DESCRIBE` returned the SDP; `PLAY` delivered **360 RTP packets (469,943 B)** on `/live/ch00_1` and **922 packets (1,285,254 B)** on `/live/ch00_0` in a 6 s window. Codec per SDP: **H.265/90000** | **Nothing.** No RTSP, no ONVIF, no HTTP; no frame has ever been obtained from this unit |
| **Audio / talkback evidence** | Audio: `GetAudioSources` reported a 2-channel source, but **both SDPs carried a single `m=video`** — so microphone audio was **not** in the stream. Talkback: `GetAudioOutputs` → **Action Not Implemented**, i.e. no ONVIF audio output | **Nothing** — no standard interface to ask |
| **Media transport options** | RTSP over TCP (interleaved) on 554 — **but unauthenticated, so unusable**. No other option exposed | Only the vendor's own ports (8800/9800), authentication unverified → **no usable transport today** |
| **Security blockers** | 1) no enforced authentication; 2) unknown-path `DESCRIBE` returned the default SDP (no path validation); 3) anonymous disclosure of MAC/IP, the `admin` account name and all profiles; 4) clock **9,001.5 s behind true UTC** while claiming NTP | 1) no standard interface; 2) vendor-protocol authentication unverified; 3) the proprietary protocol is out of scope without a separate owner decision; 4) timestamp trust is unestablished (no clock source read) |
| **Current state** | ONVIF **DISABLED** (re-verified: 554/8899/3702 refused; 8800/9800 open) | Only 8800/9800 open |
| **Adapter class today** | `refused_unsafe_interface` — the `unsafe_interface_device_script()` shape | `capability_absent` — the `no_video_device_script()` shape |

**Both rows end in "no usable media path", and the two ends are reached from opposite directions.
That is why the boundary in the companion document refuses by name instead of returning a boolean.**

---

## 3. Task 4 — Alexa camera readiness, per camera

Alexa's documented camera surfaces (retrieved 2026-10-17, both HTTP 200):

- **`Alexa.CameraStreamController`** — the RTSP path. Its prerequisites include "**RTSP + RTP
  streaming protocol**" and "**Interleaved TCP on port 443** (for both RTP and RTSP)".
- **`Alexa.RTCSessionController`** — for devices "capable of real-time communication with the WebRTC
  protocol"; the CameraStreamController page says to implement this one *instead* for RTC-capable
  cameras. Amazon's Smart Home security overview recommends WebRTC where possible, and the RTC page
  links the two-way communication APIs — i.e. talkback rides this interface.

| Readiness item | Camera 1 | Camera 2 |
|---|---|---|
| Discovery readiness | **Partial.** The device is identifiable and stable, but nothing it exposes is discoverable while ONVIF is off — which is the required state | **None.** No responder of any kind |
| Authentication status | **Failed.** The interface accepted unauthenticated clients | **Unverified**, with no interface to authenticate against |
| Video capability evidence | Verified for the *wrong* reason: video flowed **to an unauthenticated client**. For an authorized path, nothing is verified | **None** |
| Audio / talkback evidence | Audio not in the stream; **no** ONVIF audio output, so no talkback | **None** |
| Media transport options for Alexa | Neither Alexa path can be met today: `CameraStreamController` needs RTSP **with interleaved TCP on 443**, and this unit's RTSP is plain 554, unauthenticated, and currently disabled; `RTCSessionController` needs a WebRTC peer, which needs an authenticated source to feed it | No transport exists, so neither path can be met |
| Security blockers | Unauthenticated media + anonymous identity/account disclosure; requires a compensating control and an authenticated front end before *any* viewer, Alexa included, is attached | No interface; a viewer would have nothing to render, and a viewer must never be the thing that authenticates a camera |
| Work remaining for actual Alexa compatibility | (1) an *authenticated* camera interface (vendor path 1 or 2 — **not** the toggle as-is); (2) an adapter behind the boundary in the companion spec, proven by an authenticated session; (3) an authenticated WebRTC/RTC session controller fed from a compliant source; (4) two-way audio only if a real audio output is demonstrated; (5) account linking + certification, which the Alexa+ tooling gate currently blocks for non-partners (friction-log Incident 75) | (1) a supported interface at all — vendor partner programme, or a different camera; (2)–(5) as above, all of it strictly after (1) |

**Nothing above is enabled by this milestone.** The MCP tools remain read-only status/alert surfaces
and media never travels over MCP or JSON; the existing WebRTC signalling path is the intended video
route, and it is not wired to any camera.

---

## 4. What is still missing, stated without rounding

1. **No real adapter exists.** The boundary is implemented and tested against deterministic mocks; an
   `AdapterClass::rtsp_onvif` adapter has not been written, because neither camera offers an
   authenticated standard interface to write it against.
2. **No authenticated camera session has ever been established** with either unit. The phrase
   "camera adapter works" is therefore not usable anywhere in this project's documentation.
3. **Camera 2 has no demonstrated sensor output of any kind** — not video, not audio, not motion
   events. Describing it as a working sensor-only node would be a false claim; it is a *reachable
   device on the LAN with an unverified vendor interface*.
4. **Camera 1's clock is untrustworthy** (2 h 30 m behind true UTC while claiming NTP), so any future
   observation from it must be stamped with the receiving host's clock.
5. **The Alexa+ tooling gate stands** (partner-only), which bounds what "Alexa compatibility" can mean
   for this submission regardless of camera readiness.

## 5. Escalations for the owner (each is a decision, not a task)

| # | Decision needed | Consequence if granted |
|---|---|---|
| 1 | Pursue the vendor partner programme (OEM/ODM · API integration · secondary development) for camera 2 | The only legitimate route to camera 2 media; commercial terms with Macro-Video/Guangzhou Hongshi |
| 2 | Any use of the proprietary protocol on 8800/9800 | Requires a separate explicit approval *and* a security review; not started, not prototyped |
| 3 | Re-enabling camera 1's ONVIF for any reason | Would restore unauthenticated live video and anonymous disclosures; currently **no** |
| 4 | Camera replacement with a unit offering an *authenticated* standard interface | Removes the compensating-control requirement entirely; simplest path to a working integration |

## 6. Reproduce

```bash
# boundary + mock prototype (no camera, no socket, no media)
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests "[v380]"

# the two real cameras' current state (read-only, no credentials)
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899     # camera 1: DISABLED / DISABLED
bash scripts/v380_probe.sh <redacted-lan-host-20>                       # camera 2: 8800/9800 only
```

## 7. Status

| Task | Status |
|---|---|
| 1 — adapter boundary contract | **DONE** — `docs/HG_V380_09_ADAPTER_BOUNDARY_SPEC.md`, implemented in `src/camera/`, one authorization decision point, stable logical ids, address-as-identity refused in code |
| 2 — vendor-protocol feasibility | **DONE** — no published SDK/API; a documented partner programme is the legitimate route; every prohibited option named and excluded; no handshake, no decryption, no ONVIF reactivation |
| 3 — non-invasive prototype | **DONE** — deterministic mock adapters + connector + a `[camera]` test suite exercising registry, consent, rejection, reconnection and revocation against the REAL `DeviceRegistry` and the REAL pipeline; the mock's limits are asserted by a test, not just documented |
| 4 — Alexa camera readiness | **DONE** — per-camera matrix above, with the Alexa interface requirements cited and the tooling gate recorded |
| Escalations | **OPEN** — §5; the proprietary protocol is **not** touched pending a separate owner decision |
