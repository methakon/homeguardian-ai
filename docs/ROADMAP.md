# HomeGuardian AI — Implementation Roadmap

## Phase A — Orchestration and Repository Foundation

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| A-01 | Verify Antigravity delegation workflow | — | `agy --conversation` returns `SESSION_OK` | DONE |
| A-02 | Initialize Git repository | A-01 | Repo exists at `/home/swarna-sekhar-dhar/projects/homeguardian-ai` on `main` branch | DONE |
| A-03 | Create initial documentation | A-02 | README, ARCHITECTURE, ROADMAP, DEVELOPMENT, TESTING, ORCHESTRATION, DECISIONS, STATUS all exist and are committed | DONE |
| A-04 | Review and first commit | A-03 | All docs reviewed, `.gitignore` present, initial commit created | DONE |

## Phase B — Minimal Vertical Slice

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| B-01 | CMake build system | A-04 | `cmake` configures and `make` builds an empty executable | TODO |
| B-02 | Application entry point | B-01 | `main()` starts, initializes logging, loads config, enters main loop | TODO |
| B-03 | Structured logging | B-01 | spdlog initialized with configurable level and output | TODO |
| B-04 | Configuration loading | B-02 | JSON config loaded and validated; defaults applied for missing values | TODO |
| B-05 | Graceful shutdown | B-02 | SIGINT/SIGTERM handled; resources cleaned up; clean exit | TODO |
| B-06 | Initial test suite | B-01–B-05 | Catch2 tests pass for all above modules | TODO |

## Phase C — Event Model and Pipeline

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| C-01 | Core event types | B-06 | Event, Scene, Alert types defined; serialization tested | TODO |
| C-02 | Event ingestion interface | C-01 | IEventSource interface defined | TODO |
| C-03 | Event processing pipeline | C-01, C-02 | Pipeline stages: ingest → correlate → infer → route | TODO |
| C-04 | State manager | C-03 | Tracks occupancy, known faces, recent events | TODO |
| C-05 | Consent manager | C-03 | Per-zone, per-activity consent enforcement | TODO |

## Phase D — Persistence

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| D-01 | SQLite persistence | C-03 | Events, faces, consent, config stored and queried | TODO |
| D-02 | Retention policies | D-01 | Automatic data expiration per configurable policies | TODO |

## Phase E — Sensing and Inference

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| E-01 | Simulated event source | C-03 | Generates synthetic events for testing | TODO |
| E-02 | Camera interface + OpenCV | C-03 | Capture frames from video source | TODO |
| E-03 | Face analysis (OpenCV DNN) | E-02 | Face detection and emotion estimation | TODO |
| E-04 | Audio capture + voice analysis | C-03 | Microphone input with VAD | TODO |
| E-05 | ONNX Runtime integration | C-03 | Load and run ONNX models | TODO |

## Phase F — HTTP and MCP

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F-01 | HTTP server (Boost.Beast) | C-03 | REST API for dashboard and alerts | TODO |
| F-02 | MCP Streamable HTTP server | F-01 | MCP protocol implemented and tested | TODO |
| F-03 | Alexa+ integration | F-02 | Alexa+ can invoke HomeGuardian tools | TODO |

## Phase G — Dashboard and Notifications

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| G-01 | Web dashboard | F-01 | HTML/CSS/JS dashboard shows events and status | TODO |
| G-02 | Notification service | C-03 | Email/push/webhook alerts | TODO |
| G-03 | Ring simulator | C-03 | Simulated Ring events for development | TODO |

## Phase H — Safety and Hardening

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| H-01 | Fall detection pipeline | E-02, E-03 | Pose-based fall detection with configurable sensitivity | TODO |
| H-02 | Emergency workflow | G-02 | Configurable escalation: notify → call → emergency services | TODO |
| H-03 | Privacy audit | D-01 | All data access logged; consent enforcement verified | TODO |
| H-04 | Security review | All | Input validation, injection prevention, access control | TODO |
