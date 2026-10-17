# HG-CAM-10 — Onboarding an additional home-network camera

**Issue:** #19 (`HG-CAM-10 — Hermes 3: onboard additional authorized home-network webcam from screenshots`)
**Date:** 2026-10-18
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Builds on:** `HG-NET-02C` (site connectors, control-only coordination), `HG-NET-ARCH-01` (network-independent
design), the `HG-V380-0*` discovery and exposure-closure evidence

## Privacy note, first

**No LAN address, MAC address, V380 device identifier, Wi-Fi name or credential appears in this
document.** Identification is described structurally; every device-specific value lives only in the
owner-restricted evidence file outside the repository. This document must remain publishable.

Private values referenced below as `A1`, `A2`, `A3`, `A2-old` (addresses) and `S1`, `S2`, `S3`
(fingerprints): the real values are in the owner-restricted evidence, not here.

---

## 1. Device identification (issue step 1) — PASS

**Method.** Three independent grounds, deliberately in this order, because each one alone is
insufficient:

| Ground | What it proves | What it cannot prove |
|---|---|---|
| NIC fingerprint (normalized MAC) | two observations are (or are not) the same physical interface | which camera model it is; a *randomized* MAC is not durable (the inventory refuses those) |
| Documented port signature | the device behaves like this camera family | model, firmware, serial |
| Owner-provided identity from the vendor app | the actual unit | (this is the one that names the device — and it is **BLOCKED**, see §3) |

An IP address is used **only** as an observation of where a device was seen, never as identity — and
that is not a stylistic choice: in this very milestone the development workstation's own DHCP lease
had changed since the previous session, and one of the two known cameras had moved address too.

**Result: three distinct physical camera units are present.**

| Logical role | Fingerprint | Address now | Address before | Evidence |
|---|---|---|---|---|
| camera 1 (known) | `S1` | `A1` | `A1` (unchanged) | identical fingerprint and signature as the HG-V380-01 verification |
| camera 2 (known) | `S2` | `A2` | `A2-old` (a different subnet address, then another) | fingerprint `S2` matches the earlier camera-2 verification; `A2-old` is now **absent** from the LAN |
| **camera 3 (new)** | `S3` | `A3` | *never seen before* | `S3` appears in no earlier HomeGuardian evidence; same Wi-Fi silicon vendor as cameras 1 and 2; identical port signature |

**Why `A3` is a genuinely new device and not camera 2 moving** (the question the issue says must not be
answered wrongly):

1. Camera 2's fingerprint `S2` is live at `A2` **in the same scan pass** in which `S3` appears at
   `A3`. One physical unit cannot hold two live NICs.
2. `S3` has never appeared in any prior evidence in this repository.
3. Camera 2's previous address `A2-old` is absent — the lease moved, it was not shared.
4. Exactly three devices with the camera-family Wi-Fi silicon vendor are on the LAN, and all three
   are accounted for.

**Not proven, and not claimed:** the model, the firmware version and the V380 device identifier of
camera 3. Those require the vendor app's own identity block (§3).

## 2. LAN reachability (issue step 2) — PASS

Read-only, low-rate, targeted: one ICMP packet per host over the local /24 plus a neighbour-table
read, then a TCP connect on the **documented camera interfaces only** (554 RTSP, 8899 ONVIF, 8800 and
9800 vendor control/media). No authentication attempt of any kind; no router, extender or camera
setting touched.

| Device | 554 RTSP | 8899 ONVIF | 8800 vendor | 9800 vendor | Interpretation |
|---|---|---|---|---|---|
| camera 1 (`S1` @ `A1`) | closed | closed | OPEN | OPEN | ONVIF/RTSP stay OFF as required; vendor listener only |
| camera 2 (`S2` @ `A2`) | closed | closed | OPEN | OPEN | same family signature |
| camera 3 (`S3` @ `A3`) | closed | closed | OPEN | OPEN | same family signature |
| camera 2's old address `A2-old` | — | — | — | — | **absent** from the LAN (no ICMP, no ARP entry) |

Discovery services: **no** mDNS answers for `_rtsp`, `_onvif`, `_http` or the vendor service, and
**no** ONVIF WS-Discovery responder for `NetworkVideoTransmitter`. Consistent with camera 1's ONVIF
being disabled and with cameras 2 and 3 offering no standard interface to advertise.

**Second pass, same day.** The same three fingerprints were present at the same addresses, so nothing
drifted between the two reads. That pass also produced the cleanest possible demonstration of the
principle this whole design rests on: the address this workstation itself held in the previous
milestone is now held by a **different device** (a randomized MAC that was not on the LAN before). One
address, two different devices, on this network, observed — which is exactly why an address may never
be an identity, and why the fingerprint (plus the owner's identifier) is what the inventory keys on.

## 3. Registry onboarding (issue step 3) — **PASS (privately verified)**

Camera 3 is now a registered device. The onboarding gate (in code, `CameraInventory::onboard`)
requires all three of:

1. a valid **durable** fingerprint (a randomized MAC is refused outright),
2. a **stable owner-provided device identifier** that is not an IP address,
3. an **explicit owner authorization** for that device.

The owner supplied a V380 Pro screenshot of the camera's own **General** settings page, which
carries the Device ID and the Equipment Model. The first read was **local OCR** (tesseract, on this
machine) - the configured vision backend rejected the request outright because its API key is
invalid, so no image was processed by any remote service - and that OCR returned three candidate
readings for one digit. The identifier now in use is the **owner's own read-out from the app
screen**, which supersedes the OCR: an identifier is only as good as its source, and here the vendor
app is the source, not a text-recognition pass over it. The identifier is **not** in this
repository — the record and the identifier are held in the owner-restricted evidence file, and the
committed code and tests use synthetic fingerprints and identities throughout.

The registration artifact was then verified against the real parser: it loads, it reports exactly
one authorized camera record, and a later observation of the **same NIC at a new address** resolves
as `known_moved` with the logical identity unchanged. That is the DHCP-churn guarantee, tested
against the real registration rather than a fixture.

**Finding worth keeping: the model string could not have done this job.** The screenshot's
Equipment Model is the V380 platform model code, and it is **the same code as camera 1's**. Cameras
1 and 3 are therefore the same hardware family — which means neither the model string nor the
vendor product name can tell those two units apart. The per-unit Device ID is the only app-side
identifier that distinguishes them, and that is precisely why the gate demands an owner-provided
identity instead of accepting a model, a product name or an address. (The device in the screenshot
is not shown to be camera 1, because camera 1 is independently accounted for on the LAN in the same
pass in which the third unit appears.)

**Recorded honestly:** the OCR misread one digit of the identifier, and that was caught and
corrected by the owner. It is worth naming because it is the exact failure mode this design guards
against elsewhere - a machine-derived value quietly becoming an identity. The correction cost one
character in a record held privately; had the identifier been an address or a model string, the same
correction would have been impossible to make soundly.

**Still not verified for this unit:** firmware/software version, IP/MAC as the app reports them, and
the retail model number with its specification table (chip, sensor, resolution, lens, PTZ range).
Those need the "View device QR Code" screen, the firmware-information screen for this device, the
"Reception" screen (Wi-Fi + IP), or the retail box. None of them are needed for registration; they
are needed before any specification or media claim about camera 3 is written.

### 3.1 Phase-2 verification checklist — the REAL record against the REAL registry

Not a fixture: the artifact below is camera 3's registration record, with the owner-supplied
identifier and the LAN-verified fingerprint, exercised end-to-end. **44 assertions, all passing.**
Raw output is in the owner-restricted evidence file.

| Group | Verified |
|---|---|
| Registration record | one authorized record; the logical id is not an address and contains none; media readiness sits at the observed fact; no firmware claimed without a verified read-out |
| Same address | resolves as the known camera; health online |
| **DHCP address change** | resolves as `known_moved`, **not** as a new device; logical identity unchanged; authorization survives; the old address becomes history, not identity; no duplicate record and no phantom candidate invented |
| Persistence across restart | the binding survives a save/reload; a further address change after the restart is still recognised as the same camera |
| Health / offline | silence past the timeout marks it offline **without deleting it**; recovery restores online with the same identity |
| Camera-specific consent, revocation, capture | authorized with its own credential; a foreign credential is refused; revocation takes effect; re-authorization restores access; consent withdrawal blocks collection; the operator's capture switch blocks collection independently; the registry holds exactly one device |

Camera 1 and camera 2 are untouched by all of this: this registry holds only camera 3's record, and
neither of the other two cameras' identities, credentials, consent or settings were read or written.

## 4. Reuse of the site-connector framework (issue step 4) — PASS (code) / BLOCKED (deployment)

The trust chain is unchanged and is now proven end-to-end for three cameras:

```
CameraInventory      decides WHO a device is   (durable fingerprint + owner identity; address is
                                                only an attachment, with bounded history)
      |
net::DeviceRegistry  holds AUTHORIZATION       (paired / consent / capture / revocation, per device)
      |
SiteConnector        enforces it PER FRAME      (re-checks authorization before each frame; a
                                                denial terminates the session and says which)
      |
SiteProfile          bounds REACH               (per-site CIDR allowlist: one site cannot reach
                                                another site's cameras)
```

Health, consent and control all come from the existing components; nothing was forked for the new
camera.

## 5. Authenticated local video/audio (issue step 5) — **FAIL (no such interface exists)**

**No ONVIF control is visible in the newest screenshot, and none is present on the wire.** The
measured interfaces on all three cameras are the vendor's own listeners only.

- Standard media ports (554, 8899): closed on all three.
- No discovery advertisement of any standard service.
- Enabling ONVIF is **forbidden** by the issue and would reintroduce camera 1's previously measured
  anonymous-media exposure.

The remaining candidate is the vendor's proprietary protocol, which requires reverse engineering
credentials or bypassing its authentication. **That is out of scope by explicit instruction and was
not attempted.** It is escalated for a separate owner decision, and it is not something this
milestone can turn into a PASS.

**Consequence:** video, microphone audio and two-way audio are all **BLOCKED behind that decision**,
and no live feed exists. Mocks in the test suite are labelled synthetic and are not evidence of camera
media.

## 6. Direct encrypted media path (issue step 6) — designed, not exercisable

`DirectMediaPlanner` decides the path, and it **cannot** choose the coordination hub under any input
(asserted exhaustively). For camera 3 as it exists today the decision is `unavailable`, because the
device declares no media capability — there is nothing to transport. Oracle WireGuard stays
**control-only**: it carries registration, heartbeat, authorization, consent/revocation, ICE
candidates, stream control, session termination and bounded telemetry, under a 1 KiB ceiling that
makes a video frame an encoding failure rather than a bandwidth surprise.

## 7. Always-on local connector (issue step 7) — **BLOCKED (hardware)**

No always-on host exists inside the camera site. The development workstation is not always-on, is
itself on a DHCP lease, and Ubuntu Docker is explicitly not a production component. The site
connector design does not depend on either. Candidate options are unchanged from HG-NET-02C: a small
always-on box wired into the camera LAN, or the Mi A3 **only** if it is genuinely co-located and
without interfering with its MCP service (Session 2's workstream). This is a procurement/placement
decision for the owner, not a coding task.

## 8. Behaviour tests (issue step 8) — PASS (in-process, against the real registry)

New `[inventory]` suite: **8 cases, 257 assertions, all passing.** Full suite: **7158 assertions /
555 cases, all passing** (no regressions).

| Test | Result |
|---|---|
| Three distinct cameras onboarded, each authorized separately, each collected from independently | PASS — three logical identities, three credentials; one camera's credential is rejected for another |
| Revoking ONE camera stops only that camera | PASS — the other two keep streaming |
| Withdrawing CONSENT on another stops only that one, with a different reported reason | PASS — `consent_missing`, not `revoked` |
| DHCP churn: a camera moves address and keeps its identity, authorization and history | PASS — identity and consent unchanged; the address is an attachment with a bounded history |
| A NEW device seen at a RECYCLED old address is not mistaken for the camera that used to be there | PASS — new device, new id, no inherited consent |
| Offline after silence, then recovery with the same identity | PASS |
| Randomized (locally administered) MAC cannot ground a durable identity | PASS — refused on both registration and observation |
| Registration refused without an owner-provided identity, or with an IP address as one, or without explicit authorization | PASS |
| Media readiness is never raised by a scan; firmware is never inferred | PASS — `MediaStatus` has no "working" member |
| Identity bindings survive a restart | PASS — a moved camera is still known after reload; malformed files are refused |
| Inconsistent discovery pass (one NIC at two addresses, or one address claimed by two NICs) | PASS — refused, and nothing applied |
| An address is never accepted as identity | PASS — refused with the reason that explains why |

## 9. Required status report

| Item | Status | Basis |
|---|---|---|
| Device identified | **PASS** | three distinct units from fingerprint + port signature + simultaneous presence, **and** the owner-supplied app identifier for the third unit |
| LAN reachable | **PASS** | live, responding, documented ports measured; camera 2's old address confirmed absent |
| Registry onboarding | **PASS** | owner-supplied V380 identifier recorded privately; the record loads through the real parser and binds across an address change |
| Authenticated camera connection | **FAIL** | no standard interface exists; proprietary path forbidden |
| Video | **BLOCKED** | same blocker; no live feed exists and none is claimed |
| Microphone audio | **BLOCKED** | not independently testable; no interface |
| Two-way audio | **BLOCKED** | not independently testable; no interface |
| Remote readiness | **BLOCKED** | needs a media path and an always-on connector host (the registered identity now exists) |
| Camera-specific consent / revocation / offline handling | **PASS** | verified against the real record and the real registry (§3.1) |
| Registry persistence across restart | **PASS** | save/reload round-trip on the real record, malformed files refused |
| Discovery of the third camera | **PASS** | identified, reachable, fingerprinted and registered; still no standard media interface on it |

## 10. Standing constraints, honoured

| Constraint | Status |
|---|---|
| No anonymous ONVIF/RTSP, no insecure feeds | Honoured — nothing enabled; the previously exposed interface stays OFF |
| No firmware intervention, no factory reset, no credential guessing, no proprietary-protocol reverse engineering | Honoured — not attempted |
| No Oracle/mail/Cloudflare/router changes, no port forwarding | Nothing touched |
| Ubuntu Docker not required for production; Mi A3 MCP service not disturbed | Honoured — no deployment at all |
| Private addresses, device IDs and credentials stay out of public GitHub | Honoured **in this document and this milestone's code/tests** (synthetic fingerprints throughout). See the exposure note below |
| Do not claim a working live camera feed until demonstrated on the physical device | No such claim appears anywhere |

## 11. Owner-specific values in this public repository

The repository is public. Earlier milestones' evidence documents carried real LAN addresses, MAC
addresses, the Wi-Fi name and firmware version strings, and this milestone's own first commit briefly
did the same in comments and tests. **All of it has now been redacted** (owner-approved): evidence
documents use placeholders with a privacy notice, and documents, scripts and tests that merely needed
an example address use a neutral range. A scan over every tracked file finds no remaining real MAC, no
private host address, no Wi-Fi name, no firmware version string and no device identifier.

Recorded plainly for the owner: redaction reduces **future** exposure. Values that were already
cloned, fetched or mirrored cannot be recalled by these commits, and git history still contains them.
Removing them from history would require a rewrite and force-push, which was not requested and is not
done here.

The camera-3 Device ID, its per-unit MAC and its LAN address live only in the owner-restricted
evidence file outside every repository, together with the seed file the code consumes.
