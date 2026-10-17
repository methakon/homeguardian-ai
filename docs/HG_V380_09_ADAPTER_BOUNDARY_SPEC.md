# HG-V380-09 — Camera Adapter Boundary Specification

> **Privacy notice:** device-specific values in this document (LAN addresses, MAC
> addresses, Wi-Fi name and firmware/software version strings) are replaced with
> placeholders such as `<redacted-lan-host-19>`. The concrete values are held in the
> owner-restricted evidence file outside this repository. Every measurement, finding and
> conclusion in this document is unchanged.


**Date:** 2026-10-17
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`
**Base:** `release/hackathon-2026-integration` (`ad01600`) — chosen because it carries the bounded device
registry, the registry CLI, the consent gate and the MCP server, none of which the camera workstream
branch had. The V380 discovery/evidence documents and probe scripts are imported unchanged.
**Design inherited from:** `docs/HG_NET_ARCH_01_NETWORK_INDEPENDENT_CAMERA_INTEGRATION.md`

This document is task 1 of HG-V380-09: the exact contract between the components, the identity rule,
and the failure vocabulary. The prototype that exercises it is task 3 and is described in §7.

---

## 0. The one-line contract

> **A camera can only be used through a site connector, the connector can only act with a current
> authorization decision from the registry, and the only thing that leaves the connector is a bounded
> observation — never media.**

Everything below exists to make the three ways that can go wrong *unrepresentable* rather than
merely discouraged: an address being used as identity, a mock being reported as a real camera, and a
refusal being indistinguishable from an absence of activity.

---

## 1. Components and the direction of every edge

```
  ┌────────────────────────── site ──────────────────────────┐
  │                                                          │
  │  ┌────────────────────┐        ┌──────────────────────┐  │
  │  │ ICameraAdapter      │◄──────│ SiteConnector         │ │
  │  │ (camera-SPECIFIC)   │  pull │ (orchestrator)        │ │
  │  │  · mock             │       │  · re-auth per frame  │ │
  │  │  · rtsp/onvif (n/a) │       │  · bounded pull       │ │
  │  │  · vendor (n/a)     │       │  · bounded reconnect  │ │
  │  └────────────────────┘       └───────┬──────────────┘  │
  │                                        │ authorize()      │
  │                                        ▼                  │
  │                            ┌──────────────────────────┐   │
  │                            │ IDeviceAuthorizer        │   │
  │                            │  ↳ RegistryAuthorizer    │   │
  │                            └───────────┬──────────────┘   │
  └────────────────────────────────────────┼─────────────────┘
                                           │ read-only
                                           ▼
                    ┌──────────────────────────────────────────┐
                    │ net::DeviceRegistry (EXISTING)           │
                    │  paired · revoked · credential_id ·      │
                    │  consent · capture_enabled · capabilities│
                    └──────────────────────────────────────────┘

  connector ──Observation──► core::Pipeline ──Event──► correlation/AlertPolicy/persistence
                                                       (EXISTING single event path)

  status surface ──► Alexa-facing controls: MCP tools (read-only status/alerts) and the
                     WebRTC streamer for video. Media NEVER travels over MCP or JSON.
```

| Edge | Direction | Payload | Notes |
|---|---|---|---|
| Connector → adapter | pull only | `StreamRequest`, control commands | the connector never pushes media down |
| Adapter → connector | metadata only | `FrameMeta` (size, keyframe, pts, codec) | **no payload buffer exists in the interface** |
| Connector → authorizer | request | `{LogicalDeviceId, credential_id, Capability}` | one question, one answer |
| Authorizer → registry | read-only | device record lookup + `authenticate()` | no writes; operator actions go through the registry API/CLI |
| Connector → pipeline | one-way | `Observation` → `Event` | the existing pipeline, not a second engine |
| Registry → control plane | operator action | pair / consent / capture / revoke | unchanged by this milestone |

## 2. Identity (stable, logical, never an address)

```cpp
struct LogicalDeviceId {
    std::string site_id;     // "site.home.dhar"
    std::string device_id;   // "cam.v380.ec39.01"
    bool valid() const;
    std::string canonical() const;   // "site.home.dhar/cam.v380.ec39.01"
};
```

Rules, all enforced in code (`src/camera/CameraTypes.h`):

1. **Character set and length are bounded** — lowercase alphanumerics, `.`, `-`, `_`; ≤ 64 chars per
   segment. An identity that cannot be printed safely cannot be logged safely.
2. **An IPv4 literal is REFUSED as an identity segment** — `<redacted-lan-host-19>` as a `device_id` fails
   `valid()`. This is not pedantry: both cameras have already changed address once (camera 2 moved
   from a NAT'ed `<redacted-nat-host-164>` to `<redacted-lan-host-20>`), so an address in an identity field is a latent
   identity bug. The check is a real dotted-quad test, so `cam.v380.ec39.01` and `999.1.1.1` (not a
   valid octet) both pass.
3. **MAC, SSID, subnet, model and serial are ATTRIBUTES**, exposed only through
   `ICameraAdapter::attachment_summary()` for human recognition. They are never an authorization
   input. Camera 1 reports `SerialNumber = 123456789` (a placeholder shared across units) and
   `Model = IPCamera` (generic), so neither could be an identifier even if we wanted it to be.
4. **The observation `source` carries the logical identity** (`camera:<site>/<device>`), never an
   address — asserted by a test.

## 3. `ICameraAdapter` — the camera-specific seam

The only interface that knows what a V380 is. Its full definition is `src/camera/ICameraAdapter.h`;
the contract that matters:

| Member | Contract |
|---|---|
| `id()` | the stable logical id; never derived from a network attribute |
| `adapter_class()` | `mock` / `rtsp_onvif` / `vendor_protocol_unverified` — authoritative, and the input to provenance |
| `attachment_summary()` | informational observation of model/firmware/address; never authorization |
| `declared_capabilities()` | a CLAIM. Not a grant — the connector still authorizes |
| `probe()` | reachability + interface availability, consuming no media |
| `start_stream(StreamRequest)` | bounded; requires `Capability::video` declared AND caller authorization |
| `stop_stream()` | idempotent, never throws |
| `read_frames(FrameMeta*, max)` | **metadata only**; returns how many frames were accounted for |
| `control(ControlCommand)` | closed vocabulary; requires the matching declared capability |
| `last_outcome()` / `frames_accounted()` | the classified result and a bounded activity signal |

Two design choices worth naming:

- **No payload ever crosses this seam.** `read_frames` fills `FrameMeta`, so a mock *cannot* become a
  place where media accumulates, and an adapter *cannot* hand the connector a frame to store.
- **No method may block indefinitely.** An implementation that cannot honour a budget must return a
  classified failure instead of hanging — the real family closes a client silently, so "wait for
  more" is precisely the wrong default.

## 4. `IDeviceAuthorizer` — the single decision point

```cpp
virtual AuthorizationOutcome authorize(const LogicalDeviceId& id,
                                       const std::string& credential_id,
                                       Capability capability) const = 0;
```

`RegistryAuthorizer` is the production implementation over the EXISTING `net::DeviceRegistry`. It
answers by reading, in this order, facts the registry already keeps separately:

| Order | Registry fact | Outcome | Why this order |
|---|---|---|---|
| 1 | record exists | `unknown_device` | identity before permission |
| 2 | `revoked` / `revoked_by_file` | `revoked` | the fact an operator most needs to see; reported even when pairing and consent were also cleared |
| 3 | `paired` | `not_paired` | |
| 4 | `credential_id` binding + `authenticate()` | `credential_mismatch` | an **empty** credential is refused, never treated as "no check needed" |
| 5 | `consent_granted` (+ purpose) | `consent_missing` | |
| 6 | `capture_enabled` | `capture_disabled` | the operator's switch is last and never inferred from consent |

Invariants: **no writes** (it is const against the registry, so pairing/consent/revocation stay the
operator's actions); **fail-closed** (anything unevaluable is a denial); **one question** (camera code
cannot reach unrelated registry state); and the registry's own rule is preserved verbatim —
**network authentication is not permission to collect**.

The `capability` parameter does **not** select the decision today. Permission facts and capability
claims are kept separate on purpose: a claimed capability must never widen permission. The connector
enforces declared capabilities as its own, separate check.

## 5. `SiteConnector` — the orchestrator

| Operation | Sequence | On refusal |
|---|---|---|
| `register_device()` | authorize(video) | `registered=false`, reason recorded, **no observation** |
| `start()` | authorize → declared video capability → `adapter.start_stream()` | denied ⇒ stop + `stopped_by_authorization` + no observation; no declared video ⇒ `capability_absent` + one `unavailable` observation; unsafe interface ⇒ `refused_unsafe_interface` + one `unavailable` observation |
| `pull_once(now)` | **authorize before EACH frame** → read metadata → emit one bounded observation | denial mid-pull ⇒ stop the stream, count `pulls_denied`, and emit **nothing** |
| `reconnect(now)` | bounded attempts → **re-authorize** → probe → start | revoked while offline ⇒ does not resume |
| `stop()` | `adapter.stop_stream()` | never throws |

Three rules this encodes:

1. **Authorization before every frame** — the time-of-check/time-of-use closing point. A revocation, a
   consent withdrawal or the capture switch flipping mid-stream stops the stream, rather than waiting
   for the next session.
2. **A mock can never be reported as real.** `Provenance` is derived from `adapter_class()` with no
   override path: a mock is `synthetic` by construction. This is what stops "the mock suite passed"
   from ever being read as "V380 media works".
3. **An unauthorized device produces no observation at all** — not even a "no activity" one. A camera
   we may not use must never look like a quiet camera.

## 6. Failure vocabulary (why a boolean is not enough)

| `ConnectOutcome` | Operational meaning |
|---|---|
| `ok` | the operation succeeded |
| `unreachable` | no path / no listener (the camera-2-behind-NAT case) |
| `credentials_rejected` | the device refused the credential |
| `stream_absent` | connected, but no media is being offered |
| `capability_absent` | asked for something this device does not declare (camera 2's shape) |
| `transport_closed` | closed mid-operation — **normal for this family**, not a fault |
| `refused_unsafe_interface` | the interface exists and is not safe to use (camera 1's shape) |

Adapters classify their own failures because the device supplies no diagnosis at all: measured, a V380
returns no banner, **zero bytes** on an HTTP or RTSP request, closes an idle connection at ~5.0 s and
an unrecognized payload at ~0.21 s. There is nothing to relay, so the adapter must own the
vocabulary.

The observation statuses map onto the same honesty rule the detection framework already uses:
`detected` (frames accounted for), `not_detected` (streaming, none in this window), **`unavailable`**
(we could not run it) — so "could not run" is never confused with "ran and found nothing".

## 7. The prototype (task 3) and its limits

`src/camera/MockCameraAdapter.{h,cpp}` is a deterministic, scripted stand-in: declared capabilities,
probe/start/control outcomes, frames-per-read, and a scripted transport close that mirrors the real
family's silent drop. It opens **no socket**, speaks **no protocol** and produces **no payload**.

Two scripts encode the real shapes so the boundary is tested against the situations we actually have:

- `no_video_device_script()` — camera 2's shape: reachable, but **no media capability is declared**,
  so a stream request must fail `capability_absent`.
- `unsafe_interface_device_script()` — camera 1's shape: the standard interface exists, so the adapter
  must be able to report `refused_unsafe_interface` and be left stopped.

> **A green mock suite proves the BOUNDARY, not the camera.** It does not show that either V380 can
> deliver a frame, that the vendor protocol authenticates, or that Alexa can play anything. No real
> adapter exists yet, and §4 of `HG_V380_09_FEASIBILITY_AND_ALEXA_READINESS.md` records exactly what
> is still missing per camera.

## 8. Where the Alexa-facing surfaces attach

| Surface | Attachment point | Rule |
|---|---|---|
| MCP tools (read-only status/alerts) | existing `src/mcp/` tools over the existing contract | camera status may be *reported*; **no media over MCP or JSON** — the project's standing rule is zero continuous video frames over either |
| Video viewing | the existing `web_mcp/webrtc_streamer.js` signalling path, later `Alexa.RTCSessionController` | media takes a direct/peer path; the coordination plane carries signalling only |
| Camera discovery/readiness | registry presence + `ConnectorStatus` | a device is "discoverable" only when it is registered, paired and authorized — presence is not permission |

Nothing in this milestone exposes a camera to Alexa, and nothing here enables remote access. The
release-security gate for real-camera remote access remains mandatory.

## 9. Files, tests, and what is deliberately absent

| Artifact | Path |
|---|---|
| Vocabulary, identity rule, outcomes | `src/camera/CameraTypes.h` |
| Camera-specific seam | `src/camera/ICameraAdapter.h` |
| Authorization seam (+ fail-closed default) | `src/camera/IDeviceAuthorizer.h` |
| Registry adapter (read-only) | `src/camera/RegistryAuthorizer.{h,cpp}` |
| Deterministic mock | `src/camera/MockCameraAdapter.{h,cpp}` |
| Orchestrator | `src/camera/SiteConnector.{h,cpp}` |
| Tests (`[camera]`) | `tests/camera/test_camera_boundary.cpp` |

Registered in the **test target only** — no shipped binary references these files, so their footprints
are unchanged.

The suite is tagged `[v380]`, not `[camera]`: **`[camera]` was already in use by 10 pre-existing test
cases** (`test_node_capture.cpp`, `test_simulator_backend.cpp`, `test_e2e_simulator.cpp`), so a
`[camera]` filter would have reported 25 cases while only 15 were new — the kind of count that lets a
failing new test hide behind old ones. `[v380]` matches exactly this milestone's 16 cases and nothing
else.

**Deliberately absent:** no RTSP client, no ONVIF client, no vendor-protocol code, no credential
storage for cameras, no media path, and no change to any existing component. The connector's
`AdapterClass` has a value for `rtsp_onvif` precisely so that a future adapter slots in without this
file changing.
