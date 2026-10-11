# HomeGuardian AI — Status

**Last updated:** 2026-10-11

## Current Phase

Simulator-first development — deterministic in-process sensor simulator,
dual-device shared audio broker, and consent-gated acquisition. Real camera/
microphone capture remains BLOCKED pending explicit operator approval.

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
| NDK install | NDK r26d (26.3.11579264) installed side-by-side via official Google download | 2026-10-10 |
| Android camera backend | `backend/android/NdkCameraDevice` — NDK camera API; fail-closed; cross-compiles arm64-v8a | 2026-10-10 |
| Android audio backend | `backend/android/AAudioCaptureDevice` — AAudio; fail-closed; cross-compiles arm64-v8a | 2026-10-10 |
| Delivery-time consent gate | `ConsentGuardedDevice::authorize_delivery()` — re-checks before every frame/sample delivery | 2026-10-10 |
| Fake-backend mock tests | `core/FakeMediaDevice` + `test_fake_device_delivery.cpp` — 8 host cases | 2026-10-10 |
| Defect fix | Errored device no longer resurrected to Idle; camera stop() releases handles | 2026-10-10 |
| Confirmed device | MediaTek MT6580 "Ruby" (model string cosmetic), Android 8.1.0 / API 27, armeabi-v7a, ~962MB RAM | 2026-10-10 |
| armeabi-v7a/API 27 build + on-device self-test | `hg_ondevice_selftest` built for device ABI/API; 17/17 non-capture checks pass on device via adb | 2026-10-10 |
| JDK + SDK platform 27 | Temurin JDK 17 (user-writable) and SDK platform 27 / build-tools 27.0.3 installed | 2026-10-10 |
| APK harness (non-capture) | Minimal debug-signed APK built with JDK+SDK+NDK (no Gradle); keystore outside repo; capture disabled by default | 2026-10-11 |
| Test suite | 93 test cases, 424 assertions, all passing | 2026-10-10 |
| Simulator-first backend | `backend/simulator/SimulatedSensorBackend` — deterministic camera frames + PCM mic samples; bounded ring; Config struct (dimensions, frame-rate, channels, test signals) | 2026-10-10 |
| Linux sensor discovery | `backend/linux/SensorDiscovery` — V4L2 + PipeWire metadata enumeration; opens no device | 2026-10-10 |
| End-to-end simulator workflow | `test_e2e_simulator.cpp` — camera/mic/speaker → consent-gated delivery → synthetic processor → pipeline rule/alert → safe speaker response; failure & consent scenarios | 2026-10-10 |
| Dual logical-device + shared audio broker | `backend/simulator/LogicalDevice` + `AudioBroker` — two independent logical devices share ONE mic (single capture) with per-consumer consent fan-out and ONE bounded arbitrated speaker | 2026-10-11 |
| Product-readiness audit | Fixed stale README, hardcoded CMake paths → `${CMAKE_SOURCE_DIR}`, hardened .gitignore; documented build reproduced fresh | 2026-10-11 |
| Alexa custom skill prototype | `alexa_skill/` — ASK SDK v2 handler + 13 Node unit tests (optional, separate from core; no deployment) | 2026-10-10 |
| Test suite | 147 test cases, 1546 assertions, all passing | 2026-10-11 |
| Runtime integration | `core/SimulatorRuntime` wired into `Application::run()` — the running app exercises the consent gate, shared audio broker, event pipeline, and clean shutdown; 7 runtime integration tests | 2026-10-11 |
| Test suite | 154 test cases, 1583 assertions, all passing | 2026-10-11 |
| Event-to-alert integration | `SimulatorRuntime` routes AUTHORIZED synthetic observations through `Application::process_event()` → Pipeline validation + TimeWindowCorrelation + alert; 9 event-to-alert tests; capture-off/withdrawal/missing-consent produce no events | 2026-10-11 |
| Test suite | 163 test cases, 1617 assertions, all passing | 2026-10-11 |
| Runtime hardening v1 | `core/AlertPolicy` (dedup/cooldown/re-arm, injectable clock, bounded); explicit `AppState` lifecycle + partial-startup cleanup; 27 hardening tests; reproducible demo `docs/DEMO_SIMULATOR.md` | 2026-10-11 |
| Test suite | 190 test cases, 1712 assertions, all passing | 2026-10-11 |
| Runtime readiness v2 | `core/PersistenceAudit` (durable alert metadata via existing SQLite, historical-vs-current distinguishability, retention, fail-safe degraded) + `core/RuntimeStatus` (bounded privacy-safe counters); restart distinguishability verified; 10 audit/status tests | 2026-10-11 |
| Test suite | 200 test cases, 1753 assertions, all passing | 2026-10-11 |
| Runtime readiness v3 | `core/AlexaContract` (versioned, authenticated, read-only integration contract + auth boundary; 14 contract tests) + Node adapter tests (12); SQLite integrity/migration tests (7); privacy/consent regression tests (7); plus P1 rule-specific clearing (3) and P2 scheduled retention (4) landed this cycle | 2026-10-11 |
| Test suite | 235 test cases, 1925 assertions, all passing | 2026-10-11 |
| Runtime readiness v4 | `hg_alexa_bridge` (local NDJSON stdio bridge over the real AlexaContract) + Node process-boundary integration tests (14) and restart tests (4) driving the actual compiled C++ binary; reproducible demo `docs/demo_alexa_integration.sh`; no network/deploy | 2026-10-11 |
| Test suite | 235 test cases, 1928 assertions, all passing; Node 43/43 (13 handler + 12 contract + 14 integration + 4 restart) | 2026-10-11 |

## Test Results

```
100% tests passed, 0 tests failed out of 1
All tests passed (1925 assertions in 235 test cases)
Focused: [audit] 11 / 50; [status] 3 / 13; [alertpolicy] 14 / 62; [clearing] 3 / 17; [retention] 7 / 34; [alexa] 14 / 70; [integrity] 7 / 39; [privacy] 16 / 66; [lifecycle] 17 / 107; [hardening] 5 / 13; [event2alert] 9 / 34; [runtime] 7 / 40; [sim] 24 / 915; [dual] 12 / 74; [e2e] 9 / 73 — all pass
ASan/UBSan full suite: passed (235 cases), no leaks, no sanitizer errors
Android cross-compile (arm64-v8a/API 34): passed (static archive, not runnable)
Android cross-compile (armeabi-v7a/API 27): passed (on-device self-test binary)
On-device non-capture self-test (real hardware): 17/17 PASS, exit 0
Application run (simulator mode): capture-off → no media/no alerts + final_state=Stopped; capture-on → authorized observations through Pipeline, 1 alert delivered + persisted + 8 dedup-suppressed, pipeline_history=10, bounded speaker, final_state=Stopped
Restart: same audit DB, 2nd process reports historical_alerts=1 (distinguishable from current-process alerts)
Alexa contract: 14 C++ contract tests + 12 Node adapter tests pass (mock clients, no deploy); LIVE integration NOT tested (needs deployed authenticated endpoint) — documented as pending
Local process-boundary integration: hg_alexa_bridge (NDJSON stdio over the REAL AlexaContract) driven by Node — 14 integration + 4 restart cases against the actual compiled binary; reproducible demo docs/demo_alexa_integration.sh; no network listener, no deploy
Release reproduction: fresh out-of-source build (existing dep cache) → 235/1928 + bridge, new tests included
```

## Verification Status (separated by layer)

- **Compile (Android arm64-v8a / API 34):** VERIFIED.
- **Compile (Android armeabi-v7a / API 27):** VERIFIED.
- **Mock integration (host fake backend):** VERIFIED.
- **Linux simulator (consent, lifecycle, delivery, bounded buffers, dual-device,
  shared audio broker, e2e):** VERIFIED — deterministic, host-only; NO physical
  sensor opened.
- **Documented build reproduction (fresh out-of-source dir):** VERIFIED.
- **On-device non-capture self-test (real hardware):** VERIFIED — consent gate,
  lifecycle, fail-closed guards, cleanup; no camera/mic opened.
- **APK harness (build/package only):** VERIFIED — debug-signed APK builds and
  packages; declares only CAMERA + RECORD_AUDIO; capture disabled by default;
  NOT installed, no permissions granted, no capture run.
- **Real V4L2/ALSA/PipeWire capture or OS permission semantics:** NOT VERIFIED —
  simulator-only; requires physical hardware or a root-loaded virtual device.
- **Physical hardware capture (camera/audio):** BLOCKED — awaits explicit
  operator approval before the first camera/microphone activation. NOT claimed.

## Current Work

Phase F2.2 — Android device backends. NDK r26d installed; NdkCameraDevice and
AAudioCaptureDevice cross-compile for arm64-v8a / API 34 and are fail-closed by
default. On-device capture, permission-revocation, and hardware-boundary consent
testing are BLOCKED: no device is attached (adb empty) and API 34 is an
unconfirmed assumption. Hardware-level consent enforcement is not claimed.

## Blockers

- Real camera/audio capture (F22-09) is BLOCKED pending: an APK harness with a
  proper Android runtime permission flow, and explicit approval before the first
  camera/microphone activation. The no-capture on-device self-test already runs
  and passes. Device is connected and authorized.
- The device's marketing model string ("S25_Ultra") does not match its actual
  hardware (MediaTek MT6580 "Ruby", 32-bit, ~1 GB RAM); build targets follow the
  real hardware, not the model string.

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
- **Phase F2.1 commit:** `e81584e`
- **Phase F2.2 commit:** `50e43ea`
- **F2.2 hardening commit:** `94e6714`
- **F22-08 commit:** `2caf62e`
