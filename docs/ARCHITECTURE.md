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

## Build & portability (added 2026-10-11)

- **Out-of-source build** via CMake. First configure fetches spdlog v1.13.0 and
  nlohmann/json v3.11.3 from GitHub (network required **once** to populate the
  dependency cache); SQLite and Catch2 are vendored in `third_party/`.
- CMake include paths use `${CMAKE_SOURCE_DIR}` (no absolute host paths), so the
  documented `mkdir build && cd build && cmake .. && make -j4` reproduces on any
  machine. Verified by a fresh out-of-source reproduction (135/1472 pass).
- The `homeguardian` application builds and runs a clean main loop with capture
  disabled by default; it opens no sensor and writes no media.
- `config/homeguardian.json` sets `media_capture_enabled: false`,
  `cloud_processing_enabled: false`, and a `data_dir` field. The current
  `Application` does not open a database (the field is reserved), so no
  privileged path is required to run.

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

## Dual logical-device simulation + shared audio broker (added 2026-10-11)

Two **independent logical devices** participate in a **shared Ubuntu audio
broker** (all simulated, host-only, no physical sensor):

- `simulated_alexa_enabled_device`
- `simulated_alexa_assistant`

Each logical device (`src/backend/simulator/LogicalDevice.{h,cpp}`) has its own
**identity, capabilities, lifecycle, status, bounded event history, and
configuration**. The devices are independent: one device's state, history, or
failure never affects the other. A logical device is a HomeGuardian simulation
entity — it does **not** run Amazon's original Alexa OS and makes no Alexa-
product claim. "Alexa-enabled" here means only that the logical device consumes
the shared microphone and emits speaker output through the broker.

The **shared audio broker** (`src/backend/simulator/AudioBroker.{h,cpp}`)
provides one future Ubuntu mic source and one speaker output:

- **One microphone acquisition source.** The broker owns a single simulated mic
  and starts exactly one capture session. There are no duplicate/competing
  capture sessions; `capture_and_fanout()` advances the shared source once and
  fans the SAME bytes out to consumers.
- **Per-consumer consent at every audio-delivery boundary.** Each consumer is
  registered with its own `ConsentGate` + `AcquisitionRequest` + optional
  OS-permission callback. At each fan-out the broker re-evaluates every
  consumer independently (`ConsentGate::check`); a denied/withdrawn/revoked
  consumer receives nothing (fail-closed) and does **not** stop the shared
  source for others. If NO consumer is authorized, the shared source produces
  nothing (stays quiet).
- **Bounded output queue + speaker arbitration.** Responses from all consumers
  are enqueued onto ONE bounded queue (default 64; overflow rejects the new job
  and counts it dropped — memory never grows without bound). `play_next()`
  arbitrates the single speaker: it never overlaps playback; a job is removed
  from the queue only after a successful play, so a playback error leaves the
  job queued for retry after recovery.
- **Explicit errors + deterministic shutdown.** Speaker playback errors are
  injectable and recoverable (close + re-init). `shutdown()` stops the single
  capture session, clears the bounded queue, releases the speaker, and is
  idempotent.

**Camera stays separate.** A logical device may advertise `has_camera`, but the
broker provides no raw camera access to it; camera is a distinct simulated
peripheral, and no assumption is made that an Alexa-enabled device yields camera
frames.

**Privacy/safety boundary:** speaker playback is an OUTPUT path and is NOT
gated by the capture-consent boundary (it is not acquisition); capture fan-out
IS gated per consumer. Everything is the deterministic in-process simulator — no
physical microphone/speaker/camera, no AWS, no Alexa OS, no credentials.

## Runtime hardening: alert policy + lifecycle (added 2026-10-11)

**`AlertPolicy`** (`src/core/AlertPolicy.{h,cpp}`) is a small, deterministic
notification layer applied to alerts the existing Pipeline/rules produce. It does
not replace the correlation engine; it governs *how often* a repeated firing is
surfaced:

- **Identity** = `rule_name` + `severity`. Different keys are tracked and
  delivered independently (never suppressed against each other).
- **First occurrence** for a key is always delivered.
- **Cooldown** (default 60s, configurable): repeat firings for the same active
  condition within cooldown are suppressed and counted.
- **Re-arm**: after cooldown expires the next firing re-notifies; if the
  condition clears (a cycle yields no alert for that key) the next firing is a
  fresh condition and is delivered immediately.
- **Bounded**: retained dedup state is capped (oldest evicted); history is
  bounded. An injectable clock makes cooldown/re-arm deterministic in tests; a
  backward-moving clock cannot extend a cooldown.

Rationale: a `TimeWindowCorrelation` rule fires on **every** evaluation where the
trigger holds, so N observations for one continuous condition yield up to N-1
alerts — correct rule behavior, but noisy. `AlertPolicy` collapses that to one
notification per active condition with a re-notification interval, without hiding
the first occurrence or any materially new (different-key) alert. It is a
notification policy, **not** a safety detector; a suppressed duplicate is still
an active condition. The alert is a synthetic presence notice, never a real-world
danger detection.

**Application lifecycle** (`AppState`): `Constructed → Starting → Running →
Stopping → Stopped`, with `Failed` for config/startup errors. `run()` applies the
config kill switch (capture disabled by default; configuration cannot silently
grant consent), and on partial startup failure tears down already-started
components so the runtime never reports healthy while half-initialized.
`is_healthy()` is true only while cleanly `Running`/`Stopping`, never `Failed`.
Shutdown is idempotent and clears bounded queues. A reproducible demonstration is
in `docs/DEMO_SIMULATOR.md`.

## Runtime readiness: durable audit + observability (added 2026-10-11)

**`PersistenceAudit`** (`src/core/PersistenceAudit.{h,cpp}`) durably records
**delivered alert metadata** using the EXISTING SQLite layer (`SQLiteDatabase` +
`SchemaManager` + `AlertRepository`) — no parallel storage engine. The `alerts`
table already stores only metadata (alert_id, severity, message,
contributing_event_ids, detected_at, confidence, rule_name, details JSON).

- **What persists:** delivered alert records only. **What never persists:** raw
  audio/video, biometric data, media payloads, or unnecessary personal
  identifiers. Consent is never persisted as permanent authorization — consent
  lives only in the in-memory gate and fails closed after restart.
- **Restart semantics:** alerts written before the current process's start are
  "historical"; the class records the process start and exposes
  `historical_alert_count()` so historical and current-process alerts are
  distinguishable. Verified by running the actual executable twice against one
  DB (run 2 reports `historical_alerts=1`).
- **AlertPolicy dedup/cooldown state is intentionally NOT persisted** (documented
  decision): the simulator-only lifecycle treats each process start as a fresh
  condition; persisting cooldown would add complexity with no concrete
  requirement.
- **Failure handling:** a write failure is caught, counted, and surfaced as
  DEGRADED (`persist=DEGRADED`, `persist_fail>0`) — it never reports success when
  a write failed and never corrupts runtime state. A corrupt/unsupported DB file
  causes the open to throw (caller runs degraded); the file is never silently
  deleted.
- **Retention:** `prune_older_than(cutoff)` deletes alerts older than a cutoff,
  transactionally, preserving newer/unrelated records.

**`RuntimeStatus`** (`src/core/RuntimeStatus.{h,cpp}`) provides bounded,
privacy-safe in-memory counters that RESET per process: app state, capture
kill-switch, media authorized/denied, events processed/rejected/failed, alerts
generated/delivered/suppressed, persist-failed, speaker queue depth/overflow/
played, startups/clean-shutdowns/recoveries. These are deliberately separate
from durable audit metadata so a counter can never be mistaken for audit
history. The summary line logs only counts/states — never media payloads,
secrets, credentials, or personal data — and emits one line per cycle, not per
sample. Observability output cannot grant consent or enable capture.

Both are wired into `Application`: persistence opens at startup (degraded on
failure), delivered alerts are persisted, and the status summary is logged when
entering the main loop and at clean shutdown.

## Alexa integration contract (added 2026-10-11)

`AlexaContract` (`src/core/AlexaContract.{h,cpp}`) defines the application-facing
contract for a future Alexa adapter. It is pure domain logic — no network, no
AWS, no deployed endpoint, no credentials.

- **Versioned, read-only schema:** contract version 1; operations `status` and
  `alert_summary` only. There is deliberately NO operation that enables capture,
  grants consent, changes config, or activates a sensor.
- **Authentication boundary:** authorization requires a verifiable credential
  checked by an injected `IAlexaCredentialVerifier`. A client-supplied identity
  is NEVER trusted as proof. Missing/invalid/expired credentials fail closed
  with `unauthorized`; an authenticated principal not permitted for the specific
  operation gets `forbidden`. The contract never carries real credentials — the
  production verifier would validate a signed request/account-linking token.
- **Validation:** request bodies are bounded (≤4KB), must be a JSON object with
  only known fields; unknown fields, unknown operations, malformed JSON, and
  unsupported versions are rejected (never silently coerced).
- **Responses:** only approved, non-sensitive summary fields (state, capture
  flag, aggregate alert count/highest severity, capped alert ids). No media
  payloads, secrets, personal identifiers, filesystem paths, or internal
  diagnostics. Errors are generic (`error_message`).
- **Availability:** a degraded/unavailable application fails closed
  (`unavailable`) rather than returning stale/partial state.
- **Tested locally** with mock clients and deterministic fixtures: 14 C++
  contract cases (`tests/core/test_alexa_contract.cpp`) + 12 Node adapter cases
  (`alexa_skill/test/contract.test.js`) mirroring the same semantics.
- **NOT deployed:** a live Alexa-to-application integration requires a deployed,
  authenticated endpoint and is documented as pending. No claim of live
  integration is made.

## SQLite integrity and migration (verified 2026-10-11)

Reuses the existing `SQLiteDatabase` + `SchemaManager` + repositories. Verified
via `tests/persistence/test_integrity.cpp` (temporary databases only):

- **Migration v1→v2** is additive: alert metadata and identifiers, events,
  indexes (`idx_alerts_severity`, `idx_alerts_detected_at`), and the primary-key
  constraint are preserved. `user_version` advances 1→2.
- **Repeat migration is idempotent** (3× re-open preserves data).
- **Interrupted/failed migration rolls back** within a transaction: partial v2
  tables are undone and `user_version` stays 1; a later real migration succeeds.
- **Retention** (`delete_older_than`) removes only expired alerts; newer and
  unrelated-table records are preserved.
- **Reopen after clean shutdown** returns persisted data.
- **Write after close fails loudly** (throws) — never reports silent success.
- **Corrupt/unsupported DB file** causes open/initialize to throw and the file
  is preserved (never silently deleted or reset).

## Privacy and consent guarantees (verified 2026-10-11)

`tests/core/test_privacy_consent.cpp` proves, on the components the running
Application uses: capture-disabled startup delivers no media despite grants;
withdrawn consent blocks only the affected consumer; unauthorized observations
never reach the event sink/pipeline; a restart (fresh runtime) does not restore
capture authorization (capture + consent are per-process, fail-closed);
persisted alerts contain only documented metadata fields (no audio/video/
payload/biometric keys); per-process status counters start at zero while audit
history persists separately and is reported as `historical`; retention removes
only expired alerts and preserves newer records.

## Rule-specific condition clearing + scheduled retention (added 2026-10-11)

- **P1:** `Application::process_event` re-arms AlertPolicy per REGISTERED rule
  name via `Pipeline::rule_names()` (derived from `ICorrelationRule::name()`),
  never a hardcoded literal — so adding rules needs no change. Covered by
  `[clearing]` tests (multi-rule independence, idempotent repeat, unknown-rule
  no-op).
- **P2:** configurable `alert_retention_days` (validated 0–36500; 0 = keep
  forever). `Application` prunes once at startup and at most once per 10-minute
  interval in the main loop via `PersistenceAudit::maybe_prune_retention`
  (bounded — never per-tick). Covered by `[retention]` tests (age window,
  disabled, interval-bounded, failure-degrades). NOTE: retention bounds logical
  row count; the SQLite file's physical size is not shrunk (no VACUUM — deleted
  pages may remain allocated).

## Local end-to-end integration bridge (added 2026-10-11)

`hg_alexa_bridge` (`src/bridge/hg_alexa_bridge.cpp`) is a small LOCAL-ONLY
executable that exposes the REAL `AlexaContract` over a newline-delimited JSON
(NDJSON) stdio boundary. It is the process boundary the JavaScript Alexa
adapter talks to — NOT a mock. The Node tests spawn the actual compiled binary
and exercise the full request/response path.

- **No network listener.** stdin/stdout only; nothing binds a port. Chosen over
  a local HTTP server because no existing architecture requires one and stdio
  avoids listener/permission surface.
- **Real contract, real state.** Each request line is parsed and handled by
  `AlexaContract::parse` + `handle` — all validation, version gating, and
  fail-closed auth live in the shared contract, not duplicated. Status/alert
  data come from the real `RuntimeStatus` + `PersistenceAudit`.
- **Authentication:** a test-only credential verifier loaded from a JSON file
  passed on the command line (`{token: {principal, allow_status,
  allow_alert_summary, expires_at_ms}}`; `expires_at_ms:0` = never expires). No
  real secrets; the table is generated by tests/demo and kept out of version
  control. A missing/unreadable table fails closed (every verify unauthenticated).
  The credential value is never logged or echoed.
- **Bounded + safe:** request lines are size-bounded (oversized → malformed);
  a defensive max-request lifetime bound; exits on EOF, a `shutdown` control
  line, or bad usage (exit 2). The Node client spawns it with an argument ARRAY
  (no shell → no injection), enforces an output bound, and kills on timeout.
- **Read-only:** only `status` and `alert_summary`; cannot enable capture, grant
  consent, change config, or activate a sensor.
- **Restart:** the bridge opens `homeguardian_bridge_audit.db` in its working
  directory. Restart tests run the real app to produce an alert into a temp DB,
  then start a fresh bridge against the same data and verify the historical
  alert is visible in the aggregate summary. Temporary databases only.

**Tests:** `alexa_skill/test/integration.test.js` (14 process-boundary cases) and
`alexa_skill/test/restart.test.js` (4 restart/degraded cases) drive the actual
compiled binary. **Demo:** `docs/demo_alexa_integration.sh`.

## Observation and inference data flow (added 2026-10-12)

A detection layer was added on top of the existing event pipeline. It reuses
that pipeline; it does **not** add a second event-processing engine.

```
 detector (SyntheticActivityDetector)
        |  produces
        v
   Observation ──to_event()──► Event (EventType::inference) ──► Pipeline
        |                                                          |
        |  provenance / detector_id / model_id / status             v
        |  carried in payload + processing_metadata        EventValidator
        |                                                          |
        v                                                          v
  AnomalyDetector ◄── numeric feature per (source, key)      Correlation
  FusionEngine    ◄── multiple Observations (timestamped)         |
        |                                                          v
        └──────────── explainable result ────────────►  AlertPolicy ─► Alert ─► Persistence
```

**Observation model** (`src/core/Observation.{h,cpp}`): a thin, validated wrapper
that adds what `Event` does not express — **provenance** (synthetic vs real),
**detector/model identity**, and an explicit **status**
(detected / not_detected / unavailable / inconclusive). It carries only bounded,
validated feature data (≤32 keys, ≤2 KB serialized, note ≤256 chars); never raw
media, embeddings, or transcripts. `to_event()` emits a standard
`EventType::inference` event so all downstream logic is unchanged.

**AnomalyDetector** (`src/core/AnomalyDetector.{h,cpp}`): a pure-C++ statistical
detector — rolling window, running mean/stddev, configurable z-score threshold,
a warm-up period (first `min_samples` samples only build the baseline), a
missing-observation check (surfaces a sensor gone quiet), and bounded memory
(≤`max_window` samples/key, ≤`max_keys` keys). Every flag carries a
human-readable reason (baseline, sample, z-score, threshold) — i.e. explainable.
It makes **no** medical/safety/behavioural claim; deviations are candidates for
review.

**FusionEngine** (`src/core/FusionEngine.{h,cpp}`): combines timestamped
Observations conservatively — confidence propagation (agreement raises it,
conflict and unusable inputs lower it; never exceeds mean input confidence), an
explicit time window (stale inputs excluded), and conflict handling (disagreement
yields `inconclusive`, never a silent winner). `unavailable` inputs never count
as evidence of presence.

**SyntheticActivityDetector** (`src/core/SyntheticActivityDetector.{h,cpp}`): a
deterministic synthetic detector that classifies a bounded numeric energy into a
`DetectorStatus`, producing `Provenance::synthetic` Observations. It reads no
camera/mic/file; it exists to exercise the framework end-to-end and feed the
demo. Out-of-range or near-threshold inputs are reported `inconclusive`, never
fabricated as detections.

### Capability matrix (added 2026-10-12)

| Capability | Status | Notes |
|---|---|---|
| Observation model + pipeline integration | **Implemented (synthetic)** | `Observation::to_event()` → existing `Event` pipeline; 275th test-case area |
| Explainable statistical anomaly detection | **Implemented (synthetic)** | z-score baselines, warm-up, missing-observation, bounded memory |
| Multimodal fusion (confidence/conflict) | **Implemented (synthetic)** | agreement/conflict/unavailable/stale handling, propagated confidence |
| Synthetic activity detector | **Implemented (synthetic)** | deterministic, no I/O, feeds demo + tests |
| Real face/pose/lip inference | **Deferred** | requires OpenCV/ONNX/MediaPipe — none installed; 8 GB host; not safe to add this stage |
| Real audio VAD/event detection | **Deferred** | same: no audio ML stack installed |
| Lip-reading / transcription | **Deferred** | no offline model with a compatible license present |

Real face/pose/lip/voice inference is deferred with a precise reason: no OpenCV,
ONNX Runtime, MediaPipe, torch, or cv2 is installed on this 8 GB host, and the
project policy forbids pulling a large ML stack without a licensing/build/memory
review. The statistical observation/fusion framework that *would consume* those
detectors is implemented and tested now, so a future detector plugs straight in.

**Measured performance** (this host, `-O2`, Release-equivalent):
`SyntheticActivityDetector::detect` ≈ 0.84 µs/op (~1.19M ops/sec);
`AnomalyDetector::record` ≈ 0.84 µs/op (~1.19M ops/sec); peak RSS ≈ 4 MB. These
are sub-microsecond, no-ML operations that fit the low-memory Android device
budget with no model download.

**Tests:** `tests/core/test_detection.cpp` — 20 cases / 67 assertions covering
valid input, malformed/oversized payloads, unavailable/inconclusive/conflicting
inputs, stale windows, bounded keys/windows, no-raw-media-in-payload, and Event
pipeline integration. Full suite: **275 cases / 2071 assertions**, CTest 100%,
Node 76/76.

## Privacy audit log (added 2026-10-13, roadmap H-03)

The consent gate is a *pure* decision function (no side effects). The privacy
audit is therefore a **separate passive observer**, so the gate stays pure and
the audit can never alter a decision:

```
 data-access attempt (acquisition / per-consumer delivery)
        |
        v
   ConsentGate::check()  ──►  GateDecision { authorized, reason }
        |                              |
        |                              v
        |                    PrivacyAuditLog::record(entry)      (passive, best-effort)
        |                              |
        v                              v
   payload produced?          bounded ring buffer (≤ capacity)
   (only if authorized)       counters: total / authorized / denied / acquired
                                       |
                                       v
                              verify_enforcement(): no entry may
                              record an acquisition that was not
                              authorized (H-03 invariant)
```

**PrivacyAuditLog** (`src/core/PrivacyAuditLog.{h,cpp}`): a bounded (`capacity`,
default 256), thread-safe (`std::mutex`) log of consent-gated data-access
decisions. Each `PrivacyAuditEntry` carries only descriptive metadata —
timestamp, subject scope, purpose, data category, the gate decision, the deny
reason, and whether a payload was acquired. String fields are clamped to
`kMaxFieldChars` (128) so a malformed caller cannot bloat an entry. `record()`
is best-effort and never throws, so a logging failure can never widen access or
crash the runtime.

**Integration** (non-owning, optional pointers; null = no-op):
- `ConsentGatedAcquisition::set_privacy_audit(log)` — records every `acquire()`
  outcome (denied, authorized-but-empty, acquired).
- `SimulatorRuntime::set_privacy_audit(log)` — records every per-consumer
  delivery decision in `tick()`.

**Consent enforcement verified:** `verify_enforcement()` returns false if any
retained entry records an acquisition without an authorizing decision — the
operational form of "consent enforcement verified". A test injects a bogus
entry to prove the check is real, not a tautology.

**Demo surface:** `run_demo_scenario` reports a `privacy_audit` block
(`entries / authorized / denied / acquired / enforcement_ok / capacity`); the web
UI renders it. Capture-off reports a zeroed, enforcement-OK trail (no data
access was attempted — fail-closed).

**Not persisted to disk by this component:** the audit log is in-memory and
bounded; the structured logger still emits a durable record. Durable DB
persistence of audit entries is a deliberate follow-up.

**Tests:** `tests/core/test_privacy_audit.cpp` — 11 cases (authorized/denied
logging, reasons, bounded capacity, field clamping, the enforcement invariant
including a negative case, clear, concurrency, the passive-observer guarantee,
and two integration cases against `SimulatorRuntime` and
`ConsentGatedAcquisition`).
