# HomeGuardian AI — Status

**Last updated:** 2026-10-10

## Current Phase

Phase F2.2 — Real Android Device Backends (NDK installed; hardware testing blocked)

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

## Test Results

```
100% tests passed, 0 tests failed out of 1
All tests passed (424 assertions in 93 test cases)
ASan/UBSan: passed, no leaks, no sanitizer errors
Android cross-compile (arm64-v8a/API 34): passed (static archive, not runnable)
Android cross-compile (armeabi-v7a/API 27): passed (on-device self-test binary)
On-device non-capture self-test (real hardware): 17/17 PASS, exit 0
```

## Verification Status (separated by layer)

- **Compile (Android arm64-v8a / API 34):** VERIFIED.
- **Compile (Android armeabi-v7a / API 27):** VERIFIED.
- **Mock integration (host fake backend):** VERIFIED.
- **On-device non-capture self-test (real hardware):** VERIFIED — consent gate,
  lifecycle, fail-closed guards, cleanup; no camera/mic opened.
- **APK harness (build/package only):** VERIFIED — debug-signed APK builds and
  packages; declares only CAMERA + RECORD_AUDIO; capture disabled by default;
  NOT installed, no permissions granted, no capture run.
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
