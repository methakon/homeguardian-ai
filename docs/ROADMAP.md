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
| D-01 | SQLite persistence | C-03 | Events, alerts, profiles, consent, routines stored and queried | DONE |
| D-02 | Retention policies | D-01 | Automatic data expiration per configurable policies | DONE |

## Phase E — Family Profiles, Consent, Routines and Configuration

This phase establishes a privacy-first, testable foundation before any camera,
audio, or heavier AI inference is introduced. It is deliberately separate from
the earlier "Sensing and Inference" plan, which is deferred to a later phase.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| E-01 | Family profile model | D-01 | Versioned profile with opaque ID, display name, optional age band; no sensitive attributes | DONE |
| E-02 | Consent model | D-01 | Explicit granted/denied/withdrawn states; default-deny; expiry; withdrawal blocks future processing | DONE |
| E-03 | Routine model | D-01 | Versioned routine with validated schedule and time zone; configuration only | DONE |
| E-04 | Persistence + migration | D-01 | Additive v1→v2 schema migration; FK cascade; transactional and rerunnable | DONE |
| E-05 | Configuration | D-01 | Validated Phase E config; media capture and cloud OFF by default | DONE |
| E-06 | Retention/export/deletion | D-01 | Documented lifecycle; profile deletion cascades to dependents; no media stored | DONE |
| E-07 | Tests | E-01–E-06 | Profiles, consent states, routines, restart recovery, FK/uniqueness, rollback, config | DONE |

## Phase E2 — Sensing and Inference (deferred)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| E2-01 | Simulated event source | C-03 | Generates synthetic events for testing | TODO |
| E2-02 | Camera interface + OpenCV | C-03 | Capture frames from video source | TODO |
| E2-03 | Face analysis (OpenCV DNN) | E2-02 | Face detection and emotion estimation | TODO |
| E2-04 | Audio capture + voice analysis | C-03 | Microphone input with VAD | TODO |
| E2-05 | ONNX Runtime integration | C-03 | Load and run ONNX models | TODO |

## Phase F1 — Simulated Sensing and Consent-Gated Inference

Establishes the consent gate and a simulated sensor path before any real
hardware. No real camera, microphone, biometric, or cloud processing.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F1-01 | Simulated sensor source | E-07 | Deterministic synthetic events; tagged synthetic; configurable timestamps; malformed input; disconnect/reconnect | DONE |
| F1-02 | Consent gate | E-02 | Single fail-closed decision point; checks profile, purpose, category, grant, expiry, withdrawal | DONE |
| F1-03 | Consent-gated acquisition | F1-01, F1-02 | Denied requests produce no payload; authorized requests acquire; synthetic only | DONE |
| F1-04 | Tests | F1-01–F1-03 | Granted/denied/missing/expired/withdrawn/unknown/mismatched; timestamps; malformed; reconnect; pipeline regression | DONE |
| F1-05 | Docs | F1-01–F1-04 | Threat model, gate placement, simulated-vs-real distinction in PRIVACY/ARCHITECTURE | DONE |

## Phase F2.1 — Device Interface and Lifecycle

Abstract real camera/audio behind an interface with consent-guarded lifecycle.
No real hardware integration in this milestone (no NDK/device available).

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F21-01 | IMediaDevice interface | F1-02 | Explicit states, start/stop, deterministic cleanup; no hardware code | DONE |
| F21-02 | ConsentGuardedDevice | F21-01, F1-02 | Gate required before init/start/acquire; withdrawal/expiry/permission/failure/shutdown force stop+release; fail-closed | DONE |
| F21-03 | Lifecycle tests (mock) | F21-01, F21-02 | Denied start, authorized start/stop, withdrawal, expiry, failure, permission revocation, destructor cleanup | DONE |
| F21-04 | Docs | F21-01–F21-03 | Interface/lifecycle, Android env finding, F2.2 milestone in PRIVACY/ARCHITECTURE | DONE |

## Phase F2.2 — Real Android Device Integration

Requires NDK install and a physical device. NDK installed and backends
cross-compile; hardware acceptance testing is NOT done (no device attached).

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F22-01 | Install NDK; confirm API level | F21-04 | NDK r26d installed side-by-side; API 34 assumed, device unconfirmed | DONE |
| F22-02 | Camera2 backend implementing IMediaDevice | F21-01, F22-01 | Cross-compiles for arm64-v8a; fail-closed until device confirmed | DONE (compile only) |
| F22-03 | AAudio backend implementing IMediaDevice | F21-01, F22-01 | Cross-compiles for arm64-v8a; fail-closed until device confirmed | DONE (compile only) |
| F22-04 | On-device lifecycle + permission tests | F22-02, F22-03 | Real permission revocation and device failure force stop | BLOCKED (no device) |
| F22-05 | Actual capture on a physical device | F22-04 | Verified real frame/sample capture gated by consent | BLOCKED (no device) |

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
