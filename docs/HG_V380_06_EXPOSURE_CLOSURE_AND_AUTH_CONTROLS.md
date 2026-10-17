# HG-V380-06 — Exposure Closure, Correct Check Semantics, and Authentication Options

**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Workstream branch:** `experiment/hg-v380-ec39-integration`
**Evidence SHA of the exposure report:** `1ccc028d3ccdceac5fc700f181956dd899c6d3db`
**Previous commit:** `2784d770b475ae50d34e27e8c3cf7909322c3f5e`
**Camera under test:** first camera only, `192.168.1.19` (MAC `30:be:29:3a:d3:b7`)

---

## 0. ANSWER FIRST

1. **The exposure is CLOSED.** `tcp/554` and `tcp/8899` no longer exist on the camera;
   the security check now classifies both **DISABLED**. The camera still answers on its own
   vendor ports `8800`/`9800`, which is the path the V380 Pro app uses. No reminder of the
   exposure is reachable today.
2. **"No anonymous access" is no longer reported as "protected".** The checker now
   distinguishes **DISABLED** from **AUTHENTICATION ENFORCED** (and **EXPOSED** from
   **INCONCLUSIVE**), and it will only claim a *secured* result when a negative
   unauthenticated test **and** a positive authorized test both pass. Verified by a
   self-test that drives all four classifications against local stubs (6/6) — and that
   self-test found two real bugs in the first draft of the new code.
3. **No native authentication mechanism for ONVIF/RTSP is documented for this firmware
   family, and there is positive evidence that enabling ONVIF does NOT protect the stream.**
   The app does expose a settable *device username/password*, but when ONVIF was enabled on
   this camera it demanded no credential of any kind. **Treat native authentication as
   unproven and not to be relied on.**
4. **Recommended path: an isolated camera network** with firewall-restricted access through
   one trusted HomeGuardian component (§4). It needs no firmware modification, reuses
   hardware the owner already has, and does not depend on the camera's own access control.
   It requires the owner's approval before any configuration is touched.
5. **Device time is untrustworthy and is recorded as a timestamp-integrity defect** (§6):
   the camera clock measured **9,001.5 s (2 h 30 m 1.5 s) behind true UTC**.

---

## 1. Stage 1 — the exposure is closed

### 1.1 Port state, before and after

| Port | While ONVIF was enabled (HG-V380-04, `1ccc028`) | Now |
|---|---|---|
| 554 RTSP | OPEN, served live H.265 to unauthenticated clients | **refused** |
| 8899 ONVIF | OPEN, disclosed device/account state unauthenticated | **refused** |
| 3702 WS-Discovery | answered anonymously | **refused** |
| 8800 | OPEN | OPEN (unchanged) |
| 9800 | OPEN | OPEN (unchanged) |

### 1.2 Both checker revisions agree the exposure is gone, and they say different things about it

```
old revision (preserved):  bash scripts/v380_security_check.sh 192.168.1.19 554 8899
                           -> "PROTECTED - no unauthenticated access found", exit 0
new revision (v2):         -> RTSP/554 DISABLED, ONVIF/8899 DISABLED, exit 0
```

The old wording was the defect this milestone exists to fix: **`PROTECTED` was true of a
camera whose protection was simply absent.** The new wording cannot make that claim.

### 1.3 Original security evidence preserved

The HG-V380-04 exposure evidence (anonymous SDP, RTP packet counts, anonymous
`GetUsers`/`GetNetworkInterfaces`) is unchanged in
`docs/HG_V380_04_ONVIF_ACTIVATION_VERIFICATION.md`, and the exact checker revision that
produced it is preserved at commit **`2784d77`** (`git show 2784d77:scripts/v380_security_check.sh`).
Its numbers were not edited, recomputed or "cleaned up".

### 1.4 The V380 Pro application

| Claim | Basis |
|---|---|
| No camera setting was changed by this workstream | every ONVIF call made was read-only; no write was issued, no credential sent |
| The camera still serves its vendor protocol | `8800` and `9800` open and behaving as in `HG_V380_02` |
| **The app itself remains usable** | **OWNER OBSERVATION REQUIRED** — I cannot see the phone, and the app's preview path is in the cloud/vendor stack. There is no mechanism by which a read-only probe or the ONVIF toggle would break it, and the vendor documents the ONVIF switch as independent of normal operation, but this is a reportable observation, not something I measured |

## 2. Stage 2 — corrected check semantics

### 2.1 The four classifications, as implemented

| Classification | Meaning | Measured as |
|---|---|---|
| **DISABLED** | no listener at all | connection refused / reset (errno 111 / 104) |
| **AUTHENTICATION ENFORCED** | a listener exists and *demands* credentials | RTSP `401` + `WWW-Authenticate`; ONVIF HTTP `401` or a SOAP `NotAuthorized` fault |
| **EXPOSED** | a listener answers an unauthenticated client with data | `DESCRIBE` 200 + SDP (and RTP packet count), `GetDeviceInformation` 200 |
| **INCONCLUSIVE** | a listener exists but neither challenged nor disclosed | no answer, empty close, malformed reply, transport error |

Exit codes are fail-closed: `0` only when nothing is EXPOSED and nothing is INCONCLUSIVE,
`1` on exposure, `3` on any inconclusive result, `2` on usage error.

### 2.2 DISABLED is explicitly not a security claim

The new verdict text for this camera is deliberately unflattering:

> DISABLED - no RTSP or ONVIF listener is present. This is NOT "authentication enforced":
> nothing is protected because nothing is running. A secured integration cannot be claimed
> from this result alone.

### 2.3 The positive half — authorized confirmation

`--creds <file>` makes the checker perform an **authorized** request as well:
RTSP `DESCRIBE`/`SETUP`/`PLAY` with Digest (Basic fallback), and ONVIF with
WS-Security `PasswordDigest` (HTTP Digest fallback). Then:

- challenge present + authorized request succeeds → **SECURE** (`exit 0`)
- challenge present + authorized request fails → **INCONCLUSIVE** (`exit 3`), because a
  secured integration needs *both* halves
- no credentials supplied → the positive half is reported as **NOT ATTEMPTED**, never as passed

Credential handling: a JSON `{"username","password"}` file is **refused if it is group- or
other-accessible** (verified: a `0644` file is rejected with exit 2 and nothing is sent), and
its contents are never printed, logged or echoed.

### 2.4 The checker is itself tested — 6/6

```
bash scripts/v380_security_check_selftest.sh      # local stubs on 127.0.0.1 only
  PASS  DISABLED  (closed ports)                    exit 0
  PASS  EXPOSED   (anonymous media)                 exit 1
  PASS  ENFORCED  + authorized CONFIRMED            exit 0
  PASS  ENFORCED  + authorized NOT CONFIRMED        exit 3
  PASS  ENFORCED  + authorized NOT ATTEMPTED        exit 0
  PASS  INCONCLUSIVE (mute listener)                exit 3
  == result: 6 passed, 0 failed ==
```

This exists because a checker that has only ever printed one result against one camera has
not been tested — it may be incapable of saying anything else. It immediately paid for
itself by catching **two real defects in the first draft**:

1. the DISABLED test matched the literal word "refused" while the error string carried
   `errno=111`, so a refused connection was classified INCONCLUSIVE;
2. the authorized RTSP flow derived the Digest challenge from the *successful* reply (which
   carries no `WWW-Authenticate` header), so `SETUP`/`PLAY` were sent unauthenticated and the
   authorized test failed against a stub that authenticates correctly.

A third defect was found the same way: the ONVIF retry reused a socket the device had already
closed with `Connection: close`. All three are fixed, and the harness re-runs them.

## 3. Stage 3 — what authentication actually exists for this firmware family

### 3.1 Documented, from the vendor (retrieved 2026-10-17)

| Control | Vendor documentation | What it means here |
|---|---|---|
| **Device username + password** — settable in the app | *"After the APP is connected to the device, click the [Settings] icon in the Device list, select [Password]-[Change Device Password]"* — `v380camera.com/en/article/how-to-set-or-modify-the-camera-password` | A per-device credential exists and the owner can change it. It is **separate from the V380 cloud account login** (`v380.org/password-setting` covers the account; this covers the device) |
| Device credential is genuinely enforced by the device | *"it is necessary to fill in the correct [Device username] and [Device password] in this window to connect"* — `v380camera.com/en/article/device-username-or-password-error` | The app's own preview path authenticates with it |
| **ONVIF switch** | *"select [Advanced settings]-turn on/off [Onvif setting] ... needs to be supported by the device firmware"* | The switch exists on camera 1 and was enabled — with **no authentication attached to it** (measured) |
| NVR/WMS integration is a supported use case | *"The V380 camera can be connected to NVR through wired (or wireless) methods"* | Standard-interface integration is intended by the vendor |

### 3.2 What is NOT documented — and the evidence against relying on it

- **No vendor article describes requiring authentication on ONVIF or RTSP**, no separate
  "ONVIF user" concept, and no statement that enabling ONVIF enables access control.
- **Measured counter-evidence (HG-V380-04, `1ccc028`):** with ONVIF enabled on this exact
  camera, an unauthenticated client received the SDP, a live H.265 stream (360 and 922 RTP
  packets), the MAC/IP, the `admin` account name and the stream URI. **The device credential
  did not gate any of it.** Setting a device password is therefore *not* evidence that the
  interface is protected.
- **Do not assume the V380 Pro cloud login protects RTSP/CCTV access.** It is a different
  credential for a different layer, and per the measurement above neither credential was
  demanded at the local interface.
- Third-party pages claim default RTSP credentials (`admin`/`admin`) for "V380" cameras. That
  is a **third-party claim about a broad OEM family, not about this unit, and it was not
  attempted** — trying default credentials is credential guessing and is out of scope.

### 3.3 Conclusion for this family

> **There is no documented, verified way to make this camera's ONVIF/RTSP interface require
> authentication — and there is direct evidence that enabling ONVIF does not.** The only
> reliable control is therefore *outside* the camera: who is allowed to reach it.

This does not make the camera unusable; it changes which component must be trusted. That is
the isolated-network design below.

## 4. Recommended: an isolated camera network with one trusted component

### 4.1 Shape

```
        main home LAN (192.168.1.0/24)          isolated camera network
        Airtel router, Mi A3, workstation       (no route to the internet, no route in
                    |                            except through the broker)
                    |                                    |
            [trusted HomeGuardian broker host]  <---->  [camera(s)]
             - the ONLY host with a second interface on the camera segment
             - default-DROP firewall on that interface, both directions
             - holds the camera credentials; never logs or exports them
             - pulls frames, applies consent + per-payload authorization,
               converts to Observations, sends them through the EXISTING transport
             - no transcoding, no media relay, no cloud, no port forwarding
```

Properties this buys, none of which depend on the camera's own access control:

- the camera is reachable **only** by the broker — every other host, including any phone or
  laptop on the home LAN, has no path to it;
- there is **nothing to port-forward**: no NAT rule, no listener exposed off-network;
- the camera's lack of authentication stops mattering, because the population of clients that
  can reach it is exactly one;
- consent and authorization live in HomeGuardian, where they can be audited, rather than in
  firmware that ignores them.

### 4.2 The hardware is already in the house

The extender measured in `HG-V380_05` is a NAT'ing router: **it already isolates its
`192.168.0.0/24` segment from the main LAN, and nothing can reach in from upstream.** The
existing topology can therefore be turned into the design above without new equipment:

1. connect the broker host to the extender's LAN side (the idle `enp5s0` Ethernet port takes
   a cable — the same zero-config step `HG-V380_05` recommended);
2. leave the extender's upstream as-is, and **add no port forwarding** — the camera stays
   unreachable from the main LAN;
3. bind the camera-facing work of the broker to that interface only, with a default-DROP
   firewall policy on it;
4. move the second camera (and, if the owner prefers, the first) onto that segment's Wi-Fi,
   with the extender's SSID password rotated and known only to the owner and the broker.

If the owner instead wants the permanent flat-subnet architecture from `HG_V380_05` (bridge
mode), the isolation has to be reintroduced some other way — a VLAN-capable AP or a dedicated
camera SSID with client-isolation plus a firewall — because a flat subnet puts the camera
within reach of every device on the LAN.

### 4.3 What needs owner approval before anything is configured

- any change to the extender (mode, DHCP, SSID, firewall) — **not touched, approval required**;
- the broker host's own firewall rules on the camera-facing interface;
- which camera(s) live on the isolated segment;
- the release-security gate for real-camera remote access **stays mandatory** and is not
  relaxed, bypassed or re-scoped by this design. Nothing here enables remote viewing.

## 5. Preserved architecture (Stage 4)

- **HomeGuardian C++ on the Mi A3 remains the intelligence engine.** The broker pulls and
  normalizes; detection, fusion, alerting and consent stay in the existing C++ pipeline, and
  observations enter it through the existing `Observation` → `Pipeline` path — never a second
  event engine.
- **Future authenticated web viewing, listening, talkback and Alexa-style clients are
  preserved as goals**, not foreclosed: they sit behind the same consent gate and the same
  authenticated boundary as every other surface. Talkback is explicitly *not* assumed —
  camera 1's ONVIF reported `GetAudioOutputs → Action Not Implemented`, so no ONVIF audio
  output exists there.
- **The Oracle VPS stays a remote-access gateway** (VPN/HTTPS coordination). It performs no
  default video processing, no transcoding, no storage and no media relay — and in this design
  it does not sit in the media path at all.

## 6. Timestamp integrity — recorded as a defect, with the rule that follows

| Finding | Value |
|---|---|
| Camera ONVIF clock vs true UTC | **9,001.5 s behind** (camera reported `2026-10-17T11:34:27Z`; verified true UTC was `14:04:28Z`) |
| How true UTC was established | external HTTP `Date:` header (`Sat, 17 Oct 2026 14:04:46 GMT`) **and** `timedatectl` (NTP active, synchronised) — not the local clock alone |
| Reported timezone | `TZ = PST0PDT` — not a valid POSIX zone for this deployment |
| Other identity hazards on the same device | `SerialNumber = 123456789` (a placeholder, identical across units), `Model = IPCamera` (generic), ONVIF `FirmwareVersion = 2.4` disagreeing with the app-side string, and an ONVIF encoder config (`H264 2304x2592`) contradicting the stream's actual SDP (`H265`) |

**Rules this imposes on any future integration:**

1. **Use trustworthy receive timestamps.** Every observation is stamped with the HomeGuardian
   host's clock at the moment of reception. Device-supplied time is **not** used as the
   observation timestamp.
2. If device time is ever used (e.g. for correlating an ONVIF event), it must be **stored
   separately, flagged as device time, and carry the measured offset** — never silently mixed
   into the same field.
3. **Never key device identity on the serial** (it is a placeholder) or on the model string
   (it is generic). Identity comes from the LAN MAC and, for delivery, from the mutual-TLS
   certificate identity already used by the device transport.
4. Clock skew must be **re-measured**, not remembered: the offset is a property of the unit's
   configuration at the time it was read, and it will change if NTP is ever fixed.

## 7. Recommended integration path (ordered)

1. **Owner confirms** the V380 Pro app still previews both cameras normally after the toggle
   (§1.4) — the one thing I cannot observe.
2. **Owner approves** the isolated-segment design (§4.3), or states a preference for bridge
   mode with a different isolation mechanism.
3. **Cable the broker host** to the camera segment (idle `enp5s0`, zero configuration).
4. **Measure, then decide**: run `hg_second_camera_check.sh` on the segment to establish the
   real subnet/gateway, then enumerate the second camera's interfaces. For the **first**
   camera, re-enabling ONVIF is **not** proposed — if it is re-enabled for any reason, the
   first action afterwards is `v380_security_check.sh`, and it must not be registered while
   the result is EXPOSED or INCONCLUSIVE.
5. **Only then** implement the adapter behind the `V380Transport` seam planned in
   `HG_V380_03`, pulling through the broker, with consent before connect and per-payload
   authorization.
6. **Re-run the checker** before and after any camera setting change; state its classification
   in the milestone report rather than a bare "protected".

## 8. Evidence paths

| Artifact | Path |
|---|---|
| Corrected checker (four classifications, authorized half, exit codes) | `scripts/v380_security_check.sh` |
| Proof the classifier works and can fail (6 cases) | `scripts/v380_security_check_selftest.sh` |
| Original exposure evidence (unchanged) | `docs/HG_V380_04_ONVIF_ACTIVATION_VERIFICATION.md` @ `1ccc028` |
| Original checker revision | `git show 2784d77:scripts/v380_security_check.sh` |
| Topology / second camera | `docs/HG_V380_05_SECOND_CAMERA_EXTENDER_TOPOLOGY.md` |

Commands:
```bash
bash scripts/v380_security_check_selftest.sh                       # 6/6, no camera contacted
bash scripts/v380_security_check.sh 192.168.1.19 554 8899          # -> DISABLED, DISABLED
bash scripts/v380_security_check.sh <cam> 554 8899 --creds c.json  # adds the authorized half
bash scripts/hg_second_camera_check.sh 192.168.0.164               # second camera reachability
```

## 9. Status

| Stage | Status |
|---|---|
| 1 — verify exposure closed | **COMPLETE.** 554/8899/3702 gone, camera still alive on 8800/9800, original evidence preserved unchanged. App usability = OWNER OBSERVATION PENDING |
| 2 — correct check semantics | **COMPLETE.** DISABLED / AUTHENTICATION ENFORCED / EXPOSED / INCONCLUSIVE, fail-closed exits, authorized confirmation with a permission-checked credential file, and a 6-case self-test that found three real defects in the first draft |
| 3 — security controls | **COMPLETE.** Device username/password is documented and settable; **no documented way to require ONVIF/RTSP authentication, with measured counter-evidence.** Isolated-network design proposed; needs owner approval |
| 4 — preserve architecture | **RECORDED.** Mi A3 stays the engine; VPS stays a gateway; timestamp integrity, serial/model/clock pitfalls written down as rules |

**Verdict: the exposure is closed and the tooling that said so can now be trusted to say
otherwise when it should. The camera's own access control is NOT a control we can rely on;
integration should proceed only through the isolated-segment design, and no camera, extender,
router or firewall setting will be changed without explicit approval.**
