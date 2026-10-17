# HG-V380-02 — V380 EC39 Media Interfaces and Protocol Surface

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Target:** `<redacted-lan-host-19>` (MAC `<redacted-mac-a>`, OUI AltoBeam) — identified in
`docs/HG_V380_01_DISCOVERY.md`.

This document separates three things deliberately:

- **(A) what was MEASURED** on this camera, today;
- **(B) what is DOCUMENTED** for the V380 / Macro-video family, with sources;
- **(C) what was NOT done**, and why.

---

## 1. (A) Measured protocol surface — full port result

Full 1–65535 TCP sweep against the camera: **exactly two listeners, `tcp/8800` and
`tcp/9800`.** Every other port returns a TCP RST immediately.

| Port | Probe | Measured result |
|---|---|---|
| 8800 | TCP connect | **OPEN** — silent, waits for a client hello |
| 9800 | TCP connect | **OPEN, but closes immediately on a bare connect** (0 bytes, FIN). Any payload on it (HTTP GET, RTSP OPTIONS) is answered with a **connection reset (ECONNRESET)** |
| 80, 443, 8080 | TCP connect / HTTP `GET /` | refused (errno 111, 0.00 s) |
| 554, 8554 | TCP connect; RTSP `OPTIONS rtsp://…/ RTSP/1.0` | refused / connect fail |
| 8899 | TCP connect (documented ONVIF port) | refused |
| 3702 | WS-Discovery `Probe` → `NetworkVideoTransmitter` to 239.255.255.250:3702 | **no response** within 3 s |
| 34567 (Xiongmai/DVR default) | TCP connect | refused |
| 8800 / 9800 | UDP probes (zero-payload, `V380`, JSON) | **no reply** |

**No RTSP, ONVIF, HTTP or UPnP/mDNS interface is present.** The camera is not discoverable by
any standard discovery protocol; it only answers on its own binary protocol.

## 2. (A) Measured behaviour of the proprietary listener (no credentials sent)

These are real observations, useful because they constrain the adapter design:

| Property | Measurement |
|---|---|
| Server-first banner | **none** — the server sends nothing and waits for a client hello |
| Idle connection with no bytes sent | closed by the camera after **5.0 s** (3/3 trials: 5.0 / 5.0 / 5.0) |
| Unrecognized payload (16 arbitrary bytes) | closed after **≈0.22 s** (3/3 trials: 0.23 / 0.21 / 0.22), **0 bytes** returned — no error text, no protocol echo |
| HTTP `GET / ` on 8800 | connection closed, empty response |
| RTSP `OPTIONS` on 8800 | connection closed, empty response → **port 8800 is not an RTSP server** |
| Client half-close (`shutdown(SHUT_WR)`) | clean FIN back (0 bytes) |
| Simultaneous connections | 6 simultaneous sockets accepted; no visible connection-count limit at that level |
| tcp/9800 on a bare connect | closed immediately (0 bytes), and **resets** the connection when a payload arrives (ECONNRESET) — it is reachable but not a prompt-answering service |

Reproduce all of the above: `bash scripts/v380_probe.sh <redacted-lan-host-19>`.

Design consequences:

- A 5 s idle budget means a **keepalive** is mandatory on any long-lived control channel, and
  the reconnect logic must treat "closed with 0 bytes" as a normal, expected event rather than
  a transport fault.
- An unauthenticated or malformed packet costs ~0.2 s and yields **no** diagnostic. There is no
  way to distinguish "wrong password" from "wrong protocol version" by reading a response; the
  adapter must therefore surface its own structured error categories rather than relaying
  anything from the device.
- **Nothing in the probe sequence authenticated.** No credential, token or UID was sent, and
  none is recorded.

## 3. (B) Documented V380 media interfaces (sources cited)

### 3.1 RTSP — disabled by default, enabled from the app

- Candidate stream paths for this family (investigation candidates, **not** confirmed for the
  EC39, because there is no RTSP listener to test against):
  - main stream `/live/ch00_1` (H.264)
  - sub stream `/live/ch00_0` (H.264)
  These are the two paths named in the brief and they match the community camera database
  (cctv-database.com "V380 RTSP URL", verified 2026-08-14; ispyconnect.com V380 entry listing
  `380S/q15/Q8-C/V380pro → /live/ch00_1` and `360/380s/e12/v380vv → /live/ch00_0`).
- Default port when enabled: **554**.
- **Status on this camera: not enabled** (§1) — no 554 listener exists.

### 3.2 ONVIF — vendor-documented switch, port 8899

Vendor FAQ, retrieved 2026-10-17 from
`https://v380camera.com/en/article/how-to-turn-onoff-onvif-function-of-v380-camera`
(dating line on the page: 2026.05.15), verbatim:

> "After APP connects to the camera, click [Settings] icon in the Device list, select
> [Advanced settings]-turn on/off [Onvif setting] option
> Note: This function needs to be supported by the device firmware, and the switch will not be
> displayed on the APP if the device can not support"

The documented ONVIF port for this family is **8899**, used with **UDP** transport and the
camera's own app credentials (Home Assistant community thread "V380s Camera — Configuration":
"new Android v380 Pro app > Advanced settings > Onvif setting > On HA Integration : Onvif
Configuration: UDP, port: 8899"). **Status on this camera: the switch's effect is not present
— 8899 is refused and 3702 discovery is silent.**

### 3.3 The E27 bulb form factor specifically

Independent owners of the *same bulb form factor* report the same as-shipped state:
"Scanning it via **nmap**, I couldn't see any ports open either that could be **onvif or
rtsp** related" (shantanugoel.com, "Enabling ONVIF and RTSP in V380 Pro Bulb Camera from
Amazon", 2024-12-19), and at least one vendor/model listing for a V380 LED bulb camera states
outright "ONVIF: not supported" (v380.org product page). **The absence of these interfaces on
the EC39 is the expected default, not evidence of misconfiguration.**

### 3.4 The proprietary protocol on 8800 (reference only — NOT used)

Public reverse-engineering projects exist for this family and describe the transport as an
AES-128-ECB-encrypted binary stream over **TCP/8800** with a login handshake:

- `prsyahmi/v380` — "Extract h264 from V380 camera" (`-u`/`-p`/`-addr`/`-mac`/`-id` arguments)
- `felipemarques/camera-v380decoder` — C#/.NET port; re-exposes RTSP/ONVIF/snapshot from the
  decoded stream
- `FelixDaCraft/v308-rtsp-hack` — Macrovideo/Xiaovv V308 family: "decrypts the proprietary
  AES-128-ECB live stream on TCP/8800"
- `research.aurainfosec.io/pentest/v380-ip-camera/` — security research on the same family
  (note: this research documents a hard-coded key used to encrypt packets carrying the admin
  password — a fact that belongs in a security assessment, and is exactly why this path
  requires a separate, explicit decision)

This material is recorded **for reference only**. **It was not used, and no proprietary
protocol packet was constructed or sent in this milestone** — see §4.

## 4. (C) What was NOT done, and the decision gate

Not done, deliberately:

| Not done | Why |
|---|---|
| Did **not** enable ONVIF in the app | It is an owner action on the owner's phone, and it changes device state; the brief puts the enable decision on the owner |
| Did **not** use an SD-card `ceshi.ini` file | **Explicitly forbidden this milestone** ("Do not use unofficial SD-card configuration files, firmware modification or proprietary-protocol bypasses"). For the record, this is the route that made the failure case above work for another owner — the working content was `[CONST_PARAM] rtsp_enable=1 onvif_enable=1` (shantanugoel.com, 2024-12-19). It is documented here as *known and declined*, not as a sanctioned method |
| Did **not** construct a proprietary 8800 login packet or attempt any credential | Password guessing is forbidden; the protocol is proprietary and its bypass class is forbidden this milestone |
| Did **not** change any router setting (no DHCP reservation, no port forwarding) | Requires owner approval first; the camera is not exposed publicly and must not be |
| Did **not** measure resolution/FPS/codec/timestamps/dropped frames, audio codec, talkback, PTZ, illumination or alerts | **Impossible without a working authenticated stream.** Reporting any of these would be fabrication |

### 4.1 The decision gate

Every downstream measurement in the brief (video parameters, dual-lens layout, audio, talkback,
PTZ, night vision, alert surface — sections 4–8 of the milestone) is gated on **one owner
decision**: whether the camera is to be made reachable through a documented, authenticated,
local interface.

Two paths exist, and only one is in scope:

1. **Sanctioned and in scope — app-side ONVIF/RTSP enable.** Owner opens
   *Settings → Advanced settings → ONVIF setting* (if the switch is present at all) and enables
   it; RTSP then becomes reachable on 554 and ONVIF on 8899. **Precondition: after enabling,
   re-test for unauthenticated access and stop if any appears** (brief §3).
2. **Out of scope — the proprietary TCP/8800 path.** Even though it is the only interface
   currently live, using it means speaking a reverse-engineered encrypted vendor protocol, so
   it needs its own explicit owner decision (it is a "proprietary-protocol bypass" under the
   brief's wording).

If the ONVIF switch is **absent** in the app, the vendor's own note applies: the firmware does
not support it, and the milestone cannot proceed on path 1 at all. That is a legitimate result
to report — not a reason to reach for path 2 or the SD-card route without new approval.

### 4.2 What would be acceptable to hand a downstream reader

Once (and only once) an authenticated local interface exists, the measurements to take are the
brief's own list, and each must be reported as a measured value with its method:
resolution, FPS, codec, timestamp source and drift, session duration, dropped/reordered
frames, reconnect behaviour and recovery time; whether the fixed and PTZ lenses are separate
streams or one composite frame; whether microphone audio is multiplexed and in which codec;
whether talkback exists on a documented authenticated interface; PTZ command latency and
limits; illumination mode; and which alert events (motion / person / ONVIF) are actually
emitted and by which interface. Until then they are **NOT MEASURED** — see
`docs/HG_V380_03_HOMEGUARDIAN_ADAPTER_PLAN.md` §6.

## 5. Sources

| Fact | Source | Retrieved |
|---|---|---|
| ONVIF switch location + "needs firmware support, switch hidden otherwise" | v380camera.com/en/article/how-to-turn-onoff-onvif-function-of-v380-camera | 2026-10-17 |
| ONVIF port 8899 / UDP / app password on this family | community.home-assistant.io "V380s Camera — Configuration" | 2026-10-17 |
| `/live/ch00_1` (main) and `/live/ch00_0` (sub), port 554 | cctv-database.com/rtsp/v380 ; ispyconnect.com/camera/v380 | 2026-10-17 |
| RTSP/ONVIF disabled by default on V380 Pro; bulb form factor with no open ports | shantanugoel.com 2024-12-19 | 2026-10-17 |
| V380 LED bulb camera "ONVIF: not supported" (model listing) | v380.org/product/led-bulb-lamp-camera-v380 | 2026-10-17 |
| Port 8800 = proprietary AES stream (reference only) | prsyahmi/v380 ; felipemarques/camera-v380decoder ; FelixDaCraft/v308-rtsp-hack | 2026-10-17 |
| Family security research (hard-coded key, password packet) | research.aurainfosec.io/pentest/v380-ip-camera/ | 2026-10-17 |

**Status: protocol surface discovery COMPLETE (negative for standard interfaces). Media
parameter measurement BLOCKED — no authenticated interface exists, and enabling one is an
owner decision, not an agent action.**
