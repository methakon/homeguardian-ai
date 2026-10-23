# HG-V380-05 — Second CCTV Camera Behind a Wi-Fi Extender: Topology, Blockers and Path

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Previous commit:** `1ccc028d3ccdceac5fc700f181956dd899c6d3db`
**Milestone:** HG-V380-03 — second camera via Airtel router → Wi-Fi extender → V380 Pro camera

---

## 0. ANSWER FIRST

**The second camera is not reachable from this network, and it cannot be until one
network change is made.** The extender is running in **router/NAT mode** — it holds one
address on the Airtel LAN (`<redacted-lan-host-3>`) and serves its **own private subnet
`<redacted-nat-host-0>/24`** with its own DHCP (SSID **`kusankurdhar`**, BSSID
`<redacted-mac-c>`). The camera's reported `<redacted-nat-host-164>` therefore lives behind that
NAT. Traffic sent to it from either HomeGuardian node leaves through the **Airtel WAN to
the public internet** (`tracepath`: `<redacted-lan-host-1>` → `223.177.255.255` → `182.78.196.49`),
because no route to `<redacted-nat-host-0>/24` exists on the Airtel router.

**Stage 2 (camera interfaces) is therefore BLOCKED — not failed.** No interface on the
second camera can be tested until a HomeGuardian node is on `<redacted-nat-host-0>/24` or the
extender stops NAT'ing.

**No configuration on any router, extender or camera was changed.** Two owner actions are
requested at the end; one of them is a single Ethernet cable and unblocks Stage 2 with no
reconfiguration at all.

---

## 1. Scope and prohibitions honored

| Prohibition (this milestone) | Status |
|---|---|
| No broad host sweeps or probes against unrelated household devices | Honored — every probe was targeted at `<redacted-nat-host-164>` (plus `<redacted-nat-host-1>` / `.254` as the address's own gateway candidates) and at the three already-known LAN hosts |
| Do not reconfigure the extender or router | Honored — nothing was written to either |
| No firmware flash, reset, unofficial SD-card config or proprietary auth bypass | Honored — nothing of the sort was attempted |
| No public port-forwarding or exposing video ports to the internet | Honored — and explicitly rejected as an option in §6 |
| Preserve the first camera's configuration; do not mix identities/evidence | Honored — first camera evidence is in its own section (§7) and its configuration was untouched |
| Do not alter Oracle, Cloudflare, either VPS, mail infrastructure, Android devices or the security-audit branch | Honored — the Android nodes were **read-only** queried over adb (`ip`, route table, Wi-Fi scan list); nothing was written to them |

## 2. Method (targeted only)

1. `ip -brief addr`, `ip route`, `ip route get <redacted-nat-host-164>` on the workstation.
2. `ping` and per-port TCP to `<redacted-nat-host-164>`, `<redacted-nat-host-1>`, `<redacted-nat-host-254>` — 2 s
   timeouts, camera-relevant ports only (80/443/554/8554/3702/8000/8080/8899/34567/8800/9800).
3. `tracepath -n -m 6 <redacted-nat-host-164>` — shows where the path actually goes.
4. `ip neighbor` — is anything on `<redacted-nat-host-0>/24` even on our link? (No.)
5. Wi-Fi environment: `nmcli dev wifi rescan` + `nmcli -t -f BSSID,SSID,CHAN,SIGNAL dev wifi list`
   on the workstation, and `cmd wifi list-scan-results` / `cmd wifi status` on the Mi A3
   over adb — needed to see whether the extender advertises a network and which BSSID owns which SSID.
6. Bounded port check of the two unexplained LAN hosts (`<redacted-lan-host-3>`, `<redacted-lan-host-4>`).
7. Vendor identification of the extender by MAC OUI.

Re-runnable: `bash scripts/hg_second_camera_check.sh <redacted-nat-host-164>`

## 3. Topology — what was established

### 3.1 The workstation's actual position

```
wlp6s0   UP   <redacted-lan-host-6>/24   default via <redacted-lan-host-1>
enp5s0   DOWN (NO-CARRIER)     MAC <redacted-mac-h>   <- an idle Ethernet port
directly connected: <redacted-lan-host-0>/24, 172.17.0.0/16 (docker0), 172.18.0.0/16 (br-...)
nothing on <redacted-nat-host-0>/24
```

### 3.2 The route to the second camera does not stay in the house

```
ip route get <redacted-nat-host-164>
   ->  <redacted-nat-host-164> via <redacted-lan-host-1> dev wlp6s0 src <redacted-lan-host-6>
tracepath -n <redacted-nat-host-164>
   1: <redacted-lan-host-1>          (the Airtel router)
   2: 223.177.255.255      (Airtel upstream)
   3: 182.78.196.49        (public internet)
   4..6: no reply
```

The Airtel router has **no LAN route** for `<redacted-nat-host-0>/24`, so it treats the address as
an internet destination and forwards it upstream. That is the signature of the target
subnet living behind a second, NAT'ing router.

### 3.3 Every probe to the camera times out rather than being refused

| Target | ICMP | TCP (all probed ports) |
|---|---|---|
| `<redacted-nat-host-164>` | 0/2 replies (100 % loss) | **no answer / filtered** on 80, 443, 554, 8554, 3702, 8000, 8080, 8899, 34567, 8800, 9800 |
| `<redacted-nat-host-1>` | no reply | filtered on all probed ports |
| `<redacted-nat-host-254>` | no reply | filtered on all probed ports |

The distinction matters and is the honest reading:

- **`filtered` (timeout), not `refused` (RST)** — a closed port on a *reachable* host
  answers with an immediate reset. Nothing here resets. There is no path to that subnet
  at all, so we never learn whether any camera port exists.
- No `<redacted-nat-subnet>.x` entry ever appears in `ip neighbor` — the subnet is not on this link.

### 3.4 The extender, identified

| Evidence | Value |
|---|---|
| LAN address of the extender | `<redacted-lan-host-3>` |
| MAC | `<redacted-mac-c>` |
| OUI vendor (IEEE registry) | **Tenda Technology (Dongguan)** |
| SSID it broadcasts | **`kusankurdhar`**, channel 6 (2437 MHz) |
| BSSID of that SSID | **`<redacted-mac-c>`** — the *same MAC* as `<redacted-lan-host-3>` |
| Open TCP ports | **none** in a bounded check (22/23/53/80/81/443/554/1900/8080/8443/8899/9000) |
| Seen from | **both** the workstation (`nmcli` after rescan, RSSI 29) **and** the Mi A3's own scan list (RSSI -79) |

The identity of the extender is therefore **proven**: one Tenda radio that is
simultaneously (a) a client of the Airtel network at `<redacted-lan-host-3>` and (b) the access
point for SSID `kusankurdhar`.

### 3.5 Operating mode: **router/NAT (double NAT)** — INFERRED, and the inference is strong

Chain of reasoning, each step measured:

1. The first camera (`<redacted-lan-host-19>`) is on `<redacted-lan-host-0>/24`, exactly as an Airtel-DHCP
   client would be.
2. The second camera is reported at `<redacted-nat-host-164>` **with DHCP/automatic assignment**.
3. The Airtel router's DHCP pool is `<redacted-lan-host-0>/24` — it cannot have issued a
   `<redacted-nat-subnet>.x` lease.
4. The only device on the Airtel LAN that also owns a radio serving its own network is the
   Tenda at `<redacted-lan-host-3>` (SSID `kusankurdhar`).
5. `<redacted-nat-host-0>/24` is unroutable from `<redacted-lan-host-0>/24`, and packets to it leave for the
   internet.
6. Tenda's default LAN subnet is `<redacted-nat-host-1>/24` — consistent with the reported camera
   address, and the classic default for this vendor.

If the extender were in **bridge/AP mode** (the brief's preference), a camera associating
to it would appear on `<redacted-lan-host-0>/24` with an Airtel-assigned address, and the subnet
would be shared with the router — neither is the case.

**Marked INFERRED, not verified:** the extender's LAN gateway is expected to be
`<redacted-nat-host-1>` and the camera's gateway is expected to be that address. Neither can be
observed from the Airtel side of the NAT; they must be read from a host on
`<redacted-nat-host-0>/24`. Its admin UI is also not reachable from here (no open ports on its
Airtel-side interface), which is good practice but means nothing can be read out of it
remotely.

### 3.6 Second camera's actual gateway/subnet relationship — summary

| Property | Status |
|---|---|
| Camera's own subnet | `<redacted-nat-host-0>/24` — **INFERRED** from its address + the extender's own DHCP |
| Camera's DHCP provider | **the extender**, not the Airtel router — INFERRED from step 3 above |
| Camera's gateway | expected `<redacted-nat-host-1>` — **NOT VERIFIED** |
| Relationship to the Airtel LAN | one NAT boundary away; no route, no shared L2 |
| Is the address stable? | **Unknown** — DHCP assignment, so it can change. Anything built on the bare address is fragile until it is pinned |

## 4. Reachability from the HomeGuardian nodes

Both HomeGuardian phones are on the **Airtel** network, so both share the blocker:

| Node | Address | Route to `<redacted-nat-host-164>` | Result |
|---|---|---|---|
| Workstation (wlp6s0) | `<redacted-lan-host-6>/24` | via `<redacted-lan-host-1>` (default) | 100 % ICMP loss, all TCP filtered |
| Mi A3 `laurel_sprout` (adb over Wi-Fi) | `<redacted-lan-host-14>/24`, default `<redacted-lan-host-1>` | via `<redacted-lan-host-1>` (table 1030) | **100 % ICMP loss** |
| Panasonic ELUGA I7 (adb over Wi-Fi) | `<redacted-lan-host-16>` | same LAN as above | not separately probed (same subnet, same result by construction) |

So the answer to the milestone's question *"can the reported camera address be contacted
directly or is it hidden behind an extender NAT boundary?"* is: **hidden behind the
extender's NAT boundary — from every HomeGuardian node, including the Mi A3.**

## 5. Stage 2 — camera interfaces: BLOCKED, with a documented expectation

Nothing about the second camera's interfaces can be tested today, because there is no path
to it. What can be said now, honestly labelled:

**Owner-supplied (not verified here):** software `<redacted-cam2-software>`,
firmware `<redacted-cam2-firmware>`, DHCP assignment, **ONVIF option not visible
in Advanced settings**.

**What that likely implies (documented, NOT a measurement):**

1. The vendor's own FAQ for this family states the ONVIF switch "will not be displayed on
   the APP if the device can not support" it. An absent switch is therefore evidence that
   this firmware **does not offer ONVIF at all** — the opposite situation from the first
   camera, whose switch *was* present and, when enabled, exposed an unauthenticated
   interface.
2. That same firmware line appears in the public reverse-engineering project
   `archcloudy/v380-cameras` (a port of `prsyahmi/v380`), which lists a tested camera with
   `<redacted-cam2-software-family>_V1.3.7.0_20231211` / `<redacted-cam2-firmware-family>_V1.0_WF_20230607` — the **same
   `<redacted-cam2-software-family>` platform and the same `<redacted-fw-token>_XHR_E` model line** as ours, a different
   build date. That project's premise is that "newer V380 cameras use encryption for video
   and audio streams", i.e. it exists to **decrypt the proprietary stream**, and its
   RTSP/ONVIF capability is a **server the project runs**, not a camera feature.
   **Implication: for this family, expect the proprietary vendor protocol on TCP/8800 and
   no native RTSP/ONVIF.**
3. Therefore the likely Stage 2 outcome is one of:
   - **(a)** the camera exposes only the vendor ports (`8800`/`9800`) → no standard local
     interface → a **feasibility report** is the deliverable, per the milestone; or
   - **(b)** the camera exposes more than expected (e.g. an HTTP interface), which would be
     a genuinely new finding worth reporting immediately.

**Both cases require getting onto `<redacted-nat-host-0>/24` first.** That is the single blocker.

**Explicitly NOT done:** no proprietary authentication bypass, no SD-card configuration
file, no firmware flash, and no attempt to speak the encrypted vendor protocol. Doing so is
a scope decision the owner has to make (§6.4).

## 6. Stage 3 — recommended architecture

### 6.1 Ranked options

| # | Approach | Changes required | Effect on other devices | Verdict |
|---|---|---|---|---|
| **1** | **Put the extender into bridge/AP mode** so both cameras live on the Airtel LAN (`<redacted-lan-host-0>/24`) | extender admin access + **owner approval** (it is a reconfiguration) | clients of `kusankurdhar` are re-addressed by the Airtel DHCP and briefly drop; extender loses its own admin subnet | **RECOMMENDED as the target architecture** — one flat authenticated subnet, no NAT traversal, no forwarding, WS-Discovery works, both cameras reachable by every authorized node |
| **2** | **Temporary: get one node onto `<redacted-nat-host-0>/24`** — an Ethernet cable from a LAN port of the extender into the workstation's idle `enp5s0`, *or* associate the workstation to `kusankurdhar` | **none** on the extender or router; in the cable case none at all | none (option adds a second interface; Wi-Fi keeps its Airtel address and default route) | **DO THIS FIRST** — unblocks Stage 2 today with zero reconfiguration and zero risk to the other cameras |
| 3 | Leave double NAT, add per-service port forwards on the extender | extender admin + approval; and it keeps two subnets | none | **Not recommended** — fragile, breaks ONVIF/mDNS discovery (multicast does not cross a NAT router), and every forward is an exposed service path |
| 4 | Leave double NAT, port-forward on the **Airtel** router to reach the camera from outside | router admin | **exposes the camera to the internet** | **REJECTED — forbidden by the brief and by this project's standing rules** |
| 5 | Speak the proprietary encrypted vendor protocol and re-expose RTSP/ONVIF from a local decoder process | new explicit owner decision + security review | none | Possible fallback, **not sanctioned today** — see §6.4 |

### 6.2 Proposed target architecture (the same shape the project already has)

```
                    Airtel router / ONT (ZTE F670L V9.0)  <redacted-lan-host-1>
                     |                     |
        [Wi-Fi <redacted-wifi-ssid>]        [extender set to BRIDGE/AP]
             |                                   |
   Mi A3 (HomeGuardian C++)          camera 1 (<redacted-lan-host-19>)   camera 2 (<redacted-lan-subnet>.x)
   workstation (diagnostics)                   |
                                   HomeGuardian hub, device registry,
                                   consent-gated acquisition, per-payload authorization
                                               |
                          web dashboard + Alexa surface, later reachable
                          through approved VPN/HTTPS (Oracle = gateway only,
                          no media processing, no transcoding, no relay)
```

Both cameras then sit on **one private, authenticated subnet**; each is a separately
registered device with its own identity, credentials, consent record and availability
state. Nothing is port-forwarded, nothing is exposed publicly, and no media leaves the LAN.

### 6.3 If the extender cannot be switched to bridge mode

Then the second camera stays on an island. The only non-exposing workaround is to run the
HomeGuardian **acquisition node on that island** (a small always-on host on
`<redacted-nat-host-0>/24`) and have it deliver observations and (later, approved) media to the hub
through the existing mutual-TLS device transport — the same pattern already used for the
Android node, where the device dials the hub rather than the hub dialling the camera. This
works, but it adds a host, and it does not remove the double NAT. Bridge mode is simpler
and is preferred.

### 6.4 The proprietary-protocol question (a decision for the owner, not for me)

If Stage 2 finds no standard interface, the fallback is the community approach: decode the
vendor's encrypted stream on TCP/8800 and re-expose RTSP/ONVIF locally, with authentication
on. Three things must be said plainly:

- Earlier milestones forbid "proprietary-protocol bypasses"; using this path needs a **new,
  explicit owner decision** plus a security review, not an assumption.
- The reference decoder (`archcloudy/v380-cameras`) **defaults to NO authentication** on
  the RTSP/ONVIF it exposes (`--secure` is required to turn it on). That is exactly the
  failure just measured on the first camera. Any such process on this project must ship with
  authentication **on by default and non-optional**, bind to the LAN only, and never be
  reachable off-network.
- It requires camera credentials and a device ID at runtime, so it needs a credential
  store with the same posture as the rest of this project (file refused if group/other
  readable, never logged, never in a status payload).

## 7. First camera — verified separately (evidence NOT mixed with the second)

Re-checked at the start of this milestone, configuration untouched:

```
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899   ->  exit 0, PROTECTED
ports: 554 refused, 8899 refused, 3702 refused, 8800 OPEN, 9800 OPEN
```

**The HG-V380-04 exposure is CLOSED.** The owner disabled the ONVIF setting, `554`/`8899`
are gone, and the camera is back to its as-shipped proprietary-ports-only state. The first
camera's documented state and measurements (`HG_V380_01`/`_02`/`_04`) remain its own and
must not be transferred to the second camera: **different firmware family**
(`<redacted-cam1-firmware>` / `<redacted-cam1-software-family>` vs the second camera's `<redacted-cam2-firmware-family>` /
`<redacted-cam2-software-family>`), different platform, and a different ONVIF outcome (present-but-unauthenticated
there vs absent here).

## 8. Stage 4 — protecting both cameras (rules, to be applied when the second is onboarded)

1. **Separate device identity** per camera in the registry — never one shared row.
2. **Separate credentials**; no credential from camera 1 is reused on camera 2, and the
   ONVIF/stream password must not be the app account password by default.
3. **Separate consent records** (`subject_scope`/`purpose`/`data_category` per device), so
   withdrawing consent for one camera cannot silently affect the other.
4. **Separate capability records** — do not copy camera 1's capabilities (H.265 SDP,
   PTZ present, no audio track, no ONVIF audio output) onto camera 2. Every capability claim
   for camera 2 must be measured on camera 2.
5. **Separate availability state** — a camera that is offline behind the extender must not
   mark the other offline.
6. **Do not propagate the first camera's ONVIF mistakes**: any interface enabled on camera 2
   must pass `v380_security_check.sh`-style verification (anonymous media and anonymous
   device/account disclosure are both failures) **before** it is registered.
7. Both cameras must keep working in V380 Pro; nothing in the proposed architecture
   requires disabling the vendor app, and no cloud path is used by HomeGuardian.
8. The **release-security gate for real-camera remote access stays mandatory** — this
   milestone does not touch, relax or bypass it.

## 9. Owner actions required

**Action A — unblock Stage 2 (do this one first; no reconfiguration, no risk):**
Connect one of the following, then tell me it is done:
- **A1 (preferred):** an **Ethernet cable from a LAN port of the extender** to this
  workstation's Ethernet port. The port is present and idle (`enp5s0`,
  `<redacted-mac-h>`, currently NO-CARRIER). This puts the workstation on
  `<redacted-nat-host-0>/24` while its Wi-Fi keeps the Airtel address — both networks reachable at
  once, nothing reconfigured.
- **A2:** associate the workstation's Wi-Fi to SSID **`kusankurdhar`** instead. Also fine,
  but it moves this machine off the Airtel LAN for the duration, so tools on that LAN
  become unreachable meanwhile. If you choose A2, enter the Wi-Fi password in the desktop
  network UI yourself — **do not paste it here or into any file in the repo.**

After either, I will immediately: confirm the camera's real subnet/gateway, pin down
whether its address is DHCP-leaseable, enumerate its interfaces, and run the same
authentication assessment that was run on camera 1.

**Action B — target architecture (needs your explicit approval, because it disrupts other devices):**
Approve switching the extender from router/NAT mode to **bridge/AP mode** so the second
camera joins `<redacted-lan-host-0>/24`. Impact, stated plainly: every device currently on
`kusankurdhar` is re-addressed by the Airtel DHCP and loses connectivity for the duration
of the change; the extender's own `<redacted-nat-host-1>` admin subnet disappears, so it is then
administered through the Airtel LAN. I will not touch the extender without this approval,
and I would prefer to have it done with you present.

**Action C — a fact only you can confirm:** is the second camera still at
`<redacted-nat-host-164>` as shown in the screenshots, and is anything else besides the second camera
connected to `kusankurdhar`? The second answer decides how disruptive Action B really is.

**Action D — the ONVIF question on this model:** the screenshot shows no ONVIF option.
Please confirm you checked *Settings → Advanced settings → ONVIF setting* on **this**
camera and that the row is absent (rather than just not noticed). That determines whether
Stage 2 is a short interface test or a feasibility report.

## 10. Reproduce

```bash
bash scripts/hg_second_camera_check.sh <redacted-nat-host-164>   # targeted reachability + route verdict
bash scripts/v380_security_check.sh <redacted-lan-host-19> 554 8899   # first camera is still PROTECTED
tracepath -n -m 6 <redacted-nat-host-164>                        # shows the path leaving for the ISP
nmcli -t -f BSSID,SSID,CHAN,SIGNAL dev wifi list       # kusankurdhar -> <redacted-mac-c>
ip neighbor | grep '<redacted-lan-host-3> '                      # same MAC, the extender on the Airtel LAN
```

## 11. Status

| Stage | Status |
|---|---|
| 1 — extender topology | **COMPLETE.** Mode: router/NAT (double NAT), inferred from six measured facts. Extender identity proven (Tenda, `<redacted-lan-host-3>` / `<redacted-mac-c>`, SSID `kusankurdhar`). Route, gateway and reachability measured from the workstation **and** the Mi A3 |
| 2 — second camera interfaces | **BLOCKED — no path to the camera.** Expected outcome documented, including the strong indication that this firmware family has no native ONVIF/RTSP. Nothing assumed, nothing bypassed |
| 3 — integration approach | **RECOMMENDED:** temporary direct Ethernet (Action A) now; bridge/AP mode (Action B) as the target. Port-forwarding rejected. Proprietary-protocol fallback flagged as an owner decision with a mandatory authentication posture |
| 4 — protecting both cameras | Rules specified (§8); applies at onboarding. **First camera verified PROTECTED and its exposure closed** |

**Verdict: CONDITIONAL — a validated, secure path exists (bridge/AP mode on the extender,
both cameras on the authenticated Airtel LAN), but Stage 2 cannot proceed and no media can
be tested until the owner performs Action A. No camera media is reachable today, and no
insecure workaround will be used to make it reachable.**
