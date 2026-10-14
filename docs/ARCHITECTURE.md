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
an active condition. The alert is a synthetic, **detection-driven** activity
notice (L-11: it is raised only when the fused detector evidence qualifies),
never a real-world danger detection.

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
pipeline integration. Full suite at that milestone: **275 cases / 2071
assertions**, CTest 100%, Node 76/76 (current totals are recorded in the later
sections of this document and in `docs/STATUS.md`).

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

## Native demo transport: one C++ process, no Node (added 2026-10-13, v14)

```
 (legacy)  browser ─► node http server ─► spawn hg_alexa_bridge  (NDJSON) ⟶ 2+ procs
 (v14)     browser ─► hg_alexa_bridge --http …  ─► Application / AlexaContract /
                        │ (embedded dashboard)      PersistenceAudit / Pipeline   ⟶ 1 proc
                        └─ NativeHttp.h (loopback-only, GET-only subset)
```

`NativeHttp.h` is a small, bounded, loopback-only HTTP/1.1 subset (no external
dependency). `hg_alexa_bridge --http <config> <creds> [--port N] [--token T]`
serves:

- `/` and `/index.html` — the dashboard, **embedded** at build time from
  `src/web/index.html` via CMake `configure_file` → `generated/hg_web_assets.h`
  (no runtime web file, no request-time filesystem path);
- `/api/health` — static `{ok,up,simulated}`;
- `/api/status` — real `AlexaContract::handle(status|alert_summary)` over the
  live `Application` + SQLite;
- `/api/run-scenario?cycles=N` — the real `run_demo_scenario` (simulator →
  pipeline → correlation → alert policy → persistence), bounded to N≤100.

The NDJSON stdio mode is unchanged and still used by the Node/integration tests.
The dashboard is vanilla HTML/CSS/JS (unchanged); the browser talks only to the
loopback JSON API.

**Per-run semantics.** A single long-lived process serves repeated runs, so
`run_demo_scenario` resets per-run state at entry (`RuntimeStatus` counters,
`AlertPolicy`, pipeline history, and `SimulatorRuntime::reset_for_reuse()`), and
`grant_consent` is idempotent, so each run reproduces the legacy fresh-process
behaviour. See `docs/DECISIONS.md` (D-native-http) and `docs/FRICTION_LOG.md`
(v14) for the defects this surfaced.

**Security boundary.** Loopback-only bind by construction; GET-only; `Host` must
be a loopback authority (DNS-rebinding defence); non-loopback `Origin` rejected
(cross-site defence); no bodies/framing headers; CSP + frame/referrer headers on
HTML; bounded sizes (8192 B request, 128 B path) and 3 s client timeouts; one
request at a time. Details and the simulator-vs-production distinction:
`docs/SECURITY_READINESS_V8.md`.

## v17 — Scenario catalogue, runner and explainable alerts (2026-10-13)

    DemoScenario (core/DemoScenarios)            data only: cycles, consent, energies
            |
            v
    run_demo_scenario(Application&, spec)        core/DemoScenarioRunner
            |
            +--> SimulatorRuntime: reset counters + CONSENT STATE, grant/withdraw,
            |       start, N x tick()  -- each AUTHORIZED observation -> event sink
            |
            +--> Application::process_event()    (the EXISTING path)
            |       -> Pipeline (validation + correlation)
            |       -> AlertPolicy (dedup/cooldown)
            |       -> delivered alert -> Application alert sink (passive observer)
            |
            +--> SyntheticActivityDetector + FusionEngine (detection stage)
            |       observations -> to_event() -> same pipeline (inference events)
            |
            v
    ScenarioRunResult  ->  persist_run_alerts(PersistenceAudit&, result)
                                writes EXACTLY the delivered alerts (real output)

- `Application::set_alert_sink()` is a passive observer invoked only for
  DELIVERED alerts. It cannot change delivery, ordering, or persistence.
- `SimulatorRuntime::reset_consent_state()` clears only the synthetic in-memory
  consent repo; it never touches durable state.
- HTTP: `GET /api/run-scenario[?scenario=<id>][&cycles=N]`, `GET /api/alerts`,
  `GET /api/scenarios`, plus the existing `/`, `/index.html`, `/api/health`,
  `/api/status`. All loopback-only, GET-only, with the H-04 protections.
- The dashboard is vanilla HTML/CSS/JS, embedded in the binary; it renders
  backend-derived values using `textContent` / `createElement` (never `innerHTML`
  with backend strings).

## L-11 — Detection-driven alert path (2026-10-13)

The alert is now CAUSED by the detection/fusion evidence. The raw-observation
`sim-presence` rule is gone.

    SimulatorRuntime::tick()  (one shared synthetic payload)
        |
        |  per consumer: ConsentGate decision  (authorized -> delivered)
        v
    authorized synthetic Observation event  --->  Application::process_event()
        |                                                 |
        | (the tick delivered media)                      v
        v                                          Pipeline (validation +
    SyntheticActivityDetector                       recent-event window)
        |                                                 |
        v                                                 v
    Observation (inference event)  ----------->  DetectionFusionRule.evaluate(window)
                                                          |
                                        Observation::from_event() rebuilds the
                                        detection evidence from the window
                                                          |
                                                          v
                                                    FusionEngine.fuse()
                                                          |
                        not_detected / inconclusive / unavailable / conflict
                                                          |
                                                  (no alert returned)
                                        detected AND confidence >= floor AND
                                        >= 2 usable votes
                                                          |
                                                          v
                                   Alert {severity from confidence, rule
                                   "detection-fusion", reason/evidence in details}
                                                          |
                                                          v
                                     AlertPolicy (dedup/cooldown by rule+severity)
                                                          |
                                                          v
                                     Application alert sink -> PersistenceAudit
                                                          |
                                                          v
                                     /api/alerts  +  embedded dashboard

Key properties:
- Detection runs ONLY after an authorized acquisition. Capture off, consent
  withdrawn, or no authorized consumer => `tick()` delivers nothing => no
  detection step => no detection-driven alert.
- Raw observations never raise alerts; they are the detector's input.
- Alert severity is derived from the fused confidence (>= 0.8 -> warning, else
  info). The system never emits `critical`, and makes no fall/medical claim.
- `Observation::from_event()` is the round-trip that lets the rule reuse the SAME
  evidence the pipeline carried — no parallel detection state.
- Everything the API/dashboard shows (verdict, confidence, contributing count,
  provenance, reason) comes from the alert's own `details`, i.e. the real
  evidence.

## L-12 — Statistical anomaly stage (2026-10-13)

    authorized synthetic observation (one per tick that delivered media)
        |
        +--> SyntheticActivityDetector  -> Observation (activity-energy)      [detection]
        |
        +--> AnomalyMonitor             -> Observation (activity-anomaly)     [L-12]
                |
                |  key: "activity-energy"; rolling mean/stddev (bounded window)
                |  warm-up (< min_samples) -> inconclusive   (never a vote)
                |  within baseline          -> not_detected
                |  high / low deviation     -> detected      (the detector's
                |                              target IS "an anomaly")
                |  deliberate gap probe     -> inconclusive + status "missing"
                v
        Application::process_event -> Pipeline (validation + bounded history)
                |
                v
        DetectionFusionRule.evaluate(window)
                |
                |  EVIDENCE ALLOWLIST (L-12): only detector ids on
                |  cfg.evidence_detector_ids are reconstructed as evidence.
                |  Default = {"activity-energy"} -> "activity-anomaly" is
                |  ADVISORY: reported, audited, persisted, but never a vote.
                |  Adding "activity-anomaly" makes its influence explicit and
                |  causal (unit-tested in both directions).
                v
        AlertPolicy -> alert sink -> SQLite -> /api/alerts + dashboard

Properties:
- One detection step and one anomaly step per AUTHORIZED acquisition. No
  acquisition (capture off / consent withdrawn / no authorized consumer) => no
  observation, no anomaly sample, no alert.
- The baseline is reset per scenario run, so repeats are deterministic.
- Anomaly results are bounded and non-sensitive: feature value, mean, stddev,
  z-score, counts and a short reason. No media, identities or profiles.
- A statistical deviation is NOT a fall, emergency, medical or danger claim; the
  alert message and rule are unchanged (L-11 semantics preserved).


## H-01s — Synthetic impact prioritisation (2026-10-13)

H-01s adds one bounded stage and one prioritisation change. It is a SIMULATION
demonstration of severity prioritisation; it is **not** fall detection.

```
 authorized acquisition (SimulatorRuntime)
   +--> SyntheticActivityDetector -> Observation (activity-energy)      [L-01]
   +--> AnomalyMonitor            -> Observation (activity-anomaly)     [L-12]
   +--> SyntheticImpactClassifier -> Observation (impact-signal)        [H-01s]
              |
              v
        Pipeline (history -> time-window correlation)
              |  DetectionFusionRule    (activity evidence -> info/warning)
              |  SyntheticImpactRule    (confirmed impact -> CRITICAL)
              v  highest severity wins (info < warning < critical)       [H-01s]
        AlertPolicy (dedup + cooldown, keyed rule+severity)
              v
        SQLite -> Native HTTP API -> dashboard
```

**`SyntheticImpactClassifier`** (`src/core/ImpactClassifier.{h,cpp}`): deterministic
two-phase pattern over a bounded feature in [0,1].

| Sample | Condition | Result |
|--------|-----------|--------|
| transient | intensity >= 0.80 | `inconclusive`, phase `transient` — NOT a confirmation |
| stillness | <= 0.25 within 4 samples of a transient | `detected`, phase `confirmed` — the pattern that H-01s acts on |
| expired | transient with no stillness inside the window | `not_detected`, phase `expired`, with the reason |
| otherwise | — | `not_detected` |

**`SyntheticImpactRule`** (`src/core/SyntheticImpactRule.{h,cpp}`): reads only
`impact-signal` observations reconstructed from the window; requires a confirmed
observation AND >= 2 supporting impact observations; maps a confirmed pattern to
`Severity::critical` and puts the bounded evidence in `details`
(`impact_pattern`, `transient_intensity`, `stillness_intensity`,
`samples_since_transient`, `supporting_observations`, `provenance`, `simulated`,
`emergency_services_contacted: false`). It is the only source of critical alerts
in the build.

**Severity prioritisation.** `Pipeline::process` evaluates every registered rule
and returns the highest severity, so a critical impact alert is never lost behind
an activity alert. Activity evidence and impact evidence therefore both appear in
the critical scenario (1 CRITICAL `synthetic-impact` + 1 INFO `detection-fusion`),
and neither suppresses the other.


## v21 — HTTP status contract, retention and failure handling (2026-10-13)

**The handler owns the business status.** `hg_native_http::Handler` is
`std::function<std::string(const HandlerRequest&, int& status_out)>`: the handler
returns a JSON body and reports the HTTP status for the outcome it decided, while
the transport keeps owning framing, header and query validation. An unsupported
but well-formed value (an unknown scenario id) is therefore `400 Bad Request` with
`{"ok":false,"error":"unknown_scenario","scenario_id":"…"}` instead of a
`200`-with-`ok:false`. Transport rejections (413 oversized, 403 Host/Origin, 405
method, 404 path, 400 malformed/query) never reach the handler and cannot be
overridden by it. The dashboard reads `scenario.ok`, so the change is
client-compatible.

**The bridge's durable alert store is bounded.** `hg_alexa_bridge` writes alerts to
its own audit DB (`homeguardian_bridge_audit.db`) and now applies the config's
`alert_retention_days` to it: once at startup and, at most once per interval,
after a scenario write (`PersistenceAudit::maybe_prune_retention`).
`alert_retention_days = 0` keeps records forever. Durability (WAL +
`synchronous=FULL`) and the alert record shape are unchanged.

**Logging is diagnostic, never fatal.** `Logger::initialize` creates the
`homeguardian.log` file sink on a best-effort basis: if the working directory is
not writable it logs to the console only and reports the reason. It previously
threw `spdlog::spdlog_ex` out of `main()`, so the shipped binaries died with
SIGABRT instead of starting degraded.

**Degraded mode is explicit.** If the audit database cannot be opened (unwritable
directory, corrupt file), the bridge keeps serving: `/api/status` answers
`{"error":"unavailable","ok":false}`, read routes fail closed, a scenario still
produces real detection evidence but claims **no** persistence. Nothing is
fabricated to mask the degraded state.


## Distributed target architecture (owner-directed, 2026-10-13)

The simulator-first product (v21) remains intact and is the hackathon fallback.
The target product adds a distributed topology around the SAME core:

```
  Android node(s)                        Docker host
  ┌──────────────────────┐               ┌───────────────────────────────────┐
  │ Camera1/Camera2       │   D-03        │ hub container (non-root, caps off)│
  │ AAudio microphone     │  authenticated│  ┌─────────────────────────────┐  │
  │ ConsentGuardedDevice  │◄──encrypted──►│  │ hg_alexa_bridge (C++20)     │  │
  │ IMediaDevice adapters │   LAN only    │  │  consent gate + audit       │  │
  │ explicit start/stop   │               │  │  observation → detection     │  │
  │ permission lifecycle  │               │  │  → fusion → AlertPolicy      │  │
  └──────────────────────┘               │  │  → SQLite (WAL, FULL)        │  │
                                          │  └─────────────────────────────┘  │
                                          │   /data volume (durable)          │
                                          │   127.0.0.1 only, NOT published    │
                                          └───────────────────────────────────┘
```

Design rules that hold across the D-series:

- **One event engine.** Nodes produce observations; the hub's existing pipeline
  processes them. No node runs detection/fusion/alerting.
- **Consent first.** A physically connected phone is not permission to record:
  acquisition needs an explicit user action + OS permission + a consent record,
  and every path fails closed.
- **Loopback by default.** The hub's all-in-one HTTP server stays
  `127.0.0.1`-only. A network-facing boundary is D-03 (authenticated, encrypted,
  separately reviewed) — never a listener-address change.
- **Derived observations by default.** Compact features (motion energy, audio
  RMS, availability) are the default payload; raw media only when specifically
  authorised.

### D-01 container shape

Multi-stage: a builder that compiles the hub, a `test` stage that runs the
existing suite with the container toolchain, and a minimal runtime image running
as uid 10001 with all capabilities dropped. `/data` is the only writable path and
is a named volume, so container replacement preserves alert history. The
entrypoint `exec`s the hub, making it PID 1 for clean SIGTERM.

Builder and runtime use the **same** Debian release (trixie-slim): the project
requires CMake >= 3.28.3 (bookworm ships 3.25.1 and cannot configure), and a
binary built against a newer glibc than the runtime provides would not start.


## D-02 — Android acquisition node (2026-10-13)

```
  Android node (apk/)                                  native contracts (src/core/)
  ┌──────────────────────────────────────────┐         ┌───────────────────────────┐
  │ MainActivity  (identity, consent,        │         │ CaptureAccounting         │
  │   facing/size/FPS, rate/channels, REC    │  JNI    │  bounded + authorized +    │
  │   banner, live counters)                 │◄───────►│  timestamped payload       │
  │ Camera1Bridge  (preview frames)          │  12     │  contract, saturating      │
  │ AAudioCaptureDevice (PCM buffers)        │  entry  │  counters, drop reasons    │
  │ DeviceIdentity (Android ID → opaque id)  │  points │ NodeIdentity (node_id)     │
  └──────────────────────────────────────────┘         └───────────────────────────┘
             │ per-frame / per-buffer                      │ every payload
             ▼                                             ▼
        ConsentGuardedDevice ── authorize_delivery() ── ConsentGate (granted/
        (IMediaDevice lifecycle)                          withdrawn/expired,
                                                          capture enabled)
```

Design rules this milestone adds:

- **One accountant, two driver threads.** Camera1 frames (Java callback thread)
  and AAudio PCM buffers (driver thread) both go through the same
  `CaptureAccountant`, which validates dimensions/size/rate/channels/timestamps,
  refuses unauthorized payloads with a counted reason, and returns a counter
  *copy* to the UI so nothing observes half-updated state.
- **Authorization is per payload, not per session.** The camera path calls the
  full gate for every frame; the audio path calls the full gate from its data
  callback for every buffer. A withdrawal or permission revocation therefore
  stops delivery at the next payload, and the audio path additionally raises a
  stop request that the control thread turns into a real stop+release.
- **The client is never trusted to gate itself.** `CaptureAccounting` is a core,
  host-tested component: the same code that enforces limits on the phone is the
  code the host tests drive, so the contract is verified without hardware.
- **No background capture exists** in this milestone: no foreground service, and
  leaving the activity stops every device.


## Skills-integration layer (2026-10-14, D-02x)

Three new capability groups, all pure C++20 with no added dependency, all bounded:

```
 permitted PCM ──► extract_audio_features ──► ActivityChangeDetector (level/trend)
 permitted luma ─► extract_motion_features ─► ActivityChangeDetector (motion)
                                          └─► AnomalyMonitor  (z-score; EXISTING)
 any bounded vector ─────────────────────────► VectorDriftDetector (cosine regime shift)
                                                        │
                            bps (motion / audio / environmental)
                                                        ▼
                                        DempsterShafer::combine  (conflict K)
                                                        ▼
                                       PrecursorSuite::fusion  -> result vocabulary
                                                        ▼
 authorized environmental sample ──► EnvironmentalAdapter ──► CorrelationWindow
 node / Home Assistant / fixture ──► SecurityObservation ──► to_observation()
                                                        ▼
                                    Observation::to_event()  (EXISTING conversion)
                                                        ▼
                          EXISTING Pipeline -> rules -> AlertPolicy -> SQLite -> HTTP
```

Design rules added by this layer:

- **The fusion frame cannot express a calamity.** `Hypothesis` has exactly three members
  (`routine`, `activity_change`, `environmental_anomaly`) and a test asserts the count, so a
  seismic/flood "precursor" hypothesis is unrepresentable rather than merely discouraged.
- **Ignorance is first-class.** Warming up is expressed as a vacuous mass (the detector does not
  vote), and `belief`/`plausibility`/`ignorance` are reported together so the reader sees the
  interval the fusion is working with.
- **Contradiction suppresses.** `K > 0.9` leaves the combination un-normalised and reports
  `suppressed`; averaging across disagreement is exactly what the skill warns against.
- **Availability is a transition, not a heartbeat.** `DeviceAvailabilityTracker` is silent while
  a device is healthy and emits one transition per state change, so a dead sensor stays
  distinguishable from a silent one; a reconnect is an availability observation and never a
  state replay.
- **The hub talks to Home Assistant read-only.** `HttpClient` is GET-only over plaintext with an
  explicit opt-in gate; `HomeAssistantAdapter` refuses to run disabled, misconfigured or
  unauthorized, and every mapped observation carries real provenance plus consent context.
- **Advisory by default.** New detector ids are not in `DetectionFusionRule`'s default evidence
  allowlist, so the historical scenario behaviour is unchanged and the opt-in is explicit.


## Dashboard status surface (2026-10-14)

The loopback bridge now serves `/api/integrations` in addition to `/api/health`, `/api/status`,
`/api/alerts`, `/api/scenarios` and `/api/run-scenario`. It is read-only and changes no state, and
every value in it is derived: the Home Assistant block from `HomeAssistantConfig::problems()`, the
sensor-device count from a real `DeviceAvailabilityTracker`, the result vocabulary from the
detection layer's closed label set. The page renders it with the three provenance labels
(LIVE SENSOR DATA / SYNTHETIC TEST DATA / HYPOTHESIS / UNVALIDATED CORRELATION) and a per-label
legend, and states the never-produced claims in the page copy rather than in the API, so the
payload cannot carry one. Per-skill detail: `docs/SKILL_VALIDATION_REPORT.md`,
`docs/ANIMAL_ACTIVITY_DETECTION.md`, `docs/HOME_SECURITY_SENSOR_INTEGRATION.md`,
`docs/HOME_ASSISTANT_INTEGRATION.md`.


## D-03 — Device ingress boundary

The hub now has TWO listeners with deliberately different trust levels, and the reason they are
separate processes is that they were reviewed against different threat models.

```
Android node (Apache-2.0 APK)                       Docker host (LAN only)
+-----------------------------+                    +---------------------------------------------+
| operator START              |                    |  ingress container (non-root, caps dropped)  |
| OS camera/mic permission    |                    |  :8443/tcp  <-- THE ONLY PUBLISHED PORT      |
| HomeGuardian consent gate   |                    |   1. TLS handshake (private CA, client cert  |
| per-frame consent re-check  |   framed, versioned |      required; TLS 1.2 minimum)             |
| bounded feature extraction  |   compact observation 2. TLS peer identity -> CN = device id    |
| bounded queue (128, drop-oldest) ------------------> 3. fingerprint == registry credential_id |
| ObservationTransport (TLS)  |                    |   4. registry: paired / revoked / consent /  |
+-----------------------------+                    |      capture enabled / capabilities          |
                                                   |   5. bounds, timestamp window, sequence      |
                                                   |      watermark, duplicate window, allowlists |
                                                   |   6. sequence watermark COMMITTED last       |
                                                   |   7. Observation -> IngestDetectors (per     |
                                                   |      device) -> Pipeline -> FusionRule ->    |
                                                   |      AlertPolicy -> SQLite (WAL, FULL)       |
                                                   +---------------------------------------------+
                                                                       |
        loopback only, NOT published          +--------------------------v------------------+
        +------------------------------------>|  hub container: dashboard + demo server  |
        |  (docker-compose.loopback.yml binds  |  127.0.0.1 inside the container only     |
        |   the host side to 127.0.0.1)        +-----------------------------------------+
```

Why the ingress is not simply the existing server on `0.0.0.0`:

- The existing server is a handwritten HTTP server reviewed for loopback. Changing its bind address
  would put its full surface — dashboard, API, JSON, SQLite-backed reads — on the Wi-Fi, which is a
  different security problem than "accept compact observations from authenticated devices".
- The ingress therefore speaks ONE protocol, exposes NO dashboard or command channel, and never
  opens SQLite for requests. A device cannot read alerts, list other devices, or invoke any command.
- The two containers share the image and the security posture (non-root, all capabilities dropped,
  `no-new-privileges`, no Docker socket, no privileged mode) but not the port, the data volume or
  the trust level.

Trust chain, in the order the hub checks it:

1. **Is the connection legitimate?** TLS with a client certificate that chains to the private CA,
   `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` set, verify result checked explicitly. No certificate, no
   conversation.
2. **Who is it?** The certificate CN is the claimed device id; the SHA-256 fingerprint of that
   certificate must equal the `credential_id` recorded for that device. Both must hold.
3. **May it be here?** Registry state: paired, not revoked, presence in the registry.
4. **May it measure?** Consent granted for that device, capture enabled for that device, and the
   source type in the device's capability allowlist.
5. **Is the data plausible?** Bounded numeric features, timestamp inside the window, sequence
   strictly ahead of the watermark, observation id unseen, queue not full.
6. **Only then** is provenance derived, the watermark committed, and the observation handed to the
   existing pipeline.
7. **The pipeline's output lands where the hub can serve it.** The ingress and the hub share one
   persistent volume and one SQLite database, so an alert raised from an ingested observation appears
   in the dashboard and its API rather than only inside the ingress. Both processes use WAL with a
   local filesystem and a 5 s busy timeout. Whether ingested evidence may VOTE in fusion is an
   explicit operator setting (`evidence_detector_ids`); by default it is recorded and displayed but
   casts no vote, and nothing on this path can reach `Severity::critical` (ADR-014).

## Ingress authorization and replay flow (D-03.1)

```
device (mTLS, cert CN = device id)
   |  hello
   v
[connection setup]  chain + endpoint identity verified
   |  welcome { device_id, last_sequence }        <- the device's resume point
   |  observation -----------------------------.
   v                                           |
[per-observation]                               |
   refresh_authorization_locked()               |  stat registry/revocation, re-read on
     - registry: paired/revoked/consent/capture |  change or on a 250 ms deadline
     - revocation list                          |
   authorization fault? -> refuse (unauthorized, fail closed)
   else ingest.accept():                        |
     validate -> commit hook (ReplayState,      |
       atomic write BEFORE delivery)            |
     -> sink / detectors / fusion -> alert      |
   ack { observation_id, sequence, accepted, reason }
```

Two rules the diagram encodes:

1. **Authorization is evaluated where the observation is delivered**, not only where the
   connection was accepted. A revocation therefore reaches an open connection.
2. **Durable state is written before delivery.** A crash between the two cannot make an
   accepted observation replayable, and a failure before delivery is `processing_failed`,
   never an acceptance.

## Ingress processing and replay flow (D-03.2)

```
device (mTLS, cert CN = device id)
   |  welcome { device_id, last_sequence }        <- the device's resume point
   |  observation ------------------------------.
   v                                           |
[validate] authorization (live) + bounds + values
   |  repeat of the last COMPLETED observation? --> answer it, DO NOT reprocess
   |  sequence behind the watermark?           --> replayed_sequence
   v
[begin]  in memory: this attempt is IN FLIGHT (guards this process only)
   |
[process] sink -> detectors -> fusion -> alert  <-- throws => abandon + processing_failed
   |
[complete] DURABLE: the one write, watermark advances <-- fails => processing_failed (no false ack)
   |
   ack { observation_id, sequence, accepted, reason }
```

Two rules the diagram encodes, both of which were wrong before D-03.2:

1. **The durable watermark may only move AFTER processing succeeded.** Recording first made
   a failure indistinguishable from success in durable state, so the retry the hub asked for
   was refused as a replay.
2. **A duplicate answer must never re-run the work.** An unacknowledged observation is
   answered from the record; only genuinely new sequences are processed.

## Local voice feedback (H-02A)

The spoken-response layer sits between the existing finding and the existing output, adding no
second audio stack:

    AudioActivitySession finding
      -> VoicePhrasebook        (pure policy: closed vocabulary, gate, interval, cooldown)
      -> VoiceResponseController (commits only on a real hand-off; owns the suppression window)
      -> IVoiceOutput -> LocalSpeaker (fork+execvp, bounded queue, worker thread) -> PipeWire sink

Rules the layer holds: the sentence set is closed at compile time and asserted by test; the
spoken text travels as exactly one argv element so no detector or payload string can reach a
shell; a decision is not a commitment (a refused delivery consumes nothing); the feedback window
is bounded and can only be shortened, so environmental monitoring always resumes. An officially
supported Alexa adapter could be added behind the same `IVoiceOutput`/phrasebook seam without
touching the detection engine.
