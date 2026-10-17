# HG-CAM-09 — Media integration contract, test matrix and remaining work

Design assessment for delivering Panasonic camera and microphone data into HomeGuardian's
authorized C++ processing and viewing environment. **This is a contract and a gap analysis, not an
implementation.** Nothing here is deployed; no public endpoint is created.

Companion evidence: `docs/HG_CAM_08D_EVIDENCE_AND_REPRODUCTION.md`.

---

## 1. Where the two sides actually are

**Android side (this milestone, verified on hardware).** A foreground service owns an unbounded
Camera2 → MediaCodec H.264 pipeline and a separately-armed `AudioRecord` PCM path, with real
counters proving continuity across a 30-minute screen-off window. `MediaSink` is a **loopback-only,
token-gated** record emitter. That is a *proof the phone can produce continuous A/V*, not a
transport.

**HomeGuardian side (pre-existing, per this repo's roadmap).**

| piece | state | relevance |
|---|---|---|
| Device protocol + per-device certificates, private CA, identity from the certificate (X-01) | DONE | the identity model a media transport must reuse |
| Mutual TLS transport, TLS 1.2 min, client-cert verification, hard frame cap (X-02) | DONE | `src/net/TlsTransport.*` |
| Android transport in the APK (X-09) | PARTIAL — compile-verified, no phone attached | the seam this milestone finally makes testable |
| Multi-node media: per-device media/consent, independent streams, device-scoped source ids, clock skew, bounded buffering, reconnect isolation, revocation (D-04) | **TODO** | most of the remaining work below lands here |
| Alexa surface (`AlexaContract`, `hg_alexa_bridge`, `alexa_skill`) | aggregate-only, simulated | explicitly NOT a camera device integration — see §6 |

The gap is therefore **concentrated**: the phone can produce the media and HomeGuardian already
owns the identity and transport primitives. What does not exist is a media-specific session on top
of that transport.

---

## 2. Integration contract (proposed, minimal)

Each clause preserves a property the brief requires. Names are indicative; the transport is the
existing mutual-TLS one, not a new one.

### 2.1 Device identity — reuse X-01, never invent a second one

- The node's identity is the **device certificate** issued by HomeGuardian's private CA. Identity is
  derived from the authenticated peer, never from a payload field.
- Consequences that must hold: a stolen/duplicated payload cannot assert another device's identity;
  a revoked certificate terminates that node's media without touching others.
- The current `MediaSink` loopback token is a **local development gate and must not be promoted to
  a network credential.** It is not a substitute for the certificate identity and it does not
  satisfy this clause.

### 2.2 Authorization — per stream, per session, default deny

- Consent is recorded per **sensor** (camera, microphone) and per **owner action**, matching the
  existing independent-arming behaviour.
- A media session must be established with an explicit grant; there is no "stream if connected"
  default. A withdrawn consent closes the session and releases the sensor, exactly as the local
  disarm path does today.
- Precedent to follow rather than re-litigate: `VoicePhrasebook::decide_camera` already refuses to
  announce a camera unless a real authenticated device is present. The same posture applies to
  delivering frames.

### 2.3 Transport — the existing mutual TLS, with an explicit media channel

- Reuse `TlsTransport` semantics: TLS 1.2 minimum, client-certificate verification against the
  private CA, frame length validated **before** allocation, socket timeouts.
- A media channel is a distinct stream on that session with its own framing, so that observation
  traffic is never blocked by media and vice versa. Bounded buffering and a hard cap are mandatory:
  a slow consumer must cause frame drop, never unbounded queue growth on a 952 MB-RAM-class host.
- **No default public media endpoint.** The node dials out; nothing listens publicly, no port
  forward, no relay. This mirrors how the existing device client already behaves.

### 2.4 Reconnection — automatic, idempotent, non-fatal

- After a network change the node re-resolves, reconnects and **re-establishes consent-gated
  sessions without operator intervention**, and without spawning duplicate sessions.
- One node's failure must not affect another (D-04's "losing one device does not affect another").
- The phone side already survives transport loss without stopping capture; the transport must
  tolerate a media session ending mid-stream and resuming without restarting the camera.

### 2.5 Screen-off operation

- Media sessions are owned by the **foreground service**, never the activity. Already true and
  measured; the contract simply forbids regressing it.
- No wake-lock assumption: the SoC does not suspend while capturing, and nothing in the design may
  depend on suspend.

### 2.6 Explicitly out of scope for this contract

Remote dashboard video rendering, public streaming, Oracle/VPS/Cloudflare routing, V380 devices,
talkback (covered separately below), and any Alexa device integration.

---

## 3. Test matrix

Each row is written so it can FAIL. "Verified" means measured on hardware this milestone.

| # | property | method | status |
|---|---|---|---|
| M-1 | Camera produces continuous encoded frames, screen off | 30-min window, per-interval counters | **VERIFIED** |
| M-2 | Microphone produces continuous samples, screen off | same window, per-interval counts | **VERIFIED** |
| M-3 | The two sensors arm and release independently | mic-only arm must not open the camera | **VERIFIED** |
| M-4 | Encoded stream is decodable by a real decoder | encoder→decoder round-trip, frame count | **VERIFIED** |
| M-5 | Idle release after disarm | camera clients empty, idle CPU ≈ 0 | **VERIFIED** |
| M-6 | Revoked permission ⇒ process death ⇒ restart arms nothing | revoke mid-capture, then inspect | **VERIFIED** |
| C-1 | Audio codec interoperable with the target stack | software Opus build for arm64/API 28 | **BUILD-VERIFIED**; on-device encode NOT DONE |
| C-2 | A/V timestamp alignment within a bounded tolerance | needs a common clock + muxed container | **NOT DONE** — blocked on §4.2 |
| T-1 | Authenticated media session over mutual TLS | needs the media channel in §2.3 | **NOT DONE** |
| T-2 | Unauthorized peer cannot obtain media | negative test against the real transport | **NOT DONE** |
| T-3 | Consent withdrawal closes the session and frees the sensor | extend the local disarm test across the wire | **NOT DONE** |
| T-4 | Reconnect after network change, no duplicate sessions | link flap + session count | **NOT DONE** |
| T-5 | Slow consumer drops frames, bounded memory | throttled reader on a small host | **NOT DONE** |
| T-6 | One node's failure does not affect another | two nodes, kill one | **NOT DONE** (D-04) |
| K-1 | Talkback audible with AEC active, no echo | needs a loopback measurement | **NOT DONE** |
| A-1 | Real Alexa camera discovery | needs a Smart Home skill + WebRTC — §6 | **NOT DONE** |

Rule applied throughout: a property is only "verified" when a **positive observation** backs it —
a counter, a decoded frame, an explicit refusal. Absence of an error is not evidence.

---

## 4. Remaining work, in dependency order

### 4.1 Authenticated remote media (the critical path)

1. Define the media-channel framing on the existing TLS session (stream id, codec, dimensions,
   sample rate, per-frame pts, keyframe flag) — extend X-01's protocol, do not fork it.
2. Bind a session to the **certificate identity** plus the per-sensor consent grant, and make
   revocation tear down active media (D-04's revocation clause).
3. Replace the loopback `MediaSink` with a dial-out session; keep the local sink as a test double.
4. Enforce bounded buffering + frame drop on a slow consumer.
5. Reconnect without duplicate sessions.

**Blocked by:** nothing external. Requires the C++ side and the APK to move together.

### 4.2 A/V synchronization

- Today video and audio are two independent streams with independent timestamps and no shared
  clock. There is **no** container and **no** muxing.
- Required: a common timebase on the phone (media timestamps are already available and were
  measured advancing in real time), a declared offset at session start, and a receiver-side jitter
  buffer with a bounded playout delay.
- D-04 already names "clock-skew handling" — this belongs in that work, not beside it.
- **Not startable** before §4.1 gives the streams a shared session.

### 4.3 Audio codec compatibility

- Measured reality: the device has **no Opus encoder and no G.711 encoder** (both decoder-only).
  WebRTC's mandatory audio codecs are Opus and G.711 PCMU/PCMA.
- **Software Opus is available**, proven by building opus 1.4 with this project's NDK r26d for
  arm64-v8a / API 28 → `libopus.so`, ELF ARM aarch64, ~1.7 MB, exporting `opus_encode`,
  `opus_encoder_create`, `opus_encoder_ctl`, `opus_repacketizer_*`. The missing hardware encoder is
  **not** a blocker.
- Remaining: wire libopus into the APK behind the existing separately-armed microphone control, then
  re-measure encoded frames and CPU the way AAC-LC was measured (710,400 PCM → 694 AAC-LC frames at
  ~64 kbps, ~21.3 ms/frame), and verify an on-device Opus encode.
- Options if Opus proves too costly: AAC-LC + gateway transcode, or software G.711 PCMU (trivial
  companding, ~64 kbps narrowband, a WebRTC mandatory codec). Decision belongs with §4.1 because the
  choice constrains the framing.

### 4.4 Two-way talkback and echo cancellation

- Talkback is **not implemented**, and the brief is explicit that it must not be reported as
  implemented unless independently tested.
- The phone advertises an AOSP AEC (`7b491460-8d4d-11e0-bd61-0002a5d5c51b`, inventoried in
  HG-CAM-01) but nothing drives it yet.
- Requirements: a playback path into the same session, AEC/NS/AGC configured in the audio graph,
  and an explicit half/full-duplex policy. Measurement needs a loopback test, because "no echo was
  reported" is not evidence of no echo.
- Independent of §4.1…§4.3 in design, but it shares the session and the codec decision.

### 4.5 Alexa-compatible camera discovery — see §6

---

## 5. What this milestone does NOT change

- No main merge; this is an isolated branch based on `cad758f`.
- No public media endpoint, no listening socket reachable off the device.
- No router, VPS, Oracle, Cloudflare, mail or V380 change.
- No household audio or video stored or uploaded; the evidence is counters, timestamps and format
  strings.

---

## 6. Alexa compatibility (Task 3)

### 6.1 What exists today is NOT a camera integration

The repo's Alexa surface is an **Alexa custom skill plus a simulated bridge**
(`src/core/AlexaContract.*`, `src/bridge/hg_alexa_bridge.cpp`, `alexa_skill/`) that serves an
**aggregate-only** contract. It answers questions about HomeGuardian state; it does not present
itself to Alexa as a device. The Docker target is a stand-in for that bridge, not a device
simulator.

**A real Alexa camera integration is an Alexa Smart Home skill with a
`Alexa.CameraStreamController` directive and a WebRTC (or HLS) media path.** That is a different
interface with different certification, different auth and a different discovery model. The two must
stay distinct, and nothing in the existing aggregate contract should be stretched to pretend
otherwise.

### 6.2 Intended device identity

- **Identity source:** the existing per-device certificate (X-01) — this node is one enrolled device,
  not an account. The phone does not have an Alexa identity of its own; Alexa identity is a property
  of the HomeGuardian account linking, established separately.
- **HomeGuardian-side device id:** a stable, device-scoped id (D-04's "device-scoped source ids"),
  derived from the certificate identity rather than a user-editable label.
- **Friendly name / endpoint id:** assigned at account-link time, not by the phone.
- The phone must never carry an Alexa token. Alexa credentials belong to the hub, consistent with
  the existing credential store's separation.

### 6.3 Declared capabilities (proposed)

| capability | interface | prerequisite |
|---|---|---|
| Live camera view | `Alexa.CameraStreamController` | §4.1 transport + WebRTC signalling; §4.2 sync |
| Camera on/off state, reporting | `Alexa.RangeController`/`ToggleController` + `Alexa.EndpointHealth` | per-sensor consent already exists |
| Two-way audio (talkback) | `Alexa.CameraStreamController` two-way / `Alexa.RTCSessionController` | §4.4 AEC, measured |
| Microphone / privacy mute | explicit state + reporting | must reflect the REAL sensor state |

Constraint carried over from the existing design: **the device must not report a capability it
cannot back.** The phrasebook already refuses to announce a camera without an authenticated device
present; the same rule must govern capability advertisement, or Alexa will offer a control that
cannot work.

### 6.4 Discovery information

- Discovery is served by the **hub/skill** after account linking, not by the phone. The phone's job
  is to keep its enrolment, consent state and health accurate enough that discovery reflects
  reality.
- Required per endpoint: stable id, friendly name, description, manufacturer/model strings,
  capability list (§6.3), and reachable/health state.
- Only devices with a valid, unrevoked certificate and at least one consented sensor should appear.
  A revoked device must disappear on the next discovery cycle.

### 6.5 Authorized media-session requirements

- Session setup must be authorized by **account linking + the device's own consent grant**. Alexa
  authorizing a view does not by itself satisfy the device's consent gate, and the device's consent
  does not authorize Alexa without linking.
- Media must not be reachable from a public endpoint: the standard shape is a short-lived,
  per-session credential issued by the hub for a specific session, with the phone dialling out or
  answering only on an authorized session.
- Revocation and consent withdrawal must terminate an in-flight session.
- No default public media endpoint — restated because this is the clause most likely to be traded
  away for convenience.

### 6.6 Alexa status summary

| item | status |
|---|---|
| Aggregate query contract (existing) | **A working, simulated surface** — unrelated to camera media |
| Device identity for a camera node | **Design only** — reuses X-01; nothing implemented |
| Capability declaration | **Design only** — must not over-advertise |
| Discovery via the hub | **Design only** |
| Authorized media session + WebRTC | **Blocked** on §4.1 and §4.2 |
| Two-way audio | **Blocked** on §4.4 |
| Certification / Marketplace submission | **Not started, not in scope** |

---

## 7. Verified / prototype / blocked

**Verified on hardware:** continuous screen-off camera + microphone capture over 30 minutes with
independent arming and release; decodable H.264; AAC-LC from real microphone PCM; fail-closed,
loopback-only, token-gated local sink; permission revocation behaviour; idle release; software Opus
**build** for the target ABI.

**Prototype:** `MediaSink` (a local test double, not a transport); the AAC-LC path (real encode, but
no session/transport around it); the software-Opus artifact (built and symbol-verified, never
executed on the device).

**Blocked / not done:** authenticated remote media transport; A/V synchronization; on-device Opus
encoding and the codec decision; talkback with measured AEC; real Alexa camera integration;
multi-node media semantics (D-04); suspend-survival evidence (explicitly not the acceptance
condition).
