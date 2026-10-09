# HomeGuardian AI — Implementation Roadmap

## Phase A — Orchestration and Repository Foundation

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| A-01 | Initialize project and verify toolchain | — | C++20, CMake, spdlog, nlohmann/json, Catch2 all working | DONE |
| A-02 | Initialize Git repository | A-01 | Repo exists at `/home/swarna-sekhar-dhar/projects/homeguardian-ai` on `main` branch | DONE |
| A-03 | Create initial documentation | A-02 | README, ARCHITECTURE, ROADMAP, DEVELOPMENT, TESTING, ORCHESTRATION, DECISIONS, STATUS all exist and are committed | DONE |
| A-04 | Review and first commit | A-03 | All docs reviewed, `.gitignore` present, initial commit created | DONE |
| A-05 | Create skill inventory | A-03 | `docs/SKILL_INVENTORY.md` created with all relevant skills documented | DONE |
| A-06 | Update architecture for three-tier design | A-03 | `docs/ARCHITECTURE.md` updated with Tier 1/2/3 descriptions | DONE |

## Phase B — Minimal Vertical Slice (Tier 2 — PC-Side C++)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| B-01 | CMake build system | A-06 | `cmake` configures and `make` builds an empty executable | DONE |
| B-02 | Application entry point | B-01 | `main()` starts, initializes logging, loads config, enters main loop | DONE |
| B-03 | Structured logging | B-01 | spdlog initialized with configurable level and output | DONE |
| B-04 | Configuration loading | B-02 | JSON config loaded and validated; defaults applied for missing values | DONE |
| B-05 | Graceful shutdown | B-02 | SIGINT/SIGTERM handled; resources cleaned up; clean exit | DONE |
| B-06 | Initial test suite | B-01–B-05 | Catch2 tests pass for all above modules | DONE |

## Phase C — Event Model and Pipeline

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| C-01 | Core event types | B-06 | Event, Alert types defined; JSON serialization tested | DONE |
| C-02 | Event ingestion interface | C-01 | IEventSource interface + SimulatedEventSource implemented | DONE |
| C-03 | Event processing pipeline | C-01, C-02 | Pipeline: validate → normalize → correlate → alert | DONE |
| C-04 | Correlation rules | C-03 | TimeWindowCorrelation with configurable window | DONE |
| C-05 | Event validation | C-01 | EventValidator with deterministic normalization | DONE |
| C-06 | Config extension | C-03 | max_event_history, correlation_window_ms, alert_confidence_threshold | DONE |
| C-07 | Application integration | C-03 | Application::process_event() delegates to Pipeline | DONE |
| C-08 | Test suite | C-01–C-07 | 13 test cases covering all modules | DONE |

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
