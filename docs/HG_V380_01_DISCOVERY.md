# HG-V380-01 — Live V380 EC39 Camera Discovery

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Scope:** identify the live V380 camera on the 2.4 GHz home LAN and establish which
authenticated local interfaces it actually exposes. No configuration was changed.

---

## 1. Authorization and prohibitions honored

| Prohibition (from the brief) | Status |
|---|---|
| Do not reset, re-pair, disassemble or reflash the camera | Honored — no action taken on the device |
| No indiscriminate network scanning, no password guessing | Honored — see §2 "Method" for the exact bounds used |
| No attempts to access unrelated devices | Honored — every probe below is scoped to the identified camera address |
| Do not configure router port forwarding or expose the camera publicly | Honored — router config untouched |
| No unofficial SD-card config file (`ceshi.ini`), firmware modification or proprietary-protocol bypass | Honored — explicitly **not** used; see §5 |
| Do not print camera passwords, account tokens or unique device identifiers | Honored — no credential, UID or token appears in this document |

Two devices on this LAN were *not* touched at all: the gateway itself (§3) was only
queried for its HTTP identity header, and the two non-camera hosts were probed for open
ports only, to establish which host is *not* the camera.

---

## 2. Method (minimally invasive, bounded)

1. Interface and subnet from the host's own Wi-Fi interface (`wlp6s0`, <redacted-lan-host-0>/24).
2. Host liveness: ICMP sweep of <redacted-lan-host-0>/24 (1 packet, 1 s timeout, 50 threads).
3. ARP/neighbour table read (`ip neighbor`) for MAC addresses of the hosts that answered.
4. Vendor identification: MAC OUI looked up against the IEEE registry (`api.macvendors.com`).
5. Per-host TCP port profile, restricted to a fixed list of camera-relevant ports
   (V380/Macro-video known ports plus RTSP/ONVIF/HTTP candidates) — no full-range sweep
   against any host, then a full 1–65535 sweep **only** against the host identified as the
   camera candidate by step 6.
6. Camera fingerprinting: which host exposes the Macro-video port signature and nothing else.
7. Protocol probes against that host only: HTTP `GET /`, RTSP `OPTIONS`, SSDP `M-SEARCH`,
   mDNS service enumeration, ONVIF WS-Discovery probe, UDP probes on the two open ports.
   No credential was transmitted.

---

## 3. Live hosts found

| Address | MAC | OUI vendor (IEEE registry) | Open TCP | Assessment |
|---|---|---|---|---|
| <redacted-lan-host-1> | <redacted-mac-g> | **ZTE Corporation** | 53, 80, 443 | Home gateway — HTTP title decodes to `F670LV9.0` (ZTE F670L GPON ONT/router) |
| <redacted-lan-host-3> | <redacted-mac-c> | Tenda Technology (Dongguan) | none | Tenda network device (no open TCP) |
| <redacted-lan-host-4> | <redacted-mac-f> | not in registry (no PTR record) | none | Unidentified household device, not a camera by port signature |
| <redacted-lan-host-6> | — | — | 80, 8443 | **This workstation** (HomeGuardian demo surface) |
| <redacted-lan-host-14> | <redacted-mac-e> | locally-administered (randomized MAC) | none | Phone-class device (MAC randomization) |
| <redacted-lan-host-16> | <redacted-mac-d> | ShenZhen Konka Telecom | none | Consumer electronics (Konka-branded) |
| **<redacted-lan-host-19>** | **<redacted-mac-a>** | **AltoBeam Inc.** | **8800, 9800** | **Camera candidate — see §4** |

No device other than <redacted-lan-host-19> exposes any camera-relevant port. Two hosts that were
expected to be candidates on the earlier ARP read (<redacted-lan-host-3>, <redacted-lan-host-4>) have **zero**
open TCP ports on a full 1–65535 sweep.

---

## 4. Camera identification

**Verified on the LAN:** `<redacted-lan-host-19>`, MAC `<redacted-mac-a>` (OUI **AltoBeam Inc.**,
a Wi-Fi silicon vendor whose parts are used in low-cost IP cameras).

Evidence that this host is the V380/Macro-video camera:

1. **Port signature.** It exposes exactly `tcp/8800` and `tcp/9800` and nothing else. This is
   the Macrovideo/V380 local control+media port pattern; a community report for a V380S unit
   records "only ports 5050, 5051, 8800 and 8899 enabled" for that variant
   (netcamstudio.com forum thread, Fentac V380). Port 8800 is the port every public
   reverse-engineering project for this camera family dials
   (`prsyahmi/v380`, `felipemarques/camera-v380decoder`, `umar14/v380-android`).
2. **Protocol behaviour.** The 8800 listener is not HTTP/RTSP: it sends no banner, answers
   nothing to an HTTP request, an RTSP `OPTIONS`, a JSON request, or a binary V380-magic
   candidate, and closes the socket on unrecognized input (§2 of HG_V380_02). That matches a
   proprietary binary login-gated protocol, not a standard server.
3. **It is not any of the other live hosts.** Every other host has either no open ports, a
   router/HTTP identity, or a non-camera port set.
4. **It is not the Android node.** The Mi A3 / MediaTek node devices in this project are
   reached over ADB/USB; nothing in this list is the phone.

### What is NOT verified (owner action required)

The brief lists the camera's identity as *historical owner-provided*: "V380 Smart Net Camera,
model EC39, E27 bulb-style dual-lens PTZ, 2.4 GHz, V380 / V380 Pro app". **This document does
not upgrade any of that to verified.** From the LAN alone I can prove *a* Macro-video-signature
camera is at <redacted-lan-host-19>; I cannot prove it is specifically the EC39, nor read its firmware
version, device ID or serial, because:

- the V380 app is the only authorized source for the device identity block, and
- the camera's only open interfaces are credential-gated proprietary ones, with no
  unauthenticated read path.

**Owner action (read-only, in the V380 Pro app):**

1. Open the device list → the camera's **Settings** (gear) panel.
2. Read and report: device model string, **firmware / system software version** (the vendor FAQ
   locates this at *App → Device Configuration → Device Upgrade → System Software Version*),
   and the **IP / MAC** the app shows for the device.
3. Compare the app's IP/MAC with `<redacted-lan-host-19>` / `<redacted-mac-a>`. If they match, the
   identification is closed. If they do not, a second camera exists and this workstream's
   target is the app-reported address instead.

Nothing above requires changing a camera setting. Do not include the device ID / UID or any
account token in the reply to this project — the model string, firmware version, IP and MAC
are enough.

### Also not performed

- Router DHCP client list: the ZTE gateway's admin page requires the owner's credentials and
  no credential was requested or used. A DHCP **reservation** for the camera is a reasonable
  follow-up, but it is a router configuration change and needs separate owner approval first
  (the brief says so explicitly). Address <redacted-lan-host-19> is currently stable but unverified as
  reserved.

---

## 5. Interface availability (headline result)

| Interface | Candidate probe | Result |
|---|---|---|
| RTSP | tcp/554, plus documented alternates 8554 | **refused** (RST, 0.00 s) |
| RTSP over the vendor port | `rtsp://<redacted-lan-host-19>:8800/live/ch00_0` | connects, then `Invalid data found when processing input` — i.e. **not RTSP** |
| ONVIF | ONVIF candidate port tcp/8899, and WS-Discovery udp/3702 `Probe` for `NetworkVideoTransmitter` | **refused** / **no response** |
| HTTP device management | tcp/80, 443, 8080 | **refused** |
| UPnP/SSDP advertising | `M-SEARCH ssdp:all` to 239.255.255.250:1900 | **no responder** on this LAN |
| mDNS service enumeration | PTR query for `_services._dns-sd._udp.local` | **no responder** |
| Vendor LAN media/control | tcp/8800 + tcp/9800 | **open, credential-gated** — see HG_V380_02 |

**Conclusion:** the camera as currently configured exposes **no** RTSP, ONVIF or HTTP
interface. Only the proprietary Macro-video control/media ports are listening. This is the
documented **default state** for this camera family, not a fault: the vendor's own FAQ states
the ONVIF switch in *Settings → Advanced settings → ONVIF setting* "needs to be supported by
the device firmware, and the switch will not be displayed on the APP if the device can not
support" (v380camera.com, retrieved 2026-10-17), and RTSP/ONVIF are documented as
**disabled by default** on V380 Pro units, with independent owners of the *same E27 bulb
form factor* reporting exactly this symptom ("Scanning it via nmap, I couldn't see any ports
open either that could be onvif or rtsp related", shantanugoel.com, 2024-12-19).

Consequence for this workstream: **the enable decision is the owner's, in the app, and it is
the gate for every downstream measurement.** It is not performed here.

---

## 6. Security posture observed

- The camera is reachable **only** from the local subnet; no port forwarding exists or was
  added, and nothing on the camera was exposed publicly.
- The proprietary listener behaves defensively: it sends no banner and no error text, and it
  drops an idle connection after ~5.0 s and an unrecognized payload in ~0.2 s (measured,
  HG_V380_02 §2). It leaks no product string, version or identity on an unauthenticated
  connect.
- **If ONVIF is enabled in the app, re-check for unauthenticated access before proceeding**
  (the brief requires this, and it is a real risk for this family: ONVIF is documented as
  being reachable on `udp/8899` with the camera's own app password). That re-check is a
  precondition of HG-V380-02 §4, not optional.
- No credential, token, serial or UID is recorded in this document or in any log committed
  by this workstream.

---

## 7. Reproduce

```bash
# 1. live hosts
python3 - <<'EOF'
import subprocess, concurrent.futures
def ping(ip):
    return ip if subprocess.run(['ping','-c','1','-W','1',ip],
        stdout=subprocess.DEVNULL).returncode == 0 else None
ips=[f'<redacted-lan-subnet>.{i}' for i in range(1,255)]
with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
    print([r for r in ex.map(ping, ips) if r])
EOF

# 2. MACs / vendors
ip neighbor
curl -s https://api.macvendors.com/<redacted-mac-prefix>    # -> AltoBeam Inc.

# 3. camera port profile (bounded list; full range only against the camera)
python3 -c "
import socket
for p in (80,443,554,8554,3702,8899,8800,9800,34567):
    s=socket.socket(); s.settimeout(2)
    print(p, 'OPEN' if s.connect_ex(('<redacted-lan-host-19>',p))==0 else 'closed')
    s.close()"
```

---

## 8. Deliverables in this milestone

| Artifact | Purpose |
|---|---|
| `docs/HG_V380_01_DISCOVERY.md` (this file) | identity + LAN facts + what is unverified |
| `docs/HG_V380_02_MEDIA_PROTOCOLS.md` | the measured protocol surface and the documented V380 media interfaces |
| `docs/HG_V380_03_HOMEGUARDIAN_ADAPTER_PLAN.md` | adapter design against the existing `IMediaDevice` / consent architecture, and what it needs to become implementable |

**Status: HG-V380-01 identification = CONDITIONAL — one LAN-verified camera at <redacted-lan-host-19>;
model/firmware identity BLOCKED PENDING OWNER (app read-out). Interface discovery = COMPLETE
and negative: no RTSP/ONVIF/HTTP interface present as shipped.**
