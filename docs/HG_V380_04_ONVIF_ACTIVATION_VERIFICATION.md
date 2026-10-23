# HG-V380-04 — Post-ONVIF Activation Verification and Security Verdict

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Previous commit:** `8680ddb243ebe4a240460a6b42062ab61f6cfb38`
**Target:** `<redacted-lan-host-19>` (MAC `<redacted-mac-a>`)
**Milestone:** HG-V380-02 — post-ONVIF-activation verification

---

## 0. VERDICT FIRST

> **The camera exposes LIVE VIDEO and its full device/account configuration to any
> client on the LAN with no credentials at all.**
>
> Stage 2's mandatory authentication assessment **FAILS**. Per the milestone's own
> instruction ("If private media or privileged controls are anonymously accessible,
> STOP"), **Stage 3 and Stage 4 were not performed** — no authenticated media
> measurement was taken and no HomeGuardian adapter was written.
>
> **Requested owner action: turn the ONVIF setting back OFF in V380 Pro**, then
> reconfigure the camera so that ONVIF and RTSP require authentication before the
> interface is re-enabled. The workstream can resume the moment a
> `scripts/v380_security_check.sh` run returns PROTECTED.

Reproduce the verdict: `bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899` → exit 1.

---

## 1. Owner confirmation and firmware (owner-supplied)

The project manager enabled the ONVIF setting through V380 Pro and supplied two
screenshots (Drive folder `screenshorts`: `1000129426.png` advanced settings / ONVIF
enabled, `1000129427.png` firmware information). Reported values:

| Field | Value | Source |
|---|---|---|
| Software | `<redacted-cam1-software>` | owner screenshot (NOT independently verified) |
| Firmware | `<redacted-cam1-firmware>` | owner screenshot (NOT independently verified) |

No firmware update, factory reset or device modification was performed — by the owner
or by this workstream. **The Drive folder was not accessed during verification; these two lines are recorded
as owner-supplied evidence, not as independently observed results.**

The evidence that ONVIF was *actually* enabled is the interface change in §2, which is
independent of the screenshots.

## 2. Stage 1 — focused rediscovery (compare with HG-V380-01)

Same bounded probe list, same target. **No full-port LAN scan was repeated.**

| Port | HG-V380-01 (before) | Now | Change |
|---|---|---|---|
| 554 (RTSP) | refused | **OPEN** — answers `RTSP/1.0 200 OK` | **new** |
| 8899 (ONVIF) | refused | **OPEN** — answers HTTP/SOAP | **new** |
| 3702 (WS-Discovery) | no responder | **responder present** | **new** |
| 8800 | open | open (unchanged) | — |
| 9800 | open | open (unchanged) | — |
| 80, 443, 8080, 8554, 5000, 5050, 5051, 8000, 34567 | refused | refused | — |

`RTSP OPTIONS rtsp://<redacted-lan-host-19>/` → `RTSP/1.0 200 OK`, `Public: OPTIONS, DESCRIBE,
SETUP, TEARDOWN, PLAY, PAUSE`.

WS-Discovery (`Probe` for `NetworkVideoTransmitter`, udp/3702, broadcast + unicast)
→ 3 responses from `<redacted-lan-host-19>:3702`, `XAddrs=http://<redacted-lan-host-19>:8899/onvif/device_service`.

So the toggle did exactly what it advertises: it opened a standard RTSP + ONVIF surface
that did not exist before.

### Identity CLOSED

`GetNetworkInterfaces` (retrieved at 19:32 IST) reports:

```
NetworkInterfaces token="eth0"  Enabled=true
  Info:   Name=wlan0   HwAddress=<redacted-mac-a>   MTU=1500
  IPv4:   Enabled=true  Manual  <redacted-lan-host-19>/24   DHCP=false
```

`HwAddress` and `Address` match the LAN-discovered device exactly, so the device at
`<redacted-lan-host-19>` **is** the camera the owner enabled. It is manually configured (not DHCP),
so it will not move.

`GetDeviceInformation` (unauthenticated, see §3) reports:

| Field | Value |
|---|---|
| Manufacturer | `macro-video-soft` |
| Model | `IPCamera` |
| FirmwareVersion | `2.4` (this is the ONVIF-reported number; it is NOT the same string as the owner's app-side `<redacted-cam1-firmware>`) |
| SerialNumber | `123456789` — a placeholder/default, not a unique serial |
| HardwareId | `1.0` |

The model string `IPCamera` is generic and does **not** confirm "EC39". The owner's
app-side firmware string remains the only model-level evidence; treat EC39 as
*owner-reported*, as HG-V380-01 already recorded.

## 3. Stage 2 — authentication assessment: **FAILED (EXPOSED)**

### 3.1 RTSP: no authentication, at all

`DESCRIBE` was sent **without any credential, without any auth header**. Result on
**every** path tried:

| URL | Response | Auth challenge | SDP |
|---|---|---|---|
| `rtsp://<redacted-lan-host-19>:554/live/ch00_1` | `RTSP/1.0 200 OK` | **none** | 483 B |
| `rtsp://<redacted-lan-host-19>:554/live/ch00_0` | `RTSP/1.0 200 OK` | **none** | 484 B |
| `rtsp://<redacted-lan-host-19>:554/` | `RTSP/1.0 200 OK` | **none** | 484 B |
| `rtsp://<redacted-lan-host-19>:554/1` | `RTSP/1.0 200 OK` | **none** | 484 B |
| `rtsp://<redacted-lan-host-19>:554/` `/h264_stream` | `RTSP/1.0 200 OK` | **none** | 484 B |

There is no `WWW-Authenticate` header in any response. The server does not care which URL
is requested — an unknown path returns the same default stream description, which is
itself a defect (paths are not validated), but the material point is the missing
authentication.

### 3.2 RTSP: unauthenticated clients receive live video

`DESCRIBE` → `SETUP` (TCP interleaved `RTP/AVP/TCP;unicast;interleaved=0-1`) → `PLAY`,
all three answered `RTSP/1.0 200 OK` **with no credentials**. Interleaved RTP frames then
flowed:

| Path | RTP packets received | Bytes (6 s window) | Payload type |
|---|---|---|---|
| `/live/ch00_1` | **360** | 469,943 | 98 (H265/90000 per SDP) |
| `/live/ch00_0` | **922** | 1,285,254 | 98 (H265/90000 per SDP) |

A second, independent run in `v380_security_check.sh` gave 481 and 402 packets
respectively. **Any device on this Wi-Fi network can watch this camera live with no
credential.** The camera's own RTP timestamp delta matched wall clock within the window
(ch00_1: 4.13 s of media in a 6 s window; ch00_0: 5.47 s), i.e. this is a real, continuous
stream and not a stub.

**No footage was recorded, saved, decoded or published.** The probe counts interleaved
packets on the socket and discards the payload; no media file was created at any point,
per the milestone's prohibition.

### 3.3 ONVIF: the whole device state is readable without credentials

Every call below was made with **no credentials and no auth header**:

| ONVIF call | Result | What was disclosed |
|---|---|---|
| `GetDeviceInformation` | `200 OK` | manufacturer, model, firmware, serial, hardware id |
| `GetNetworkInterfaces` | `200 OK` | interface name, **MAC**, **IP/prefix**, DHCP state |
| `GetUsers` | `200 OK` | **`admin` / `Administrator`** — the account name and level |
| `GetProfiles` | `200 OK` | 2 profiles, encoder config, PTZ node/config tokens, multicast address |
| `GetStreamUri(PROFILE_000)` | `200 OK` | `rtsp://<redacted-lan-host-19>/live/ch00_0` |
| `GetServices` | `200 OK` | media, media2, PTZ, events, imaging endpoints |
| `GetVideoSources` | `200 OK` | source token, resolution, frame rate, full imaging block (exposure, focus, white balance, IR-cut) |
| `GetAudioSources` | `200 OK` | 1 audio source, 2 channels |
| `GetNodes`, `GetStatus`, `GetConfigurationOptions`, `GetPresets` (PTZ) | `200 OK` | PTZ model, limits, speed defaults, presets |

`GetAudioOutputs` → `400 Action Not Implemented` (see §5).

**On privileged control:** the PTZ service answers read-only calls anonymously, and the
profile/PTZ configuration is fully readable. **No PTZ motion command was issued** —
not even a zero-velocity one — because the milestone's STOP condition had already
triggered on anonymous media (§3.2). Whether an unauthenticated `ContinuousMove` would be
*accepted* is therefore **UNTESTED and must not be reported either way**. The exposure
already proven is sufficient grounds to disable the interface, and moving the owner's
camera to complete a test would be an unrequested physical action on their device.

### 3.4 The exposure is not "ordinary unauthenticated discovery"

The milestone asks for that distinction explicitly, and it matters:

- *Ordinary unauthenticated discovery* = WS-Discovery announcing that a device exists,
  or ONVIF `GetSystemDateAndTime` (required by the spec before authentication, needed for
  digest nonces). **Those are expected and are not the finding.**
- *The finding* = an unauthenticated client received **a live H.265 media stream**, the
  **device identity and network configuration**, and the **operator account name**.
  That is unauthorized access to private media and to camera state, not discovery.

### 3.5 Scope of the exposure

| Question | Answer |
|---|---|
| Reachable from the LAN? | **Yes** — proven from this workstation |
| Reachable from the internet? | **NOT TESTED.** No external vantage point was used and the router was not inspected (its admin page needs the owner's credentials). **The owner should confirm no port forwarding exists for 554/8899/8800/9800.** This must not be assumed safe |
| Is the camera's own cloud/app path affected? | Not by anything this workstream did — no camera, router or account setting was changed |

## 4. What was deliberately NOT done

| Not done | Reason |
|---|---|
| Stage 3 (authenticated media measurement: resolution/FPS/codec/stability/secondary stream/audio/profile capabilities) | Gated on satisfactory access control, which failed. **STOP invoked.** |
| Stage 4 (HomeGuardian adapter implementation) | Same gate. Also: no credentials exist, so an adapter could not be written as a *verifiable* implementation |
| Any credential attempt, password guess, or auth header | Forbidden; not needed to prove the exposure |
| Any PTZ move, preset recall, reboot or configuration write | Forbidden by the STOP condition; no control command was sent |
| Recording or publishing any footage | Forbidden; verified — no media file was written |
| Router configuration, Oracle VPS, security branch, mail infrastructure, Cloudflare, other devices | Out of scope; untouched |

## 5. Incidental findings (worth fixing regardless of the verdict)

1. **Camera clock is 9,001.5 s behind true UTC** (2 h 30 m 1.5 s). `GetSystemDateAndTime`
   returned `2026-10-17T11:34:27Z` while the host — cross-checked against Google's HTTP
   `Date:` header (`Sat, 17 Oct 2026 14:04:46 GMT`) and `timedatectl` (`NTP service:
   active, System clock synchronized: yes`) — was at `14:04:28Z`. DateTimeType is `NTP`
   and `TZ` is the invalid string `PST0PDT`. **Any timestamp taken from this camera must
   be treated as untrusted until this is fixed**; it will silently corrupt any HomeGuardian
   correlation window.
2. **ONVIF encoder config and the RTSP SDP disagree on codec.** `GetProfiles` declares
   `H264`, `Main` profile, 2304×2592, `FrameRateLimit` 15, `EncodingInterval` 50,
   `BitrateLimit` 2048; the SDP declares **`H265/90000`** (payload 98) on both paths. One
   of the two is wrong. Since Stage 3 is gated off, the *actual* encoded resolution and
   frame rate are **NOT MEASURED** — do not take 2304×2592 or 15 fps as measured values.
3. **An audio source exists in the ONVIF model but no audio is offered on the streams.**
   `GetAudioSources` reports 1 source, 2 channels; the SDP for both tested paths contains
   a single `m=video` section and no `m=audio`. So microphone audio is **not** carried on
   `/live/ch00_0` or `/live/ch00_1` as configured.
4. **No audio output → no ONVIF talkback.** `GetAudioOutputs` returns
   `Action Not Implemented`, so speaker/talkback is not exposed over ONVIF. Per the
   milestone, talkback was not assumed merely because ONVIF is enabled — and here it is
   actively absent at the ONVIF layer.
5. **PTZ is present in the ONVIF model** (node `PTZNODE_000`, config `PTZCFG_000`, pan/tilt
   `[-1.0, 1.0]`, zoom `[0.0, 1.0]`, default timeout `PT5S`, EFlip/Reverse OFF) **but no PTZ
   command was sent**, so PTZ *operation* is NOT verified.
6. **`SerialNumber` is `123456789`** — a default placeholder, not a unique identifier.
   Anything keying on the serial would collide across units.
7. **Unknown RTSP paths return the default stream.** `DESCRIBE` on `/`, `/1` and
   `/h264_stream` all returned `200 OK` with a ~484 B SDP. Paths are not validated.

## 6. Existing V380 Pro app functionality

| Item | Status |
|---|---|
| Did this workstream change any camera/account/router setting? | **No.** No write of any kind was issued — every ONVIF call was a read |
| Is the camera still reachable on its own vendor ports? | **Yes** — 8800/9800 open and behaving as measured in HG-V380-02, unchanged |
| Is the app's operation impaired? | **NOT VERIFIED** — I cannot observe the phone. There is no mechanism by which a read-only probe would impair it, and none of the camera's own ports changed behaviour, but this is an owner observation, not a measurement |

## 7. Required owner action (blocking)

1. **Disable the ONVIF setting again** in V380 Pro (Settings → Advanced settings → ONVIF
   setting). This closes 554 and 8899 and removes the exposure; the app and the camera's
   own cloud path are unaffected by that switch.
2. **Reconfigure before re-enabling, so that authentication actually applies.** The camera
   must require credentials for RTSP and ONVIF — typically an ONVIF user and a stream
   user/password set in the app — rather than relying on the toggle alone. Note that on
   this firmware the toggle alone produced an *entirely* unauthenticated surface.
3. **Keep the camera off any port-forwarded path.** Please confirm no forwarding rule
   exists for 554 / 8899 / 8800 / 9800 (router administration requires owner credentials and was not
   accessed). External reachability was not tested.
4. **Do not treat the supplied firmware screenshots as sufficient for the security
   decision** — the numbers in §1 are yours, not independently verified here.

**Resume criterion:** `bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899`
returns **PROTECTED (exit 0)**. Until then, Stage 3 and Stage 4 stay blocked.

## 8. Evidence paths

| Artifact | Path |
|---|---|
| Security verdict, re-runnable, can FAIL | `scripts/v380_security_check.sh` (exit 1 here) |
| Bounded discovery probe | `scripts/v380_probe.sh` (expected-results block updated for the enabled state) |
| Raw ONVIF responses (text, no media) | `~/.hermes/cache/scratch/v380/{devinfo,netif,users,services,clock,profiles}.xml` (scratch, not committed) |
| RTSP sessions | `~/.hermes/cache/scratch/v380/stage2b.py` output — packet counts only |
| Prior milestone | `docs/HG_V380_01_DISCOVERY.md`, `HG_V380_02_MEDIA_PROTOCOLS.md`, `HG_V380_03_HOMEGUARDIAN_ADAPTER_PLAN.md` |

No media, credential, token or password is stored in the repository or in any scratch
artefact.

## 9. Milestone status

| Stage | Status |
|---|---|
| 1 — focused rediscovery | **COMPLETE** — 554 and 8899 opened; WS-Discovery now answers; identity CLOSED (MAC/IP match) |
| 2 — authentication assessment | **COMPLETE — FAILED / EXPOSED.** Anonymous live video on both paths, anonymous device+account+network disclosure |
| 3 — authorized media verification | **NOT PERFORMED — gated off by the STOP condition** |
| 4 — HomeGuardian adapter | **NOT PERFORMED — gated off.** The plan in `HG_V380_03` stands; no code written. The pending independent physical-device authentication release gate was not touched or bypassed |

**Verdict: NO-GO for integration until the camera is reconfigured with authentication and
`v380_security_check.sh` returns PROTECTED.**
