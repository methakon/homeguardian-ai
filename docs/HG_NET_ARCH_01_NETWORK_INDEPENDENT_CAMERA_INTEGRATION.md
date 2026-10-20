# HG-NET-ARCH-01 — Network-Independent Camera Integration

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-ec39-integration`
**Reference commit:** `a3a134e27e9eb06ddbe544c849653a7cd896b062`
**Scope:** architecture design + the minimum temporary connectivity to finish camera-2 capability testing.
**Not authorized, not done:** no Oracle deployment, no VPN provisioning, no router or extender change,
no public port forwarding, no proprietary-protocol bypass.

---

## 0. ANSWER FIRST

The correction is accepted and it changes the design, not the measurements:

- **The Tenda's NAT is no longer a problem to solve — it is the normal case.** A site connector that
  sits *inside* the camera's own segment and **dials out** never needs inbound access, so camera 2
  behind a NAT'ing extender on a different subnet works exactly as well as a camera on the same LAN.
  **Bridge mode is not required, and no permanent dependency on `<redacted-lan-subnet>.x`, `<redacted-nat-subnet>.x`, the
  Tenda, or a cable to the development workstation is part of this design.**
- **Identity is logical, not locational.** A camera is `{site_id, device_id}` bound to a keypair;
  IP address, MAC, SSID and subnet are *attributes that may change at any moment*, never identity.
- **Oracle carries coordination only.** Registration, authorization, pairing and endpoint rendezvous
  are small control-plane messages. **Media never traverses Oracle** — a VPN hub that concentrates
  all media on the VPS is explicitly rejected for that reason.
- **Media is direct** (DTLS-SRTP via WebRTC, or mutual-TLS TCP where both peers are reachable), with
  a relay only in the documented symmetric-NAT/CGNAT cases and only with explicit approval.
- **The site connector is the piece that makes it work for a dumb camera**: it holds the credential
  the camera cannot hold, speaks whatever the camera speaks locally, and presents the camera to the
  control plane as a first-class registered device.

And the immediate development task, re-measured at 20:14 IST: **the workstation's connection is
unchanged** (Airtel, `<redacted-lan-host-6>/24`, via `<redacted-lan-host-1>`; `enp5s0` still `NO-CARRIER`) and camera 2
is still unreachable. That is a *temporary development-convenience* problem only — §11 gives the
smallest thing that unblocks capability testing, and §12 records what is still needed from you.

---

## 1. What is grounded in code that already exists

The repository already contains most of the pieces; this design reuses them rather than inventing
parallel machinery. (Paths noted where they live today, because this branch is the camera
workstream and does not carry them.)

| Component | Where | Role in this design |
|---|---|---|
| `DeviceRegistry` (`src/net/DeviceRegistry.h`) | `feature/secure-device-transport` and every H-series branch | **The durable identity + authorization store.** Already carries `device_id`, `display_name`, `device_type`, `credential_id` (cert fingerprint/CN), `paired`, `revoked`, `revoked_by_file`, `revocation_note`, `capabilities`, `has_camera`, `has_microphone`, `last_heartbeat_ms`, `last_sequence`, `last_observation_at_ms`, `online`, device-scoped `consent_granted`/`consent_subject`/`consent_purpose`, and the operator `capture_enabled` switch. Its own header states the invariant this design must preserve: **"NETWORK AUTHENTICATION IS NOT PERMISSION TO COLLECT."** |
| `DeviceIngressServer` / `DeviceProtocol` (`src/net/`) | same | The mutual-TLS ingestion boundary: identity from the certificate CN, authorization re-read on a deadline, sequence watermark, two-phase replay-safe record, idempotent unacknowledged retry. **This is the control plane's transport, and it already dials in from the device side.** |
| `IngressConfigPolicy` (`src/net/`) | same | Replay protection is mandatory and fails closed; configurable policy is TLS-free so it is unit-testable. |
| `tools/provision_ca.sh` | all H-series branches | Private CA: `init` / `issue <id>` / `show <id>` / `check-runtime`, with a signing key that lives in no container and no image. **This is where a durable device identity is minted.** |
| `deploy/docker/` (Dockerfile, compose, `ingress-entrypoint.sh`, health checks) | same | The deployable hub/ingress targets, and the **Alexa-compatible Docker target** the design must keep. |
| `web_mcp/` (`server.js`, `auth.js`, `device_adapter.js`, `mcp_adapter.js`, `webrtc_streamer.js`, `public/index.html`) | `experiment/mi-a3-edge*` | The authenticated viewing surface and **the existing WebRTC streaming path** the media plane should extend. |
| `IMediaDevice`, `ConsentGuardedDevice`, `ConsentGate`, `Observation`, `Pipeline` (`src/core/`) | this branch | The acquisition contract, per-payload authorization and the single event path the connector must feed. |

## 2. Durable logical identity (requirement 1)

**Rule: identity is an identifier we assign plus a key the device holds. Everything about *where* it
is attached is an attribute.**

| Layer | Value | Mutability | Used for authorization? |
|---|---|---|---|
| `site_id` | e.g. `site.home.dhar`, `site.office` | stable, operator-assigned | yes — scoping |
| `device_id` | logical id, e.g. `cam.v380.ec39.01` | stable | yes |
| `credential_id` | X.509 fingerprint / CN issued by `provision_ca.sh` | rotates only by re-issuing | **yes — this is the authentication input** |
| `attachment` | connector-reported: model string, firmware string, LAN MAC, vendor UID | informational, connector-observed | **no** |
| `endpoint` | last seen IP:port, SSID, subnet, gateway | **changes freely; never authoritative** | **no** |
| `capabilities` | declared by the connector, bounded per device | changes on reconnect | yes — as an allowlist, not a grant |

Consequences that must be enforced:

- **No authorization decision may read an address.** A device that authenticates from a new IP is
  the same device; an attacker on the old IP is not the device. (The registry already does this: it
  keys on `credential_id`.)
- **The camera cannot hold a key, so the connector holds it.** The camera's identity is represented
  by the connector's credential plus an operator-assigned `device_id`; the camera's own serial/model
  are recorded as `attachment` only. This matters concretely here: camera 1 reports
  `SerialNumber = 123456789` (a placeholder shared across units) and `Model = IPCamera` (generic), so
  neither is fit to be an identifier — see `HG_V380_06` §6.
- **Time is not identity either.** The same camera measured **9,001.5 s behind true UTC**; identity
  and ordering come from the control plane's sequence watermark, never from device time.

## 3. Multi-site topology (requirement 4, 5, 8)

```
        ┌──────────────────────────────────────────────────────────────┐
        │ COORDINATION PLANE  (Oracle VPS — access/coordination only)  │
        │  · device registry + authorization (pair / consent / revoke) │
        │  · pairing + credential registration                         │
        │  · rendezvous / signalling for direct media                  │
        │  · presence + health                                          │
        │  NO media, NO transcoding, NO storage, NO relay by default    │
        └───────▲──────────────────────▲───────────────────▲──────────┘
                │ outbound mutual TLS   │ outbound          │ outbound
                │ (small control msgs)  │ (signalling)      │ (viewer auth)
     ┌──────────┴─────────┐  ┌─────────┴────────┐  ┌───────┴──────────────┐
     │ SITE A              │  │ SITE B            │  │ VIEWERS               │
     │ ┌────────────────┐  │  │ ┌──────────────┐  │  │ · web dashboard       │
     │ │ site connector │  │  │ │site connector│  │  │ · Alexa-compatible    │
     │ │ (owns the key) │  │  │ └──────┬───────┘  │  │   Docker target       │
     │ └───┬────────┬───┘  │  │        │          │  │ · mobile               │
     │     │ local  │      │  │  camera(s)        │  └───────────────────────┘
     │  camera(s)   │      │  │  (any subnet,     │
     │  (any subnet)│      │  │   any ISP, NAT)   │
     └──────────────┼──────┘  └───────────────────┘
                    │
        DIRECT ENCRYPTED MEDIA (DTLS-SRTP / mTLS) — never through Oracle
        site connector ⇄ viewer, or connector ⇄ analysis node
```

Why the connector is *inside* the site: it makes every camera reachable regardless of ISP, subnet,
NAT or CGNAT, because **every connection it makes is outbound**. Airtel today, Jio tomorrow, a phone
hotspot next week — only the `endpoint` attribute changes.

### Deployment independence (requirement 8)

Capture, analytics and viewing are **three separately deployable roles**, and none of them may
hardcode a location:

| Role | Runs where | Constraint |
|---|---|---|
| Capture / site connector | smallest host at the site (the extender's segment, a Pi, the existing phone, whatever exists) | must reach the cameras locally; needs no inbound port |
| Analytics (detection, fusion, alerting) | **anywhere** — a site host, the Mi A3, or the hub container | consumes observations through the existing single pipeline; the Mi A3 is a *current deployment choice*, not a permanent location |
| Viewing (web / Alexa-compatible client) | anywhere with an authenticated path | receives media directly or via the connector; never through Oracle |

The Alexa-compatible Docker target is preserved as a viewing/control surface, and nothing in this
design requires the C++ analysis to live on a particular device or subnet.

## 4. Proposed connector interfaces (requirement 3)

Three seams, each independently testable. `ICameraAdapter` already exists in spirit as the
`IMediaDevice` + `V380Transport` plan (`HG_V380_03`); the other two are new.

```cpp
// 1. SITE-LOCAL CAMERA ADAPTER  (the only place that knows what a V380 is)
class ICameraAdapter {
public:
    virtual ~ICameraAdapter() = default;
    // Durable, operator-assigned. NOT derived from the camera's own serial/model.
    virtual std::string device_id() const = 0;
    // Connector-observed, informational, never an authorization input.
    virtual Attachment  attachment() const = 0;
    virtual Capabilities capabilities() const = 0;   // bounded, declared, may shrink on reconnect

    // Control-plane operations. Each one is consent-gated and returns a classified failure.
    virtual Result probe(std::chrono::milliseconds budget) = 0;      // reachability + interface state
    virtual Result start_stream(StreamRequest) = 0;                   // bounded, no recording
    virtual Result stop_stream(StreamId) = 0;
    virtual Result ptz(Command) = 0;                                  // only if capability declares it

    // Media never leaves through this interface: it yields an authenticated, bounded
    // frame/sample source that the connector exposes to authorized peers ONLY.
    virtual FrameSource* media(StreamId) = 0;
};

// 2. SITE CONNECTOR  (owns the key; the camera's representative to the control plane)
class ISiteConnector {
public:
    virtual ~ISiteConnector() = default;
    virtual Result register_with_coordination(const SiteConfig&) = 0;  // OUTBOUND, mutual TLS
    virtual Result heartbeat(DeviceStateReport) = 0;                    // presence + capabilities
    virtual Result reconcile(const AuthorizationSnapshot&) = 0;         // server is authoritative
    virtual Result announce_endpoint(CandidateEndpoint) = 0;            // for rendezvous/signalling
    virtual Result on_control(ControlRequest) = 0;                      // start/stop/ptz, consent-gated
};

// 3. MEDIA PLANE  (separate from the control plane on purpose)
class IMediaPath {
public:
    virtual ~IMediaPath() = default;
    virtual Result negotiate(PeerDescriptor) = 0;   // signalling only; ICE/DTLS-SRTP or mTLS TCP
    virtual bool   is_direct() const = 0;           // false only for an APPROVED relay
    virtual Result start() = 0;                      // begins flow; caller must have authorization
    virtual void   stop() noexcept = 0;              // idempotent
};
```

Interface rules these encode:

1. **Authorization is pulled from the control plane, never inferred locally.** `reconcile()` takes the
   server's snapshot; a connector's own opinion is not an input to a permission decision.
2. **Every control operation is consent-gated and classified.** Failures are named
   (`unauthorized`, `consent_missing`, `camera_unreachable`, `capability_absent`, `rate_limited`) —
   the connector never relays a device's own words as a diagnosis.
3. **`capabilities()` is a declaration, not a grant.** A device that declares `has_camera` still
   needs `paired && consent_granted && capture_enabled` before a frame moves.
4. **`is_direct()` is observable**, so a deployment can alert when it silently falls back to a relay.

## 5. Control plane vs media plane (requirement 4, 5, 6)

| Aspect | Control plane | Media plane |
|---|---|---|
| Carries | registry, pairing, authorization, consent, revocation, capabilities, presence, sequence watermarks, signalling candidates | video/audio frames |
| Path | device → coordination (outbound mutual TLS) | peer → peer, direct |
| Transport | reuse `DeviceProtocol` + `DeviceIngressServer` | DTLS-SRTP (WebRTC) or mutual-TLS TCP |
| Through Oracle? | **yes** — small, bounded, low-volume | **never** |
| Size per event | bytes to a few kB | continuous, high volume |
| Failure mode | reconnect with backoff, resume above `last_sequence` | renegotiate, or fall back to an approved relay |

**Why a VPN hub is rejected:** routing media through an Oracle-hosted VPN hub would put every frame
on the VPS's uplink and CPU, which is exactly the bandwidth and role problem the requirement names.
VPN-style connectivity remains useful for *management* access; it is not the media path.

## 6. Reconnection and identity under network change (requirement 2)

The behaviour to implement is the same as the Android node already needs:

1. **Everything is outbound.** The connector dials the coordination endpoint; it never listens for
   inbound connections. CGNAT, double NAT and a changing public IP are therefore all irrelevant.
2. **Exponential backoff with jitter**, bounded attempts, and a status the operator can read
   (`connecting`, `backoff`, `registered`, `degraded`). A transport closure with zero bytes is a
   normal event in this protocol family (measured on camera 1: idle close at ~5.0 s, silent close in
   ~0.2 s on an unrecognized payload) and must not be reported as a camera fault.
3. **Resume, don't reset.** The welcome carries the server's `last_sequence`; a reconnecting connector
   resumes above it. Replay is still refused, and a legitimate reconnect still works.
4. **Endpoint change is an attribute update, not a new device.** `announce_endpoint()` refreshes the
   rendezvous hint; the `endpoint` field carries no authority, so a moved camera keeps its consent,
   its capabilities and its history. **Revocation must survive an address change** — deleting a
   device's trust is a control-plane act, never something a reconnect can undo.
5. **Identity is presented on every reconnect** by certificate, so a captured `device_id` string alone
   is useless to an attacker (`identity_mismatch` is already the measured behaviour when a certificate
   claims a different id).

## 7. Direct media vs an approved relay (requirement 6)

Decision order, most preferred first:

1. **Direct, same site** — connector and viewer/analysis on the same segment: mutual-TLS TCP RTSP-like
   flow, or WebRTC inside the site. No NAT involvement at all.
2. **Direct, cross-site** — WebRTC with ICE; signalling goes through the coordination plane (small),
   media goes peer-to-peer. Works for the common cone-NAT cases.
3. **Site connector as the media origin** — the connector pulls from the camera and serves authorized
   peers over its **own** uplink. This is not an Oracle relay: the bandwidth is the site's, which is
   the correct place for it. **Authenticated only, bound to the site interface, never anonymous** —
   the exact defect measured on camera 1 when ONVIF was enabled.
4. **Explicitly approved relay (last resort)** — only for symmetric NAT / hard CGNAT where no direct
   path exists, and only if the owner approves it per case. Requirements if used:
   - per-device opt-in in the registry (a device is never silently relayed),
   - the stream is end-to-end encrypted to the authorized viewer; the relay cannot decrypt,
   - no storage, no transcoding, no recording, bounded session lifetime,
   - `is_direct() == false` is surfaced in the UI and in logs so the fallback is visible,
   - documented per deployment with the reason it was needed.

## 8. Consent, authorization, revocation and isolation (requirement 7)

Every field below already exists in `DeviceRegistry`; the design adds no new trust path:

| Control | Field | Failure direction |
|---|---|---|
| Pairing | `paired` | unknown device → not paired → refused |
| Device revocation | `revoked`, `revoked_by_file`, `revocation_note` | refuses on the record; removing an id *lifts* the ban without restoring pairing or consent |
| Device-scoped consent | `consent_granted`, `consent_subject`, `consent_purpose` | absent/withdrawn/expired → refuse (fail closed) |
| Operator switch | `capture_enabled` | absent key is false; never inferred |
| Capability allowlist | `capabilities`, `has_camera`, `has_microphone` | declared capability ≠ permission to collect |
| Liveness | `last_heartbeat_ms`, `online` | stale device is offline, not trusted-by-default |
| Replay | `last_sequence`, two-phase record | a repeat of the last completed sequence is idempotent; a true replay is refused |

Plus the media-plane rules that follow from `HG_V380_04`: per-payload authorization before every
frame, and **no anonymous media path may exist on any deployment**, whether or not the camera itself
enforces anything.

**Network isolation is a compensating control, not a cure** — recorded as a standing rule:

> Putting a camera on an isolated segment bounds *who can reach* it. It does not make an
> unauthenticated camera protocol secure, it does not make the connector trustworthy, and it is not
> evidence that a device whose interface served anonymous media has become safe. Isolation is
> defence in depth and it must never be presented as a substitute for authentication — and it must not
> become a permanent prerequisite for a deployment either (§3).

## 9. What this design removes from the old plan

| Dropped as permanent | Replaced by |
|---|---|
| Tenda in bridge/AP mode | **nothing** — a NAT'ing extender is a supported, normal topology for a site connector |
| `<redacted-lan-host-0>/24` / `<redacted-nat-host-0>/24` assumptions | logical `site_id` + outbound registration |
| An Ethernet cable to the development workstation | a site connector on the site's own segment (the cable stays a *development* convenience only, §11) |
| Oracle as a media path | Oracle as coordination only, media direct or site-originated |
| Mi A3 as the fixed analysis location | capture/analytics/viewing as independently deployable roles |

## 10. Multi-site onboarding sequence (what a new site does)

1. Operator creates the site and its devices in the **registry** (`site_id`, `device_id`), and issues
   one credential per connector with `tools/provision_ca.sh issue <id>`.
2. The connector is deployed on the site's own network with credentials in place; it **dials out**,
   registers, and reports `attachment`, `capabilities` and a candidate `endpoint`.
3. Devices start **unpaired and unconsented**: registering is not permission. The operator pairs,
   grants consent for an explicit purpose, and enables capture.
4. Viewers connect and are authenticated; media takes a direct path (or the site connector's own
   uplink). The coordination plane records presence and authorization — it never touches a frame.
5. Revocation is one control-plane act and takes effect on the next authorization check.

## 11. Minimum temporary connectivity to finish camera-2 capability testing

This is a **development-only bridge**, explicitly temporary, and **removable without touching the
design in §3–§10**:

- **Smallest step:** one Ethernet cable from a LAN port of the Tenda (or any port on the camera's own
  segment) into this workstation's currently idle `enp5s0` (`<redacted-mac-h>`, `NO-CARRIER`). No
  router, extender or firewall setting changes; Wi-Fi keeps the Airtel address and default route, and
  routing is unambiguous (directly connected route for the camera segment, everything else via
  Airtel). Confirmed by `ip route get <camera-ip>` naming `enp5s0`.
- **Alternative:** associate the workstation's Wi-Fi to `kusankurdhar` (needs the owner to enter that
  Wi-Fi password locally — it must not be pasted into chat or a repo file). This loses the Airtel LAN
  for the duration.
- **Either way**, the *only* thing it buys is a path to camera 2 so capability testing can finish. The
  final system needs neither, and camera 2's NAT is handled by §3.

Testing order once there is a path (unchanged from `HG_V380_06`): access-control check **first**; if
any interface is EXPOSED, stop media work and report; then RTSP/ONVIF inventory; then capability
(`<redacted-cam2-software-family>` family — do not carry over anything measured on camera 1's `<redacted-sw-token>`); no decryption,
no bypass, no credential guessing.

**Current state, re-measured 20:14 IST:** workstation still on Airtel (`<redacted-lan-host-6>/24` via
`<redacted-lan-host-1>`), `enp5s0` still `NO-CARRIER`, camera 2 still unreachable, and the first camera's ONVIF
remains **DISABLED** (security restrictions intact, unchanged by this milestone).

## 12. What is needed from the owner

1. **The address the V380 Pro app currently shows for camera 2** (the app's device page carries it) —
   the fastest way to confirm whether it is still `<redacted-nat-host-164>` or has moved.
2. **One cable into `enp5s0`** (or the Wi-Fi alternative above) to create a path for capability
   testing.
3. Confirmation of whether camera 2's Advanced settings still shows **no ONVIF row** (absent vs
   disabled is decidable only there).

## 13. Status

| Deliverable | Status |
|---|---|
| Multi-site topology design | **PRODUCED** (§3) — coordination plane on Oracle with no media, sites represented by connectors that are fully outbound |
| Secure connector interfaces | **PROPOSED** (§4) — `ICameraAdapter` / `ISiteConnector` / `IMediaPath`, each independently testable, authorization pulled from the control plane |
| Direct-media vs control-plane separation | **SPECIFIED** (§5, §7) — with a documented, opt-in, approval-required relay case and an observable `is_direct()` |
| Minimum temporary connectivity | **SPECIFIED** (§11) — one cable (or the Wi-Fi join), dev-only, removable, not a design dependency |
| Permanent dependency on Tenda/subnet/cable | **REMOVED** (§9) |
| Oracle / VPN / router / port-forward / protocol bypass | **NOT DONE** — none authorized, none performed |
| V380 ONVIF security restrictions | **INTACT** — camera 1 re-verified DISABLED; nothing enabled anywhere |
| Camera 2 capability testing | **STILL BLOCKED on reachability** — the fix is §11, and it is a dev convenience, not an architecture requirement |
