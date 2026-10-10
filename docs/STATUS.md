# HomeGuardian AI — Status

**Last updated:** 2026-10-10

## Current Phase

Phase F2.1 — Device Interface and Lifecycle (real-device integration deferred to F2.2)

## Completed Work

| Task | Evidence | Date |
|------|----------|------|
| Repository initialized | `/home/swarna-sekhar-dhar/projects/homeguardian-ai` on `main` branch | 2026-10-09 |
| Initial documentation | README, ARCHITECTURE, ROADMAP, DEVELOPMENT, TESTING, DECISIONS, STATUS | 2026-10-09 |
| Skill inventory | `docs/SKILL_INVENTORY.md` — 24 relevant skills documented | 2026-10-09 |
| Three-tier architecture | `docs/ARCHITECTURE.md` updated with Tier 1/2/3 | 2026-10-09 |
| CMake build system | `cmake` configures, `make -j4` builds `homeguardian` executable | 2026-10-09 |
| Application entry point | `main.cpp` — config load, signal handlers, main loop | 2026-10-09 |
| Structured logging | spdlog with console + file sinks, configurable level | 2026-10-09 |
| Configuration loading | nlohmann/json, validation, defaults, save/reload | 2026-10-09 |
| Graceful shutdown | SIGINT/SIGTERM handled, atomic flag, clean exit | 2026-10-09 |
| Phase B fixes | Logger::get() throws when uninitialized; test thread safety fixed | 2026-10-09 |
| Event model | Event, Alert, EventValidator, UUID — strongly typed, immutable, JSON serializable | 2026-10-09 |
| Event source | IEventSource interface + SimulatedEventSource for testing | 2026-10-09 |
| Correlation rules | ICorrelationRule interface + TimeWindowCorrelation implementation | 2026-10-09 |
| Pipeline | Synchronous Pipeline with bounded history, validation, correlation | 2026-10-09 |
| Config extension | max_event_history, correlation_window_ms, alert_confidence_threshold | 2026-10-09 |
| Application integration | Application::process_event() delegates to Pipeline | 2026-10-09 |
| Friction log | 6 verified incidents documented | 2026-10-09 |
| SQLite persistence | IDatabase, EventRepository, AlertRepository, SchemaManager, PersistenceManager | 2026-10-10 |
| SQLite amalgamation | Compiled directly into project (no system dev headers required) | 2026-10-10 |
| Family profile model | `core/FamilyProfile.h` — versioned, optional age band, no sensitive attributes | 2026-10-10 |
| Consent model | `core/ConsentRecord.h` — granted/denied/withdrawn, default-deny, expiry | 2026-10-10 |
| Routine model | `core/Routine.h` — validated schedule and time zone, configuration only | 2026-10-10 |
| Phase E repositories | ProfileRepository, ConsentRepository, RoutineRepository | 2026-10-10 |
| Schema migration v1→v2 | Additive, transactional, rerunnable; preserves event/alert data | 2026-10-10 |
| Extended result codes | Constraint subtypes distinguished (PK/UNIQUE vs FK) | 2026-10-10 |
| Phase E configuration | media_capture_enabled/cloud_processing_enabled default OFF | 2026-10-10 |
| Test suite | 57 test cases, 241 assertions, all passing | 2026-10-10 |
| Simulated sensor source | `core/SimulatedSensorSource` — deterministic, synthetic-tagged, timestamps, malformed, disconnect/reconnect | 2026-10-10 |
| Consent gate | `core/ConsentGate` — single fail-closed decision point | 2026-10-10 |
| Consent-gated acquisition | `core/ConsentGatedAcquisition` — denied requests produce no payload | 2026-10-10 |
| F1 tests | Consent gate (9 cases), simulated sensor (6), gated acquisition (4) | 2026-10-10 |
| IMediaDevice interface | `core/IMediaDevice.h` — explicit states, start/stop, deterministic cleanup; no hardware code | 2026-10-10 |
| ConsentGuardedDevice | `core/ConsentGuardedDevice` — gate before init/start/acquire; forces stop on withdrawal/expiry/permission/failure | 2026-10-10 |
| Lifecycle tests | 9 mock cases (denied start, authorized start/stop, withdrawal, expiry, failure, permission revocation, destructor) | 2026-10-10 |
| Test suite | 85 test cases, 357 assertions, all passing | 2026-10-10 |

## Test Results

```
100% tests passed, 0 tests failed out of 1
All tests passed (357 assertions in 85 test cases)
ASan/UBSan: passed, no leaks, no sanitizer errors
```

## Current Work

Phase F2.1 — device interface and lifecycle. Complete and verified with mocks.
Real Android device integration (F2.2) is deferred: no NDK is installed and no
device is attached, so real Camera2/AAudio code cannot be built or tested here.

## Blockers

None.

## Known Issues

- `Logger::initialize()` is not thread-safe on first call. Acceptable for single-threaded startup; must be fixed before multi-threaded use.
- `Application` singleton pattern prevents multiple instances. Acceptable for single-process design.
- Pipeline is synchronous only. No async processing yet — by design for Phase C.
- Consent records are household configuration only; they are not proof of legally valid consent and are not yet enforced by any media-processing component (none exists yet).

## Next Authorized Task

Phase E2 — Sensing and Inference (deferred): simulated event source, camera
interface, face/voice analysis, ONNX Runtime. No media capture is enabled by
default and none is implemented.

## Resource Budget

- CPU: 30% of 40 logical CPUs = 12 logical CPUs max
- Build parallelism: `-j4` (conservative)
- Memory: 15 GB total, monitor usage during builds

## Session Info

- **Remote:** `origin` → `https://github.com/methakon/homeguardian-ai.git` (private)
- **Repository:** `/home/swarna-sekhar-dhar/projects/homeguardian-ai`
- **Branch:** `main`
- **Phase A commit:** `312cb75`
- **Phase B commit:** `058c692`
- **Phase B fix commit:** `e9ebfa3`
- **Phase C commit:** `479dccf`
- **Friction log commit:** `90691ef`
- **Phase D commit:** `5712aac`
- **Phase E commit:** `a472968`
- **Phase F1 commit:** `90841e9`
