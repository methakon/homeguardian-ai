# HomeGuardian AI — Friction Log

**Project:** HomeGuardian AI
**Hackathon:** Amazon Developer Hackathon 2026
**Last updated:** 2026-10-10

This log records only verified friction encountered during development.
Each incident includes the date, steps, expected vs actual result, severity,
workaround, and actionable suggestion.

---

## Incident 1: CMake FetchContent Download Timeout

**Date:** 2026-10-09
**Task:** Phase B and Phase C build configuration

**Steps:**
1. `cmake .. -DCMAKE_BUILD_TYPE=Debug -DHOMEGUARDIAN_BUILD_TESTS=ON`
2. CMake FetchContent downloads spdlog v1.13.0, nlohmann/json v3.11.3, Catch2 v2.13.10 from GitHub

**Expected:** CMake configure completes in < 30 seconds

**Actual:** CMake configure took 117-119 seconds due to network downloads. First `make -j4` attempt timed out at 120 seconds because configure + build exceeded the timeout.

**Severity:** Important

**Workaround:** Run cmake configure separately with a 300s timeout, then run `make -j4` separately. Dependencies are cached in `build/_deps/` after first download.

**Suggestion:** Consider providing a local package cache or pre-built dependency bundle for hackathon participants to avoid network-dependent builds.

**Status:** Resolved (dependencies now cached locally)

---

## Incident 2: Catch2 v2 CMake Integration

**Date:** 2026-10-09
**Task:** Phase B test suite setup

**Steps:**
1. Used `include(Catch)` and `catch_discover_tests(homeguardian_tests)` in `tests/CMakeLists.txt`
2. CMake configure failed

**Expected:** Catch2 v2 provides `catch_discover_tests` macro

**Actual:** CMake error: `include could not find requested file: Catch` and `Unknown CMake command "catch_discover_tests"`. Catch2 v2 does not provide the `Catch` CMake module — that was added in Catch2 v3.

**Severity:** Important

**Workaround:** Replaced with `add_test(NAME homeguardian_tests COMMAND homeguardian_tests)` which works with Catch2 v2's `CATCH_CONFIG_MAIN` pattern.

**Suggestion:** Catch2 v2 documentation should clearly state that `catch_discover_tests` is v3-only. Consider backporting the CMake integration to v2 or providing a migration guide.

**Status:** Resolved

---

## Incident 3: Logger Fallback Masking Initialization Failures

**Date:** 2026-10-09
**Task:** Phase B code review

**Steps:**
1. Reviewed `Logger::get()` implementation
2. Found that calling `get()` before `initialize()` silently created a default "info" logger

**Expected:** `Logger::get()` should fail explicitly if `initialize()` was not called

**Actual:** `Logger::get()` called `initialize("info")` as a fallback, masking configuration errors. If config loading failed and the logger was not initialized, the application would silently use default settings instead of reporting the error.

**Severity:** Important

**Workaround:** Changed `Logger::get()` to throw `std::runtime_error` when not initialized. Added `Logger::reset()` for test cleanup.

**Suggestion:** This is a code-level fix, not a tool issue. No external suggestion needed.

**Status:** Resolved (commit `e9ebfa3`)

---

## Incident 4: Catch2 Assertions in Worker Thread

**Date:** 2026-10-09
**Task:** Phase B test suite — Application shutdown test

**Steps:**
1. `test_application.cpp` ran `Application::run()` in a worker thread
2. Used `REQUIRE(result == 0)` inside the worker thread lambda

**Expected:** Test failure in worker thread propagates to Catch2

**Actual:** Catch2 `REQUIRE` inside a worker thread does not reliably propagate failures to the main test runner. A failing assertion in the worker thread would not cause the test to fail.

**Severity:** Important

**Workaround:** Moved assertions to the main thread using `std::atomic<int>` and `std::atomic<bool>` to capture results from the worker thread, then verified with `REQUIRE` in the main thread.

**Suggestion:** Catch2 documentation should warn against using `REQUIRE` inside worker threads. Consider providing a `REQUIRE_THREAD_SAFE` macro or documentation pattern for multi-threaded tests.

**Status:** Resolved (commit `e9ebfa3`)

---

## Incident 5: spdlog Logger Name Collision on Re-initialization

**Date:** 2026-10-09
**Task:** Phase B test — Logger uninitialized path

**Steps:**
1. Added `Logger::reset()` for test cleanup
2. Called `Logger::reset()` then `Logger::initialize("info")` in the same test
3. spdlog threw `logger with name 'homeguardian' already exists`

**Expected:** `reset()` should fully clean up the logger so re-initialization works

**Actual:** `reset()` only cleared the `shared_ptr` but did not remove the logger from spdlog's global registry. Re-initialization failed because spdlog still had the old logger registered under the same name.

**Severity:** Important

**Workaround:** Changed `reset()` to call `spdlog::drop(logger_->name())` before clearing the `shared_ptr`.

**Suggestion:** spdlog should provide a `reset()` or `shutdown()` method that both removes the logger from the registry and clears the internal state. Currently, users must know to call `spdlog::drop()` manually.

**Status:** Resolved (commit `e9ebfa3`)

---

## Incident 6: Unused Parameter Warning in Signal Handler

**Date:** 2026-10-09
**Task:** Phase C build

**Steps:**
1. Built with `-Wall -Wextra -Wpedantic`
2. Warning: `unused parameter 'signal' [-Wunused-parameter]` in `Application::signal_handler(int signal)`

**Expected:** No compiler warnings

**Actual:** The `signal` parameter was unused because the handler calls `request_shutdown()` regardless of which signal was received.

**Severity:** Nice-to-have

**Workaround:** Added `[[maybe_unused]]` attribute to the parameter.

**Suggestion:** This is a code-level fix, not a tool issue. No external suggestion needed.

**Status:** Resolved (commit `479dccf`)

---

## Incident 7: APK Launch Crash on Android 8.1 — Java 8 Lambda invokedynamic

**Date/time:** 2026-10-10 ~19:00 IST
**Area:** Android / build system

**Expected:** The harness APK launches and runs the non-capture self-test.

**Actual:** App crashed on launch:
`BootstrapMethodError` → `NoClassDefFoundError: Invalid descriptor: ex`
at `MainActivity.java` button-listener creation.

**Evidence:** `adb logcat` FATAL EXCEPTION; `dx` was invoked with
`--min-sdk-version 27`.

**Root cause:** Confirmed. Java 8 lambdas compile to `invokedynamic`. The
Android 8.1 (API 27) dex verifier on this device rejects the lambda bootstrap
descriptor. Two contributing factors: lambdas in the Java source, and `dx
--min-sdk-version 27` which enables invokedynamic desugaring.

**Resolution:** Replaced all six button listeners with anonymous inner classes
and removed `--min-sdk-version` from the `dx` step so it emits legacy
`StringBuilder` string-concat. Verified the actual DEX with `dexdump`:
0 invokedynamic/invoke-custom opcodes, 5 StringBuilder uses, listeners compiled
to `MainActivity$1..$6`. Commits `b3bd012`.

**Regression protection:** On-device launch after fix: app runs, self-test
executes, no crash.

**Remaining impact:** None for launch. Related to Incident 8.

---

## Incident 8: APK Launch Crash — Missing `libc++_shared.so`

**Date/time:** 2026-10-10 ~19:03 IST
**Area:** Android / NDK / build system

**Expected:** Native library loads after the lambda fix.

**Actual:** `java.lang.UnsatisfiedLinkError: dlopen failed: library
"libc++_shared.so" not found` at `System.loadLibrary`.

**Evidence:** `adb logcat` HGHarness UnsatisfiedLinkError; APK contained only
`libhg_harness.so`.

**Root cause:** Confirmed. The NDK C++ runtime (`libc++_shared.so`) was not
bundled into the APK, and it is not present as a system library on this device.

**Resolution:** Build script now copies
`$NDK/.../sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so` into
`lib/armeabi-v7a/` in the APK. Verified both `.so` present via `unzip -l`.
Commit `b3bd012`.

**Regression protection:** On-device: native lib loads, self-test logs
`gate_cam=1 gate_unknown_denied=1 cam_closed=1 mic_closed=1`.

**Remaining impact:** None.

---

## Incident 9: Camera Test Crashed Process — Uncaught C++ Exception Across JNI

**Date/time:** 2026-10-10 ~19:13 IST
**Area:** Android / C++ core / consent-privacy

**Expected:** A camera initialization failure returns a clean error to the UI.

**Actual:** Tapping "Start CAMERA test" aborted the process:
`libc++abi: terminating due to uncaught exception of type std::runtime_error:
NdkCameraDevice: no camera available`. App died (Application Error).

**Evidence:** `adb logcat` FATAL + tombstone abort message.

**Root cause:** Confirmed. `nativeStartCamera` let a `std::runtime_error`
thrown by the native `initialize()` escape the JNI boundary. A C++ exception
crossing JNI terminates the process.

**Resolution:** Wrapped `initialize()` and `start()` in both `nativeStartCamera`
and `nativeStartMic` with try/catch; on exception the device is closed, the
fail-closed reason is logged, and an error string is returned. Consent and
capture authorization semantics unchanged. Commit `68e9360`.

**Regression protection:** On-device after fix: tapping Start CAMERA keeps the
process ALIVE; logcat shows `camera initialize threw (fail-closed)`; no camera
opened. Host regression 93 cases / 424 assertions pass.

**Remaining impact:** Underlying cause of the thrown exception is Incident 10.

---

## Incident 10: NDK Camera Enumeration Returns 0 Devices on MT6580 (Camera1-shim HAL)

**Date/time:** 2026-10-10 ~19:28 IST
**Area:** Android / NDK / device-HAL

**Expected:** `ACameraManager_getCameraIdList` returns the 2 cameras the
framework reports.

**Actual:** NDK enumeration returns 0 cameras. `dumpsys media.camera` reports
"Number of camera devices: 2", but the NDK call yields an empty list.

**Evidence:**
- Framework: `Number of camera devices: 2`; `Camera1 API shim is using
  parameters`; HAL devices are `device@1.0/internal/0` (Back) and
  `device@1.0/internal/1` (Front) — **HAL version 1.0**.
- Instrumented NDK logcat: `ACameraManager_create OK (0x...)`;
  `getCameraIdList status=0 list=0x... numCameras=0`;
  `no camera available (status=0) — NDK enumeration returned 0 devices`.
- Both HAL devices "closed, no client instance" (free). CAMERA granted=true.

**Root cause:** Confirmed as an OEM/HAL limitation, not an application bug.
The device exposes cameras only through a Camera HAL v1.0 legacy interface via
the Camera1 shim. The NDK `ACameraManager` (camera2ndk) requires a camera2/HAL3
provider; on a HAL1-only device the camera2 service has zero camera2 devices to
enumerate. Framework inventory (via the shim) does NOT imply NDK camera2
compatibility. Our code is correct and fails closed as designed.

**Resolution:** No code fix — this is a device limitation. Diagnostic logging
added to `NdkCameraDevice.cpp` (distinguishes create-null vs idlist-0). The
exception guard and fail-closed behavior are retained. Committed with this log.

**Regression protection:** Host regression 93 cases / 424 assertions pass.
Fail-closed verified on-device (process stays alive, no capture).

**Remaining impact:** Real camera capture is BLOCKED on this device via the NDK
camera2 API. Viable alternatives for a HAL1-only device: (a) use the legacy
Camera1 Java API (`android.hardware.Camera`) via JNI, or (b) verify on a
camera2-native device. Not yet implemented. Next diagnostic step: decide
Camera1-JNI path vs. testing on camera2 hardware.

**Amendment (2026-10-10, later):** Resolved by implementing the Camera1-JNI
path (option a). See Incident 11.

---

## Incident 11: Camera1-JNI Adapter Implementation (resolution of Incident 10)

**Date/time:** 2026-10-10 ~19:40 IST
**Area:** Android / C++ core / consent-privacy

**Expected:** A working, consent-gated camera path on the Camera1-only device.

**Actual:** Implemented. `apk/.../Camera1Bridge.java` wraps
`android.hardware.Camera` (legacy API) and a native delivery gate
`nativeOnPreviewFrame` in `hg_harness_jni.cpp` reuses the existing
`ConsentGuardedDevice::authorize_delivery()` per preview frame. Backend
selection is explicit (`USE_CAMERA1`), never a silent fallback; `NdkCameraDevice`
is retained for camera2-native devices.

**Evidence:**
- Host: `test_camera1_adapter.cpp` 6 cases pass (46 assertions); full host
  regression 99 cases / 470 assertions pass; ctest 100%.
- APK build (armeabi-v7a / API 27): `Camera1Bridge` in DEX (0 invokedynamic);
  native symbol `Java_..._Camera1Bridge_nativeOnPreviewFrame` exported;
  signature valid. (`android.hardware.Camera` deprecation note is expected.)
- On-device launch (no capture): app runs, self-test logs
  `gate_cam=1 gate_unknown_denied=1 cam_closed=1 mic_closed=1`, no crash,
  camera NOT active (Active Camera Clients empty).

**Root cause addressed:** Incident 10 (NDK camera2 enumerates 0 on this
Camera1-shim HAL1 device).

**Resolution:** Camera1 adapter added; reuses the single consent mechanism
(`ConsentGuardedDevice`); every preview frame is gated immediately before
delivery; on withdrawal/revocation/error the gate returns false, the frame is
dropped, and the bridge stops preview and releases the camera. Capture stays
disabled by default; requires operator action + CAMERA permission.

**Regression protection:** 6 new host contract tests; full host suite green.

**Remaining impact:** Real Camera1 capture on the device is NOT yet run —
awaiting explicit approval (F22-11). Hardware-level capture enforcement is not
claimed until a Camera1 frame is actually captured and the withdrawal-stops-
delivery behaviour is observed on-device.

---

## Incident 12: Ubuntu Sensor Backend Assessment + AVS Deprecation Finding

**Date/time:** 2026-10-10 ~20:30 IST
**Area:** Platform expansion / documentation / Alexa integration research

**Expected:** Extend HomeGuardian to the Ubuntu development computer (camera via
V4L2, mic via ALSA/PipeWire, speaker), evaluate Docker, and assess Alexa+
integration.

**Actual (non-capture):**
- Implemented `src/backend/linux/SensorDiscovery.{h,cpp}`: non-capture
  enumeration of V4L2 camera nodes (`/dev/video*` + sysfs name) and PipeWire
  audio sources/sinks (via `pactl` metadata). Opens no device; reads no
  frames/samples.
- Added `tests/core/test_ubuntu_backend.cpp`: 4 host cases (discovery
  well-formedness against the live host; capture-disabled-by-default blocks
  start; withdrawal stops per-frame delivery; permission revocation stops
  delivery), all reusing `ConsentGuardedDevice`. Host only — no sensor opened.
- Full host suite: 103 cases / 507 assertions pass; ctest 100%.
- Documented the Ubuntu/Android distinction, Docker trade-offs, and Alexa
  findings in ARCHITECTURE, ROADMAP, PRIVACY.

**Root cause (Alexa+):** Investigated current Alexa developer routes and found
**AVS developer tools are no longer generally available for Alexa Built-in**;
Amazon directs developers to the Works with Alexa program. The supported route
is an Alexa custom/Smart Home skill → authenticated endpoint → C++ service.
The Alexa simulator cannot access the local camera/mic. No integration built;
no credentials requested.

**Resolution:** Non-capture Ubuntu discovery + host consent tests landed
(F22-12). Capture adapters (F22-13) remain PROPOSED and require explicit
approval before any sensor is opened. Docker (F22-14) and Alexa (F22-15)
documented as proposed/research-only.

**Regression protection:** 4 new host tests; full host suite green.

**Remaining impact:** The Ubuntu computer's camera and microphone have NOT been
opened. Capture adapters, Docker deployment, and Alexa integration are not
implemented and are awaiting approval / further work. No Alexa+ interaction has
been demonstrated, so no integration is claimed.

---

## Incident 13: Simulator-First Backend + Docker Fallback Decision

**Date/time:** 2026-10-10 ~21:00 IST
**Area:** Platform / test strategy / Docker

**Expected:** Assess and implement the most accurate simulator-based development
and test environment; use Docker only if the simulator is insufficient.

**Actual:**
- Assessed the Linux virtual-device tooling actually present:
  - `v4l2loopback` kernel module present on disk (v0.15.3) but **not loaded**
    and **not loadable without root** (no passwordless sudo) → cannot be used
    in this environment.
  - PipeWire `module-null-sink` creatable at runtime (verified: sink `hgsim`
    + `.monitor` appeared, then unloaded) → available but not needed for this
    phase.
  - ALSA `null` PCM plugin usable (`arecord -D null` OK) → not needed.
  - `ffmpeg` with `lavfi` present (in `~/.hermes/tools`) → not needed.
- Implemented `src/backend/simulator/SimulatedSensorBackend.{h,cpp}`: a
  deterministic, in-process `IMediaDevice` (seeded LCG camera frames and 16-bit
  PCM mic samples; `BoundedRing` acquisition buffer, capacity 256,
  overwrite-oldest; `fail()`/`recover()` error paths). Sits behind the same
  `ConsentGuardedDevice` gate as real backends.
- Added `tests/core/test_simulator_backend.cpp`: 12 host cases (determinism,
  lifecycle idempotency, capture-disabled denial, authorized delivery, consent
  withdrawal, consent expiry, permission revocation, bounded-buffer overflow,
  device error, error recovery, shutdown cleanup, audio PCM determinism).
- Full host regression: **115 cases / 575 assertions pass; ctest 100%.**

**Docker decision:** Docker is **NOT necessary** for this milestone. Every
in-scope requirement (consent deny/withdraw/expire, delivery-boundary
authorization, permission revocation, lifecycle, bounded buffers, error
recovery, shutdown) is validated deterministically by the host simulator on the
shared code paths. Only real V4L2/ALSA/PipeWire device interaction and real OS
permission semantics need hardware, and Docker on this host does not provide
that either. Docker remains a documented fallback; not implemented. No
privileged container, no PipeWire socket mount, no camera/mic passthrough.

**Resolution:** Simulator-first backend landed (F22-16); Docker deferred (F22-17)
with the capability matrix documented in ARCHITECTURE.

**Regression protection:** 12 new host tests; full host suite green.

**Remaining impact:** The simulator validates behavior on shared code paths but
does NOT prove real-device V4L2/ALSA/PipeWire correctness or real OS permission
semantics. No physical camera or microphone was opened. Real capture remains
gated behind explicit approval.

---

## Incident 14: Alexa Custom Skill Prototype + Simulator Endpoint Requirement

**Date/time:** 2026-10-10 ~22:00 IST
**Area:** Alexa integration / free-first prototyping

**Expected:** A minimal Alexa custom skill testable in the official Alexa
Developer Console Simulator, free-first, separate from the C++ core and sensors.

**Actual:**
- Verified current official docs (Amazon, 2026):
  - **The Alexa Developer Console Simulator requires a configured endpoint and
    deployed skill code** — it cannot test a skill with no backend. Lowest-cost
    supported path is an **Alexa-hosted skill** (auto-provisions AWS Lambda; no
    AWS account; testing within Lambda free tier → expected $0).
  - **Node.js 16 is deprecated**; Alexa-hosted skills default to a current
    Node.js (18/20/22) or Python runtime. ASK SDK v2 (`ask-sdk-core`) is
    compatible.
  - AVS remains deprecated for Alexa Built-in (from Incident 12).
- Implemented `alexa_skill/` using **ASK SDK v2 for Node.js**:
  - `index.js` — Lambda entry + intents (`HomeStatusIntent`,
    `GetAlertSummaryIntent`, `GetRoutineStatusIntent`, `AMAZON.HelpIntent`,
    `AMAZON.StopIntent`/`CancelIntent`) + safe catch-all; error/invalid-data
    paths return generic responses that never leak exception text or secrets.
  - `src/backend-interface.js` — documented `HomeGuardianBackend` contract +
    mock; handlers depend only on this interface; no network/disk/sensor/cred
    access.
  - `interactionModels/custom/en-US.json` — interaction model ("home guardian").
  - `test/handler.test.js` — 13 Node tests (built-in runner), no deployment.
  - `README.md` — exact manual console steps + cost/credential notes.
- Tests: **Alexa 13/13 pass** (supported intents, malformed request, backend
  error + invalid data, session-ended, mock exposes no capture/sensitive data).
  C++ full regression still green: 115 cases / 575 assertions; ctest 100%.

**Root cause (why simulator alone is not enough):** the console simulator is
gated on a deployed endpoint; this is a platform requirement, not a code defect.

**Resolution:** Handler + unit tests landed (F22-18). Deployment + console
simulator (F22-19) is BLOCKED on manual console steps documented in
`alexa_skill/README.md`; no deployment performed by these files, no AWS
resources created, no credentials stored.

**Regression protection:** 13 new Node tests; C++ regression unchanged/green.

**Remaining impact:** Console-simulator end-to-end is NOT yet run — it requires
the operator to create an Amazon Developer account, create/build the interaction
model, deploy the handler (Alexa-hosted), and run text utterances. No Echo
device, microphone, camera, or AWS account is required, and expected charge is
$0. No physical sensor was opened.

---

## Incident 15: Realistic Linux Sensor Simulation + Alexa-Demotion-to-Optional

**Date/time:** 2026-10-10 ~22:40 IST
**Area:** Simulator / test strategy / integration posture

**Expected:** Linux-native simulation as the PRIMARY testing path; Alexa kept as
an optional, additional integration-testing method (not a prerequisite for core
functionality).

**Actual:**
- Confirmed Alexa is already isolated in `alexa_skill/` (a separate command/
  response integration test) and demoted F22-19 from BLOCKED to OPTIONAL. No
  Alexa hosting, skill deployment, Echo device, or Amazon account is required
  for the core. No full Alexa skill beyond the existing prototype; no AWS
  resources created.
- Extended `src/backend/simulator/SimulatedSensorBackend` with a `Config` struct
  (avoids reordering the positional constructor):
  - Camera: configurable width/height; optional `frame_rate` pacing (wall-clock,
    0 = unpaced); frame bytes remain deterministic.
  - Microphone: configurable sample_rate, samples_per_buffer, channels
    (mono/stereo 16-bit PCM interleaved), and `TestSignal`
    (noise/sine/square/silence) with `tone_hz`.
  - Speaker: `play()` (output-only, no capture gate), `inject_playback_error()`,
    recovery via close/re-init; play never touches the capture buffer/disk/
    hardware.
  - Buffer: configurable `ring_capacity` (default 256), overwrite-oldest.
- Added 11 host tests (23 total `[sim]`): dimensions/frame-size, frame-rate
  pacing (loose time bound), configurable ring capacity + overflow, stereo
  sizing, silence/sine/square determinism, speaker play/not-initialized/error/
  recovery, and play independent of the capture consent gate.

**Root cause / posture:** Linux-native simulation already validates the entire
consent/lifecycle/delivery/bounded-buffer/shutdown surface on shared code
paths; no privileged changes or physical hardware are needed for the default
suite. v4l2loopback is present but not loadable without root, so virtual
device integration stays optional and unused.

**Resolution:** Realistic simulation landed (F22-20). Consent enforcement is
unchanged — all simulated frames/samples still pass through
`ConsentGuardedDevice::authorize_delivery()`; the simulator never activates a
real device.

**Regression protection:** 23 `[sim]` tests; full host suite green (see results
in this commit).

**Remaining impact:** The simulator does not prove real-device V4L2/ALSA/
PipeWire correctness or real OS permission semantics. No physical camera or
microphone was activated, no kernel module loaded, no system permission changed.

---

## Incident 16: End-to-End Linux Simulator Workflow

**Date/time:** 2026-10-11 ~02:25 IST
**Area:** Simulator / integration test / consent

**Expected:** A deterministic end-to-end workflow using the existing simulator,
consent guard, event pipeline, rules, and simulated speaker, plus failure and
consent scenarios.

**Actual:**
- Added `tests/core/test_e2e_simulator.cpp` (9 cases / 73 assertions) wiring
  existing components: simulated camera+mic+speaker → consent-gated delivery →
  a clearly-labelled `SyntheticTestProcessor` (only sees gate-authorized
  payloads; performs no real analysis) → `Pipeline` + `TimeWindowCorrelation`
  rule/alert → a safe, synthetic response → simulated speaker playback →
  ordering / bounded-memory / cleanup verification.
- Failure/consent scenarios: consent denied before init; consent withdrawn
  between deliveries; capture disabled during processing; camera failure +
  recovery; event-processing failure (throwing rule surfaces, pipeline stays
  usable); speaker playback failure + recovery; shutdown with pending buffered
  data (bounded during capture, released on close). Denied data never reaches
  the processor (asserted). Speaker output is independent of the capture gate.

**Test results (this commit):**
- Focused `[e2e]`: 9 cases / 73 assertions pass.
- Full host suite: **135 cases / 1472 assertions pass; ctest 100%.**
- ASan+UBSan full suite (`hg_asan`, incremental build — no clean rebuild):
  **135 cases / 1472 assertions pass, zero sanitizer errors.**

**Notes discovered while testing (product behavior, not defects):**
- `ConsentGate` fails closed on **multiple active grants for the same
  subject+purpose** (ambiguous). The E2E test therefore uses distinct purposes
  for camera vs mic. This is the gate working as designed.
- `Event`'s constructor rejects empty `event_id` and non-object payloads, so the
  validator's empty-id/non-object checks are unreachable via the public
  factory; the event-processing-failure case instead exercises a throwing
  correlation rule.

**Resolution:** E2E simulator workflow landed (F22-21). Consent enforcement
unchanged; all capture data passes `authorize_delivery()`; the simulator never
opens a real device.

**Regression protection:** 9 new `[e2e]` tests; full host + sanitizer suites
green.

**Remaining impact:** This is a synthetic scenario — it does not detect
real-world danger, identify people, or provide medically/emotionally sensitive
conclusions, and it does not prove real-device V4L2/ALSA/PipeWire correctness or
real OS permission semantics. No physical sensor was activated; no kernel module
loaded; no permission changed.

---

## Incident 17: Product-Readiness Audit (stale docs + CMake portability)

**Date/time:** 2026-10-11 ~03:10 IST
**Area:** Documentation accuracy / build portability / repo hygiene

**Expected:** Audit build, deps, install/config, fixtures, privacy, and docs;
verify the documented build reproduces; fix in-scope issues.

**Actual:**
- **Reproduced the documented build** in a fresh out-of-source dir
  (`hg_audit_build`, since removed): `cmake .. && make -j4 homeguardian_tests`
  → **135 cases / 1472 assertions pass; ctest 100%.** First configure fetched
  spdlog + nlohmann from GitHub (~105s) — network required once for the dep
  cache. Existing build dirs (`hg_build`, `hg_asan`, `build`, `build_final`) were
  NOT deleted; no concurrent clean build was run.
- **Fixed stale README.md:** it claimed "Phase A — no application code" and a
  nonexistent `src/` layout (`sensors/`, `inference/`, `pipeline/`, `server/`,
  `web/`, `scripts/`, top-level `main.cpp` misplacement), and listed `libcurl`
  and `MCP` as implemented. Corrected to the real layout, added the
  network-once dependency note, tag-filter test commands, and an explicit
  "no real capture" status. Alexa/MCP demoted to optional.
- **Fixed hardcoded absolute CMake include paths**
  (`/home/swarna-sekhar-dhar/...`) → `${CMAKE_SOURCE_DIR}` in the test target.
  Portable now; verified by the fresh reproduction.
- **Hardened `.gitignore`:** added `hg_*_build/` and `*.db`, `*.db-wal`,
  `*.db-shm`, `test_*.db` (no tracked `.db` files exist; verified).
- **Verified the app** (`homeguardian`) builds and runs a clean main loop with
  capture disabled by default; opens no sensor, writes no media.
- **Config audit:** `config/homeguardian.json` has `media_capture_enabled:
  false`, `cloud_processing_enabled: false`, and a reserved `data_dir` the
  current `Application` does not use (no privileged write path).

**Test results (this commit):**
- Fresh reproduction (`hg_audit_build`): 135 / 1472 pass; ctest 100%.
- `hg_build`: 135 / 1472 pass.
- `hg_asan` (ASan+UBSan, incremental rebuild): 135 / 1472 pass, zero sanitizer
  errors.

**Resolution:** Product-readiness audit landed (F22-22). Android backend, Linux
simulator, and Alexa skill preserved and untouched. No AWS resources,
credentials, kernel modules, permission changes, or physical sensors.

**Regression protection:** Full host + sanitizer suites green across three build
configurations.

**Remaining gaps (recommendations, not defects):** (1) the `data_dir` config is
unused — wire it into persistence or remove it when persistence is integrated
into the app; (2) no HTTP/MCP server exists yet (proposed, not implemented);
(3) README references `docs/DEVELOPMENT.md` (exists) — kept; (4) real-device
capture and real OS permission semantics remain unverified (hardware-gated).

---

## Non-Issues (Verified Working)

The following were verified to work correctly and are not friction:

- GitHub CLI (`gh`) authentication and private repository creation
- CMake 3.28.3 with Unix Makefiles generator
- g++ 13.3.0 with C++20
- spdlog v1.13.0 with console + file sinks
- nlohmann/json v3.11.3 for JSON serialization
- Catch2 v2.13.10 with `CATCH_CONFIG_MAIN`
- AddressSanitizer + UndefinedBehaviorSanitizer (clean)
- Git push to private GitHub repository
- Host V4L2/PipeWire/ALSA environment present (2 video nodes, PipeWire audio,
  Docker daemon running); current user not in `video`/`audio` groups

---

## Summary

| # | Incident | Severity | Status |
|---|----------|----------|--------|
| 1 | CMake FetchContent download timeout | Important | Resolved |
| 2 | Catch2 v2 CMake integration | Important | Resolved |
| 3 | Logger fallback masking init failures | Important | Resolved |
| 4 | Catch2 assertions in worker thread | Important | Resolved |
| 5 | spdlog logger name collision | Important | Resolved |
| 6 | Unused parameter warning | Nice-to-have | Resolved |
| 7 | APK launch crash — lambda invokedynamic (Android 8.1) | Important | Resolved (`b3bd012`) |
| 8 | APK launch crash — missing libc++_shared.so | Important | Resolved (`b3bd012`) |
| 9 | Camera test crashed process — uncaught C++ exception across JNI | Important | Resolved (`68e9360`) |
| 10 | NDK camera enumeration returns 0 devices on MT6580 (Camera1-shim HAL) | Important | Resolved via Camera1-JNI (Incident 11) |
| 11 | Camera1-JNI adapter implementation | — | Implemented; device capture awaiting approval (F22-11) |
| 12 | Ubuntu backend assessment + AVS deprecation finding | — | Non-capture discovery + host tests DONE; capture/Docker/Alexa proposed |
| 13 | Simulator-first backend + Docker fallback decision | — | Simulator landed (12 host tests); Docker deferred as unnecessary |
| 14 | Alexa custom skill prototype + simulator endpoint requirement | — | Handler + 13 unit tests DONE; console-simulator OPTIONAL (manual deploy steps) |
| 15 | Realistic Linux sensor simulation + Alexa demotion to optional | — | Config/signals/speaker landed (23 sim tests); Alexa optional, not a prerequisite |
| 16 | End-to-end Linux simulator workflow | — | 9 `[e2e]` tests; full host 135/1472 + ASan/UBSan clean; synthetic-only |
| 17 | Product-readiness audit (stale docs + CMake portability) | — | Fixed stale README + hardcoded CMake paths; hardened .gitignore; build reproduced fresh; 135/1472 across 3 configs |

**Total verified incidents:** 17
**Open incidents:** 3 (Incident 11 device-capture; Incident 12 capture adapters; Incident 14 console-simulator deploy [optional] — all pending manual/approval steps; root causes resolved)
**Resolved incidents:** 14
