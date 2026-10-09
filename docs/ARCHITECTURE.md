# HomeGuardian AI — Architecture

## Overview

HomeGuardian AI follows a layered, event-driven architecture with clear
separation between sensing, inference, event processing, and presentation.
The core is a single native C++ process with well-defined interfaces for
all external interactions.

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

## Event Flow

1. A sensor or AI model emits a **raw event** (timestamp, source, type,
   payload, confidence).
2. The **ingestion** stage validates and normalizes the event.
3. The **correlation** stage groups related events (e.g., doorbell ring +
   face detection + motion) into a **scene**.
4. The **inference** stage applies rules and ML models to the scene.
5. The **routing** stage dispatches outputs: alerts, dashboard updates,
   notifications, or actions.

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
