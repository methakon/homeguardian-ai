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
