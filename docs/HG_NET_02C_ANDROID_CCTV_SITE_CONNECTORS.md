# HG-NET-02C — Android CCTV VPN Connectivity and Always-On Site Connectors

**Issue:** #16 (`HG-NET-02C — Hermes 3: connect authorized cameras via Android peers or site connectors`)
**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Builds on:** `HG_NET_ARCH_01` (network-independent design) and `HG_V380_09` (adapter boundary,
logical identity, authorization and synthetic-provenance rules)

---

## 0. Scope, and what is honestly NOT done

This is the Hermes 3 slice of HG-NET-02. It does **not** touch Oracle, WireGuard, mail, DNS, the
Mi A3 VPN peer or MCP ingress — those are issues #14 (Hermes 1), #15 (Hermes 2) and #17
(Antigravity). No camera setting, router, extender or firewall was changed.

Delivered here: the **site-connector pilot** — its reach isolation, its control/media split, and its
session-termination semantics — implemented and tested, plus the onboarding matrix and the unmet
prerequisites stated plainly.

**Not done, and not claimed:** no site connector is deployed anywhere; no VPN is involved in any test
here; **no real camera media has been transported by anything in this milestone**; and camera 2 still
has no demonstrated sensor output of any kind.

## 1. Device classification (the distinction Issue #16 asks for)

| Device | Can it be a VPN peer? | How HomeGuardian reaches it | Evidence |
|---|---|---|---|
| **V380 camera 1** (`cam.v380.ec39.01`) | **No.** Nothing has shown these embedded units can run WireGuard | Site connector inside its LAN. Its ONVIF/RTSP stays **OFF** (anonymous media, measured) | `HG_V380_04`, `HG_V380_06` |
| **V380 camera 2** (`cam.v380.ec39.02`) | **No**, same reason | Site connector inside its LAN. No standard media/auth interface exists, so there is nothing for a connector to consume today | `HG_V380_08`, `HG_V380_09` |
| **Android CCTV phone** (Panasonic ELUGA I7, `192.168.1.16`) | **Possibly** — non-root, via Android `VpnService`, and only with the on-device VPN consent prompt approved by the owner | As a peer; its camera/microphone acquisition already runs through the existing consent gates, and it uploads no media by default | `HG_V380_09` device notes; issue #15 governs the Mi A3 equivalent |
| **Mi A3** (production host) | Yes — but that is issue #15's deliverable, not this one | As the HomeGuardian host, and as a *candidate* connector host if it is co-located with a camera | issue #15 |
| **Development workstation / Ubuntu Docker** | Test peer only | Optional; **not** required for 24/7 operation | owner architecture statement |

The connector is the load-bearing piece: it lives **inside** the camera's LAN, dials **out**, holds the
credential the camera cannot hold, and is the only client the camera ever sees.

## 2. Onboarding matrix (per camera and per site)

Legend: **IDENTITY** = the durable logical id · **REACH** = the configured allowlist that bounds it ·
**CONTROL** = what travels over the coordination hub · **MEDIA** = how video would travel ·
**PREREQ** = what must exist before this can be onboarded.

| Site / device | IDENTITY (logical) | REACH (allowlist) | CONTROL path | MEDIA path | PREREQ status |
|---|---|---|---|---|---|
| **Site `site.home.dev`** (where both cameras actually are) | connector `site.home.dev/connector.edge.01`; cameras `…/cam.v380.ec39.01`, `…/cam.v380.ec39.02` | `192.168.1.0/24` (camera 2 also moved through `192.168.0.0/24`) | Site connector → hub: register, heartbeat, authorization, consent/revocation, stream control, bounded telemetry | Direct: `direct_same_site` (the usual case today) or `direct_ice`; `site_uplink` if the viewer is remote. **Never the hub** | **UNMET — no always-on host exists inside this LAN.** The workstation is not always-on, so nothing currently hosts the connector |
| **Remote site (any future camera)** | `site.<name>/connector.<id>` | that site's own subnets only | same | same | **UNMET by definition** — no remote site, no host, no camera |
| **Android CCTV phone** (potential peer) | a device peer identity, never `192.168.1.16` | n/a (it is a peer, not a connector) | via the overlay once #14/#15 land | n/a for CCTV; its own camera is a consented local sensor | **UNMET — needs on-device VPN consent (owner action) and an Android `VpnService` client (issue #15's workstream)** |
| **Development workstation** | dev tester only | n/a | n/a | n/a | available, but must never be the 24/7 answer |

**Hardware prerequisite, stated once and plainly:** every camera site needs one **always-on host**
(so it can be present when the camera is) with a second interface or membership in the camera's
subnet. For the home site the realistic candidates are a small always-on box wired into the camera's
LAN, or — if the Mi A3 is permanently co-located with the cameras — the Mi A3 itself as a connector
host alongside its #15 role. Neither exists today. Until one does, this pilot stays a tested design
with no deployment, and the honest readiness claim is "prepared, not deployed".

## 3. Control-plane test results (measured, in-process)

All of the following are assertions in `tests/camera/test_site_connectors.cpp` (tag `[sites]`), run
against the real `net::DeviceRegistry`:

| Property | Result |
|---|---|
| **Isolation** — a connector bound to site A cannot reach a camera observed in site B's subnets | **PASS.** `scope_check()` → `out_of_site_scope`; `register_device()` refused; `start()` refused; **zero frames accounted**; and the device is simultaneously *reachable* and *authorized in the registry* — neither of which is permission to collect from another site |
| **Unevaluable reach is not permission** | **PASS.** With a profile bound and no observed address, the connector refuses instead of assuming |
| **Reach failure changes nothing about the device record** | **PASS.** Paired, consented and capture-enabled all still hold after a scope refusal |
| **Identity is not an address** | **PASS.** The same logical device moves from `192.168.1.20` to `192.168.0.164`, keeps its identity, and keeps its authorization; a move *outside* the allowlist is refused without altering identity, consent or the registry record |
| **Control vocabulary has no media member** | **PASS.** Every kind name is asserted free of `video`/`audio`/`frame`/`media`; a 32 KiB payload is **refused, not truncated**; a control message with an over-long detail is refused; malformed wire forms, unknown kinds and invalid identities are refused |
| **Telemetry is bounded, not per-frame** | **PASS.** With `telemetry_every_n_pulls = 2`, six pulls produce exactly three telemetry messages, and no message on the wire is frame-sized |
| **Control-only on the wire** | **PASS.** Every captured message is a well-formed control message within the ceiling |
| **Revocation vs consent withdrawal are reported distinctly** | **PASS.** Consent withdrawal ends the session with a `consent_change` kind and **no** `revocation` kind; a real revocation ends it with `revocation`; both leave the session inactive and emit **no** observation |
| **Session termination is idempotent and explicit** | **PASS.** `terminate_session()` stops the adapter, clears `session_active`, records the reason, and a second call adds nothing |
| **The control hub is never a media path** | **PASS across the whole input space.** Every combination of NAT classes × same-subnet × media-capability × direct-failure × uplink-usability, with relay both pre-approved and not, asserts `uses_control_hub == false` |

## 4. Direct-media limitations (what the planner can and cannot promise)

| Path | When chosen | Honest limitation |
|---|---|---|
| `direct_same_site` | both peers on the same subnet | works today for local viewing; says nothing about remote viewers |
| `direct_ice` | neither peer looks like symmetric NAT/CGNAT and no attempt has failed | **the ICE/DTLS-SRTP transport itself is not implemented in this milestone** — the planner decides *what* should happen, the peer-side transport is issue #15/#17 territory |
| `site_uplink` | a peer-to-peer path is implausible, or already failed | the bytes are the **site's** bandwidth, which is the right place for them, and it works behind CGNAT because the connector dials out — but it means the site host carries the stream for every viewer |
| `relay_required` | no direct path **and** the site uplink is unusable | a **separate capacity/permission decision**; reported with `needs_owner_approval`, never taken automatically, and never satisfied by the Oracle hub |
| `unavailable` | the connector declares no media capability | camera 2's present situation: there is nothing to transport |

**Ceilings and ratios (in code, not prose):** the control channel refuses anything above 1 KiB, a
single 1080p H.265 keyframe is referenced at 32 KiB, and the planner asserts the media-to-control
ratio is at least 32×. That is the arithmetic behind "Oracle must not carry continuous CCTV video":
a media path over this channel is not expressible.

**What has NOT been demonstrated:** no WebRTC ICE/DTLS-SRTP session, no TURN/relay path, no media
transport of any kind with a real camera, and no cross-ISP measurement. Those need a real camera
interface (not available: camera 1's is unauthenticated and disabled; camera 2 has none) and the
peer-side transport from the other workstreams.

## 5. Readiness claims — evidence-backed only

| Claim | Status | Evidence |
|---|---|---|
| The site-connector boundary exists and enforces reach isolation, control-only messaging, session termination and synthetic provenance | **VERIFIED (in-process)** | `[sites]` + `[v380]` suites; real `DeviceRegistry`; mocks only |
| A connector can be deployed at a camera site | **NOT DEPLOYED** | no always-on host exists at either site (§2) |
| Android devices can reach cameras over a VPN | **NOT VERIFIED HERE** | the Android VPN client and the overlay are issues #15/#14; this milestone neither provisioned nor ran one |
| Any V380 camera can deliver authenticated media | **NOT VERIFIED, and currently impossible** | camera 1: anonymous-only interface, disabled; camera 2: no standard interface (§1) |
| Camera 2 is a working sensor-only node | **FALSE — and must not be written anywhere** | no independent sensor output has been demonstrated from it |
| Mocks could be mistaken for real feeds | **PREVENTED BY CONSTRUCTION** | provenance is derived from `adapter_class()` with no override; asserted in both suites |
| Video never travels over the coordination hub | **VERIFIED at the boundary** | closed vocabulary + hard ceiling + the exhaustive planner invariant (§3, §4) |

## 6. Dependencies this hands to the other workstreams

| Consumer | What it needs from here |
|---|---|
| **#14 (Oracle hub, Hermes 1)** | The hub may carry exactly the kinds in `ControlChannel`: register, heartbeat, authorization, consent, revocation, ICE candidate, stream control, session terminated, telemetry. The client side now refuses anything else, including frame-sized payloads — so "measure VPS bytes to prove no media" has a client-side counterpart that makes media an encoding failure, not a bandwidth surprise |
| **#15 (Mi A3, Hermes 2)** | If the Mi A3 is permanently co-located with the cameras it becomes the natural connector host; otherwise a separate always-on host is required per site. Its peer identity must be a device identity, never `192.168.1.x` |
| **#17 (MCP ingress, Antigravity)** | Camera *control* over the hub is separate from MCP application authorization. VPN peer authentication does not authenticate an MCP caller, and nothing here weakens that |

## 7. Standing constraints, honoured

| Constraint | Status |
|---|---|
| Oracle must not carry continuous CCTV video | Enforced at the boundary (closed vocabulary + hard ceiling + planner invariant); no Oracle change made |
| No insecure ONVIF, no proprietary auth bypass, no credential guessing | Honoured — camera 1's ONVIF remains **OFF**; the vendor protocol was not touched and is escalated, not attempted |
| No public MCP endpoint / unrestricted listening | Nothing added; no listener introduced by this milestone |
| No changes to the other VPS, mail, mail certificates, Cloudflare, shared Oracle lists | Nothing touched |
| Independent durable device identities; never a dev LAN address as identity | `LogicalDeviceId` refuses an IPv4 literal as a segment; connectors have their own ids; addresses live only in `ObservedEndpoint` as attachments |
| One production C++/MCP runtime on Mi A3; Ubuntu Docker for development | Nothing here creates a second runtime; the connector is a component, not a service |
| Mocks never displayed as real | Structural, and asserted |
