# HomeGuardian AI — Architecture

## Overview

HomeGuardian AI follows a three-tier, event-driven architecture with clear
separation between acquisition, inference, event processing, and presentation.

### Tier 1 — Android Acquisition Client (later phase)

A native C++ daemon running on an Android phone (MT6580, 1GB RAM) that
captures sensor data, camera frames, and audio, performs lightweight
threshold detection, and forwards compact observations to the PC via Wi-Fi.

**Capabilities:**
- Accelerometer, light, and proximity sensor reading (Android NDK `ASensorManager`)
- Camera frame acquisition (Android NDK Camera2 API)
- Audio capture and voice activity detection (Android NDK AAudio)
- Lightweight threshold detection (motion spike, light step-change, proximity toggle)
- Wi-Fi data forwarding to PC (libcurl or WebSocket)

**Constraints:**
- No root access assumed
- Android foreground-service restrictions apply
- Background camera/audio capture is restricted by Android security model
- Battery life is a primary concern
- Consent must be explicitly granted for all capture

**Status:** Not yet implemented. Phase B focuses on Tier 2 only.

### Tier 2 — PC-Side C++ Processing (current phase)

A native C++ application running on the PC (40 logical CPUs, 15GB RAM) that
receives data from Tier 1 (or simulated sources), runs ML inference,
correlates events, and produces alerts and dashboard updates.

**Modules:**
- OpenCV-based image and video processing
- ONNX Runtime inference (optional local ML backend)
- Audio feature extraction (openSMILE or custom)
- Body-pose estimation (MediaPipe C++ or OpenPose)
- Experimental remote photoplethysmography (rPPG)
- Time-series anomaly detection
- Event correlation and multimodal fusion
- Explainable safety rules
- Optional animal-sound classification

**Constraints:**
- CPU budget: 30% of 40 logical CPUs = 12 logical CPUs max
- Memory: 15GB total, monitor usage
- No cloud dependencies for core operation
- All inference is local

**Status:** Phase B — minimal vertical slice (build system, entry point,
logging, config, graceful shutdown, tests).

### Tier 3 — Hybrid Acquisition and Inference (future phase)

Evaluates sending compact derived observations instead of continuously
transmitting all raw media.

**Approach:**
- Send face-region crops instead of full frames (where authorized)
- Send pose keypoints instead of full video
- Send voice segments instead of continuous audio
- Send compact sensor summaries instead of raw streams

**Decision criteria:** measured bandwidth, CPU, RAM, latency, privacy, and accuracy.

**Status:** Not yet implemented. Requires Tier 1 and Tier 2 to be operational.

## Design Principles

1. **Local-first** — all data processing happens on the local network.
2. **Event-driven** — sensors and AI models emit events; the pipeline
   correlates and routes them.
3. **Interface-based** — every external dependency (camera, AI provider,
   database, notification) sits behind an interface.
4. **Consent-gated** — no sensing or inference runs without explicit
   user authorization.
5. **Explainable** — every alert and inference includes confidence scores
   and evidence references.

## Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        Presentation Layer                         │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────────┐  │
│  │  Web         │  │  Alexa+      │  │  Notification        │  │
│  │  Dashboard   │  │  MCP Server  │  │  Service              │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────┬────────────┘  │
└─────────┼─────────────────┼─────────────────────┼───────────────┘
          │                 │                     │
┌─────────┼─────────────────┼─────────────────────┼───────────────┐
│         │        Core Application               │               │
│  ┌──────┴─────────────────┴─────────────────────┴───────────┐  │
│  │              Event Processing Pipeline                    │  │
│  │  ┌─────────┐  ┌──────────┐  ┌───────────┐  ┌─────────┐  │  │
│  │  │ Ingestion│→│ Correlate│→│  Infer    │→│  Route   │  │  │
│  │  └─────────┘  └──────────┘  └───────────┘  └─────────┘  │  │
│  └──────────────────────────────────────────────────────────┘  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │  Event   │  │  State   │  │  Consent │  │  Config  │        │
│  │  Store   │  │  Manager │  │  Manager │  │  Manager │        │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘        │
└─────────────────────────────────────────────────────────────────┘
          │
┌─────────┼─────────────────────────────────────────────────────┐
│         │              Inference Layer                           │
│  ┌──────┴──────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │  Face       │  │  Voice   │  │  Posture │  │  Custom  │    │
│  │  Analysis   │  │  Analysis│  │  Analysis│  │  Models  │    │
│  └─────────────┘  └──────────┘  └──────────┘  └──────────┘    │
└─────────────────────────────────────────────────────────────────┘
          │
┌─────────┼─────────────────────────────────────────────────────┐
│         │              Sensing Layer                            │
│  ┌──────┴──────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐    │
│  │  Camera     │  │  Audio   │  │  Ring    │  │  Sensors │    │
│  │  Interface  │  │  Capture │  │  Events  │  │  (GPIO)  │    │
│  └─────────────┘  └──────────┘  └──────────┘  └──────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

## Component Responsibilities

### Sensing Layer
- **Camera Interface** — abstracted video capture. Implementations for IP
  cameras, USB cameras, and simulated sources.
- **Audio Capture** — microphone input with voice activity detection.
- **Ring Events** — Ring doorbell/camera event ingestion via Ring API or
  simulator.
- **Sensors** — GPIO, accelerometer, or environmental sensor inputs.

### Inference Layer
- **Face Analysis** — face detection, recognition, emotion estimation.
  Returns structured results with confidence scores.
- **Voice Analysis** — speech-to-text, emotion detection, speaker ID.
- **Posture Analysis** — pose estimation for fall detection.
- **Custom Models** — pluggable ONNX models for specific detection tasks.

### Core Application
- **Event Processing Pipeline** — ingests raw sensor/AI events, correlates
  them temporally and spatially, runs inference, and routes to outputs.
- **State Manager** — tracks home occupancy, known faces, recent events,
  and historical patterns.
- **Consent Manager** — enforces per-zone, per-activity consent policies.
- **Config Manager** — loads and validates configuration; supports runtime
  reload.

### Presentation Layer
- **Web Dashboard** — local HTML/CSS/JS dashboard for monitoring and
  configuration.
- **Alexa+ MCP Server** — self-hosted MCP server implementing the
  Streamable HTTP specification for Alexa+ integration.
- **Notification Service** — email, push, or webhook alerts for safety
  events.

## Event Model

Events are strongly typed, immutable objects with:
- Unique event ID (UUID v4)
- Schema version (uint32, >= 1)
- Event type: observation, inference, or alert
- Source identifier (e.g., "camera.front_door", "sensor.motion")
- UTC observation timestamp (when the event occurred)
- UTC ingestion timestamp (when the system received it)
- Optional monotonic timestamp for elapsed-time calculations
- Optional confidence in [0.0, 1.0]
- Optional severity: info, warning, or critical
- Structured JSON payload (object or null)
- Optional evidence reference (opaque, not a file path)
- Optional correlation ID (groups related events)
- Optional processing metadata

Events are validated on construction. Invalid events throw std::invalid_argument.
Normalization is deterministic and preserves original timestamps and provenance.

## Event Flow

1. A sensor or AI model emits a **raw event** (timestamp, source, type,
   payload, confidence).
2. The **ingestion** stage validates and normalizes the event.
3. The **correlation** stage groups related events (e.g., doorbell ring +
   face detection + motion) into a **scene**.
4. The **inference** stage applies rules and ML models to the scene.
5. The **routing** stage dispatches outputs: alerts, dashboard updates,
   notifications, or actions.

## Pipeline Architecture

The pipeline is synchronous and in-process:

```
EventSource → Pipeline::process() → [validate → normalize → correlate → alert]
```

- **IEventSource** — abstract interface for event producers (simulated, Android, etc.)
- **Pipeline** — synchronous processor with bounded history and configurable rules
- **ICorrelationRule** — abstract interface for correlation rules
- **TimeWindowCorrelation** — detects N events of specified types within a time window
- **EventValidator** — validates and normalizes events
- **Alert** — strongly typed alert with provenance and explainability

No threads, no queues, no async. The pipeline is deterministic and testable.

## Data Flow

```
Camera ──→ Face Analysis ──→ Raw Event ──→ Pipeline ──→ Alert/Dashboard
Ring   ──→ Ring Event    ──→ Raw Event ──→ Pipeline ──→ Notification
Mic    ──→ Voice Analysis──→ Raw Event ──→ Pipeline ──→ Concierge
```

## Persistence

SQLite stores:
- Event history (with automatic retention policies)
- Known faces (embeddings + metadata)
- Consent settings
- Configuration snapshots
- Alert history

## Consent-Gated Acquisition (Phase F1)

All protected acquisition and processing must pass through a single consent
decision point before any sensor is read. There is no real hardware yet; the
gate is demonstrated against a simulated sensor.

```
AcquisitionRequest ──> ConsentGate::check() ──> GateDecision
                          │  (fail closed)
                          ├─ denied  ──> no acquisition, no payload
                          └─ granted ──> SimulatedSensorSource.next() ──> synthetic Event
```

- **ConsentGate** (`src/core/ConsentGate.{h,cpp}`) — the sole decision point.
  Checks the config kill switch (`media_capture_enabled`), the latest consent
  decision for subject+purpose, grant status, expiry, withdrawal, and the
  requested data category. Missing, ambiguous, expired, denied, or withdrawn
  authorization fails closed. The rule is defined here once; components must not
  re-implement it.
- **ConsentGatedAcquisition** (`src/core/ConsentGatedAcquisition.{h,cpp}`) —
  consults the gate, and only reads the simulated sensor when authorized. A
  denied request never produces or processes a payload.
- **SimulatedSensorSource** (`src/core/SimulatedSensorSource.{h,cpp}`) — a
  deterministic `IEventSource` whose events are always tagged `synthetic`. A
  future real hardware component must call the gate before opening any device.

This is **simulated** enforcement only. It does not prove enforcement at a real
hardware boundary, and no camera, microphone, biometric, or cloud processing
exists. See `docs/PRIVACY.md` for the full threat model.

## Device Interface and Lifecycle (Phase F2.1)

Real camera/audio acquisition is abstracted behind `IMediaDevice`
(`src/core/IMediaDevice.h`), which defines the lifecycle contract a real Android
Camera2 / AAudio backend must satisfy. No hardware code exists here; mocks
behind the interface are used to test lifecycle behavior.

Device states: `Idle → Initialized → Capturing → Stopped`, with `Error` reachable
from any state, and `close()` returning to `Idle`. `stop()` releases capture
resources and the destructor guarantees stop()+close(), so a device is never
left capturing.

```
IMediaDevice (interface)          ConsentGuardedDevice (wrapper)
  initialize()  ────────────────►  gate.check() required before initialize/start
  start()       ────────────────►  gate.check() required before start
  stop()/close() ───────────────►  always allowed (stopping needs no consent)
  fail()        ────────────────►  recheck_and_enforce() forces stop+close on
                                     consent withdrawal/expiry, OS permission
                                     revocation, or device failure
```

- **IMediaDevice** — pure virtual lifecycle: `initialize/start/stop/close/fail`,
  `state()`, `kind()`. Deterministic resource cleanup.
- **ConsentGuardedDevice** — wraps an `IMediaDevice` and a `ConsentGate`. Every
  `initialize()`, `start()`, and per-acquisition `recheck_and_enforce()` calls
  the gate first and fails closed. Withdrawal, expiry, OS permission revocation
  (via a permission callback), device failure, and shutdown all force the device
  out of capture and release resources, and block further acquisition.
- **MockMediaDevice** (test-only) — records lifecycle transitions and
  open/close counts to assert deterministic cleanup without hardware.

Camera, microphone, biometric processing, and cloud upload remain disabled by
default (`media_capture_enabled = false`). Mock tests prove lifecycle and
consent-gating behavior only; they do **not** prove real hardware enforcement.

### Android backends (Phase F2.2)

Real camera/audio backends live in `src/backend/android/` behind `IMediaDevice`
and are compiled only for Android (`HOMEGUARDIAN_ANDROID`,
`-DHOMEGUARDIAN_BUILD_ANDROID_BACKEND=ON`):

- `NdkCameraDevice` — NDK camera API (`ACameraManager`/`ACameraDevice`/
  `ACameraCaptureSession`), available since API 24.
- `AAudioCaptureDevice` — AAudio (available since API 26).

Both are **fail-closed by default**: `initialize()` throws unless
`set_device_confirmed(true)` has been called after a physical device is
confirmed available and hardware acceptance testing has passed. No device was
attached at authoring time, so real capture is never opened and the backends
report `Error` rather than pretending capture works. They cross-compile for
`arm64-v8a` / API 34 with NDK r26d; on-device capture and permission-revocation
testing remain blocked pending hardware. `ConsentGuardedDevice` gates every
lifecycle call and every frame/sample delivery through the consent gate.

## Error Handling

- All external calls use timeouts and retries with exponential backoff.
- Sensor failures are isolated — one failing sensor does not crash the system.
- Inference failures degrade gracefully — the pipeline continues with
  reduced capability.
- All errors are logged with context via structured logging.

## Security

- All external input is validated and sanitized.
- Consent is checked before any sensing or inference.
- Local network only — no cloud dependencies for core operation.
- Configuration supports TLS for any external communication.
- No credentials or personal data in logs.

## Resource Management

- Bounded queues between all pipeline stages.
- Thread pool with configurable size (default: 4 threads).
- Graceful shutdown with deterministic cleanup.
- CPU and memory usage monitored and bounded.

## Ubuntu (Linux) Sensor Backend — assessment & design (added 2026-10-10)

**Status: interfaces + non-capture discovery + host tests VERIFIED; camera/audio
capture adapters PROPOSED and NOT implemented (awaiting explicit approval).**

The Ubuntu backend targets the existing development computer. It reuses the
existing consent stack — `ConsentGate`, `ConsentGuardedDevice`,
`IMediaDevice` — and does **not** duplicate any consent mechanism. The Android
and Ubuntu backends are distinguished only by the device adapter that sits
behind `IMediaDevice`; the consent boundary, event pipeline, and persistence are
identical across platforms.

Reusable components (shared with Android):
- `IMediaDevice` — the device lifecycle contract (initialize/start/stop/close).
- `ConsentGuardedDevice` — wraps any `IMediaDevice`; re-authorizes at the
  delivery boundary via `authorize_delivery()` immediately before every
  frame/sample. Withdrawal, permission revocation, or device error forces
  capture to stop (fail-closed).
- `ConsentGate` / `ConsentRepository` — consent evaluation and persistence.
- The synchronous in-process event pipeline and SQLite persistence layer.

Planned Ubuntu adapters (PROPOSED, not yet implemented):
- Camera: V4L2 (`/dev/video*`) via a thin adapter behind `IMediaDevice`. Bounded
  frame buffer; no on-disk writes; every frame re-checked at the delivery
  boundary before processing.
- Microphone: ALSA (`arecord`/ALSA PCM) or PipeWire (`pactl`/`libpipewire`) behind
  `IMediaDevice`. Bounded ring buffer; re-check before every buffer delivery.
- Speaker: PipeWire/ALSA playback adapter (output-only; no capture semantics).

Implemented now (non-capture):
- `src/backend/linux/SensorDiscovery.{h,cpp}` — `ISensorDiscovery` interface and
  `LinuxSensorDiscovery` implementation that enumerates camera nodes
  (`/dev/video*` + sysfs name) and audio sources/sinks (via `pactl` metadata)
  **without opening any device**. Pure metadata; no frames or samples read.

Safety invariants for the Ubuntu backend (identical to Android):
- Capture disabled by default; requires explicit operator action.
- Consent rechecked immediately before every frame/audio-buffer delivery.
- On consent withdrawal, permission revocation, or device error: stop delivery
  and release resources.
- No recording to disk, no uploads, no retention of personal media by default.
- No automatic camera/mic access at launch or during tests.

**This assessment does not open the computer's camera or microphone.** The
capture adapters are the next milestone and require explicit approval before the
first activation.

## Docker assessment (added 2026-10-10)

**Recommendation: keep the C++ core on the host for now; defer containerizing
sensor access.** The core event pipeline, consent gate, and SQLite persistence
are portable and could run in a container, but the physical camera/audio access
is the deciding factor. Docker does **not** grant hardware access automatically:
a container would need explicit device mappings (`--device /dev/video0`), group
membership for `video`/`audio` inside the container, and — for full PipeWire
desktop audio — a mounted socket or `--group-add`. Privileged containers are
avoided per policy.

Security trade-offs (documented before any implementation):
- `--device /dev/video*` exposes the raw device; a compromised container could
  capture directly, bypassing the consent gate that lives in the process. This
  weakens the fail-closed guarantee unless the gate remains in the capture path.
- Audio via ALSA needs `/dev/snd` access; via PipeWire needs the user session
  socket, which couples the container to the desktop session.
- Recommendation: if a container is ever used for the core service, run the
  sensor adapters on the host (or a separate privileged-exempt helper) and keep
  the consent gate in the delivery path. No privileged containers, no broad
  device exposure. No container deployment has been implemented.

## Alexa+ integration findings (added 2026-10-10)

**Status: researched, NOT integrated; no Alexa+ interaction demonstrated.**

Verified findings (Amazon developer documentation, 2026):
- **Alexa Voice Service (AVS) developer tools are no longer generally available
  for Alexa Built-in.** Amazon directs developers to the *Works with Alexa* (WWA)
  program for device integrations. So "build our own Alexa client on the Ubuntu
  box via AVS" is not a currently supported route.
- The supported route for a HomeGuardian voice interface is an **Alexa custom
  skill** or **Smart Home skill**:
  - Hosted via Alexa-hosted (AWS Lambda, Node.js 16.x or Python 3.8), or
  - Self-hosted HTTPS endpoint invoked by the skill.
  - Account linking uses OAuth 2.0 / Login with Amazon (LWA).
  - The **Alexa Developer Console simulator** can test skill request/response
    flows without a physical Echo device.
- **Simulator limitation:** the Alexa simulator cannot access the Ubuntu
  computer's physical camera or microphone. Any claim that Alexa+ controls
  HomeGuardian sensors would require the skill backend to reach the C++ service
  over an authenticated network interface — not local sensor access. This has not
  been built or demonstrated.

Proposed architecture (if pursued): Alexa skill → authenticated HTTPS endpoint →
C++ HomeGuardian service (event pipeline unchanged; Alexa stays a separate,
authenticated client). The C++ event pipeline remains independent of Alexa. No
Amazon credentials have been requested; none will be placed in source or Git.

### Alexa custom skill prototype — implemented (added 2026-10-10)

**Status: handler + unit tests VERIFIED (13 Node cases); console-simulator
end-to-end NOT YET RUN (requires manual console steps — see
`alexa_skill/README.md`).**

A minimal **Alexa custom skill** using the **ASK SDK v2 for Node.js**
(`ask-sdk-core` 2.14.0) lives in `alexa_skill/`:

- `index.js` — Lambda entry point (`exports.handler`) and intent handlers:
  `HomeStatusIntent`, `GetAlertSummaryIntent`, `GetRoutineStatusIntent`,
  `AMAZON.HelpIntent`, `AMAZON.StopIntent`/`AMAZON.CancelIntent`, plus a safe
  catch-all. Backend/error paths return generic, non-sensitive responses and
  never leak exception text or secrets (verified by tests).
- `src/backend-interface.js` — the documented contract (`HomeGuardianBackend`)
  between the skill and the C++ core, with a **mock implementation**. The
  handlers depend only on this interface; a real backend would be injected here
  with no handler changes. **No network, disk, sensor, or credential access.**
- `interactionModels/custom/en-US.json` — the interaction model
  (invocation name "home guardian").
- `test/handler.test.js` — 13 unit tests using Node's built-in test runner,
  runnable with **no deployment, no AWS, no simulator**.

**Verified current facts (Amazon docs, 2026):**
- **The Alexa Developer Console Simulator requires a configured endpoint and
  deployed skill code** — it cannot test a skill with no backend. The
  lowest-cost supported path is an **Alexa-hosted skill**, which auto-provisions
  the AWS Lambda without you creating an AWS account; testing volume is within
  the Lambda free tier (expected charge $0). Node.js 16 is deprecated, so the
  hosted skill defaults to a current Node.js (18/20/22) or Python runtime; the
  ASK SDK v2 is compatible.
- The Alexa simulator **cannot** access the Ubuntu computer's camera or
  microphone. This skill exposes **no** sensor status or control.

**Security boundary:** the skill is a separate, thin client. It never contacts
the C++ process or any sensor in this prototype. Any real backend integration
requires an authenticated interface and separate approval. No credentials are
stored in the repository.

## Simulator-first development environment (added 2026-10-10)

**Decision: SIMULATOR FIRST; Docker only if simulation is insufficient.** The
simulator validates the entire consent/lifecycle/delivery/bounded-buffer/shutdown
machinery on the shared code paths, so no real sensor is opened during
development.

Implemented: `src/backend/simulator/SimulatedSensorBackend.{h,cpp}` — a
deterministic, in-process `IMediaDevice` implementation:

- **Deterministic camera frames:** seeded 64-bit LCG fills `width*height` bytes.
  Same seed and call sequence → identical bytes (reproducible tests).
- **Deterministic microphone samples:** seeded LCG fills 16-bit PCM mono buffers
  (`samples_per_buffer * 2` bytes).
- **`BoundedRing` acquisition buffer** (default capacity 256): overwrite-oldest
  on overflow; `buffer_size()` can never exceed capacity; `buffer_dropped()`
  counts evictions. This is the bounded-memory invariant under test.
- **Lifecycle + error injection/recovery:** `initialize/start/stop/close` follow
  the `IMediaDevice` contract with idempotent stop/close; `fail()` forces Error
  and releases capture; `recover()` is the deterministic cleanup + re-init path.
- **Speaker output:** modeled as output-only and validated only at the
  interface/lifecycle level in-simulator; real PipeWire/ALSA playback is not
  exercised here.

It sits behind the **same** `ConsentGuardedDevice` gate as the Android and
Ubuntu backends, so `authorize_delivery()` at the frame/sample boundary is the
identical production enforcement point exercised by these tests.

**Simulation safety:** opens no device file, makes no capture-hardware syscalls,
writes no media files, no network. Frames/samples exist only in memory. This
backend must never be used in place of a real backend in production capture.

**Linux simulator/virtual-device environment assessed (not all required):**
- `v4l2loopback` kernel module **is present** on this host
  (`/lib/modules/.../v4l2loopback.ko.zst`, v0.15.3) but is **not loaded** and
  **cannot be loaded without root** (no passwordless sudo). Not used.
- PipeWire null-sink/source: a `module-null-sink` can be created at runtime
  (verified: loaded sink `hgsim` + `.monitor`, then unloaded). Useful later for
  real audio-path testing; **not used** in this phase.
- ALSA `null` PCM plugin: present and usable (`arecord -D null` succeeds).
  Not used in this phase.
- `ffmpeg` (in `~/.hermes/tools`) with `lavfi` sources: available for generating
  synthetic streams if a future hardware-in-loop test needs a real device feed.
  Not used in this phase.

**Linux-native simulation is the PRIMARY testing path** (decision, added
2026-10-10). `SimulatedSensorBackend` is configured through a `Config` struct so
adding options never reorders a positional constructor:

- **Camera:** configurable `width`/`height`; optional `frame_rate` (frames/sec)
  pacing via wall-clock sleep (0 = unpaced). Pacing affects timing only; frame
  bytes remain fully deterministic.
- **Microphone:** configurable `sample_rate`, `samples_per_buffer`, `channels`
  (mono/stereo, 16-bit PCM interleaved), and a `TestSignal`: `noise` (existing
  pseudo-random), `sine` (pure tone at `tone_hz`), `square`, and `silence`. The
  deterministic waveforms let tests assert exact, known content.
- **Speaker:** `play(payload)` models the audio **output** path only (no capture,
  so no consent delivery gate); `inject_playback_error()` + recovery via
  close/re-init. Playback never touches the capture buffer, disk, or hardware.
- **Buffer:** configurable `ring_capacity` (default 256); overwrite-oldest keeps
  memory bounded.

The default test suite needs **no root, no kernel modules, no physical
hardware**. V4L2/ALSA/PipeWire virtual devices are documented as optional future
hardware-in-loop tools but are **not** required and are not loaded/installed.
No physical camera or microphone is activated; the simulator never opens a real
device.

### End-to-end simulator workflow (added 2026-10-10)

`tests/core/test_e2e_simulator.cpp` wires the existing components into one
deterministic, automated scenario (host, non-capture):

1. Initialize simulated camera, microphone, and speaker.
2. Feed authorized synthetic frames/samples through the consent-guarded delivery
   path (`ConsentGuardedDevice::authorize_delivery()` before each payload).
3. Pass authorized inputs to a clearly-labelled **`SyntheticTestProcessor`** — a
   trivial, deterministic component that only ever sees gate-authorized payloads
   and produces observation `Event`s. It performs **no** real analysis and draws
   **no** conclusion about people, danger, health, or emotion.
4. Generate events via the existing `Pipeline` + `TimeWindowCorrelation` rule
   (fires after 2 observations → `Severity::info` alert).
5. Build a **safe response** (`build_safe_response`) — synthetic, aggregate, no
   sensitive detail, explicitly labelled "not a real-world alert".
6. Send the response to the simulated speaker (`play()`) and verify playback.
7. Verify **event ordering** (delivery order preserved), **bounded memory**
   (capture buffers ≤ capacity; bounded pipeline history), and **lifecycle
   cleanup** (stop/close → Idle, buffers cleared, `released()`).

Failure/consent scenarios (all proven fail-closed):
- Consent **denied before initialization** → device never initialized; processor
  never runs; no event created.
- Consent **withdrawn between two deliveries** → the second delivery is denied
  at the boundary and never reaches the processor.
- **Capture disabled** (kill switch) during processing → next delivery denied
  even with an active grant.
- **Camera failure** → delivery stops (fail-closed); recovery via close/re-init
  resumes delivery.
- **Event-processing failure** (a throwing correlation rule) → the pipeline
  surfaces the error and a healthy pipeline remains usable.
- **Speaker playback failure** then recovery via close/re-init.
- **Shutdown with pending buffered data** → buffers are bounded during capture
  (overwrite-oldest) and fully released on stop/close.

Privacy boundary: speaker output is **independent** of the capture-consent gate
(playback is an output path, not capture), while all capture data must pass the
delivery gate. Denied capture data never reaches the processor (asserted). This
is a synthetic test scenario only — it does not detect real-world danger,
identify people, or provide medically/emotionally sensitive conclusions.

### Capability matrix

| Requirement / behavior | Simulator (host) | Docker | Real hardware |
|---|---|---|---|
| Consent denial / withdrawal / expiry blocks delivery | VALIDATED | not needed | not needed |
| Delivery-boundary `authorize_delivery()` per frame/sample | VALIDATED | not needed | confirm on device |
| Permission revocation stops delivery | VALIDATED (injected) | not needed | OS-level confirm |
| Device lifecycle + idempotent stop/close | VALIDATED | not needed | confirm on device |
| Bounded buffers (no unbounded growth) | VALIDATED | not needed | confirm on device |
| Deterministic frame/sample generation | VALIDATED | not needed | n/a |
| Error injection + recovery | VALIDATED | not needed | confirm on device |
| Shutdown / resource cleanup | VALIDATED | not needed | confirm on device |
| Alexa conversational behavior (skill logic) | Skill-logic sim (future) | n/a | n/a |
| Real V4L2 ioctl / format negotiation | not simulated | could host build deps | **requires hardware** |
| Real ALSA/PipeWire PCM stream + latency | not simulated | could host build deps | **requires hardware** |
| Real device enumeration (`/dev/video*`, sound server) | metadata-only (SensorDiscovery) | n/a | **requires hardware** |
| Real OS permission semantics | not simulated | n/a | **requires hardware** |

**Docker conclusion:** Docker is **not necessary** for the current milestone.
Every requirement in this phase — consent, lifecycle, delivery boundary,
permission revocation, bounded buffers, error recovery, shutdown — is validated
deterministically by the host simulator on the shared code paths. The only items
that genuinely need more than the simulator are real V4L2/ALSA/PipeWire device
interaction and real OS permission semantics, and those require physical
hardware (or a loaded `v4l2loopback` / null-sink), which Docker on this host does
not provide either. Docker remains a documented fallback if a future requirement
needs a specific Ubuntu userspace or build-dependency isolation; it is not
implemented. No privileged container; no host PipeWire socket mount; no
camera/mic device passthrough.
