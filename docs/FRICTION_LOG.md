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

## Incident 18: Dual Logical-Device Simulation + Shared Audio Broker

**Date/time:** 2026-10-11 ~04:10 IST
**Area:** Simulator / dual-device / audio broker / consent

**Expected:** Two independent logical devices sharing one Ubuntu mic source and
one speaker, with per-consumer consent, bounded/arbitrated output, and
deterministic shutdown — all fully simulated.

**Actual:**
- Added `src/backend/simulator/LogicalDevice.{h,cpp}`: identity, capabilities,
  lifecycle (Idle/Initialized/Active/Stopped/Error), bounded per-device event
  history (overwrite-oldest), and config. Devices are independent.
- Added `src/backend/simulator/AudioBroker.{h,cpp}`: ONE shared simulated mic
  (single capture session) fanning out to N consumers, each with its own
  `ConsentGate` re-evaluated at every fan-out; ONE bounded speaker output queue
  (default 64) with arbitration (no overlap). Speaker is an OUTPUT path, not
  gated by capture consent. Explicit injectable/recoverable playback errors;
  deterministic idempotent `shutdown()`.
- Added `tests/core/test_dual_device_broker.cpp`: 12 cases / 74 assertions —
  device independence + bounded history; shared-source fan-out to both
  consumers (single capture session: mic advanced once per fan-out, not per
  consumer); per-consumer denial/withdrawal; no-consumer-authorized → source
  stays quiet; speaker arbitration (no overlap); bounded queue overflow;
  playback failure + recovery; speaker independent of capture consent;
  deterministic shutdown (idempotent).

**Design decisions recorded:**
- Consumers do NOT drive the shared mic lifecycle; the broker owns the single
  capture session, and consumers only authorize delivery per fan-out. This
  avoids multiple consumers initializing/starting the same device (which the
  simulator forbids) and keeps a denied consumer from stopping the shared
  source. (An earlier draft had each consumer wrap the mic in a
  `ConsentGuardedDevice`; that double-initialized the shared mic and was
  replaced with direct per-consumer `ConsentGate::check`.)
- Speaker queue overflow rejects the NEW job (queue stays at capacity) rather
  than evicting the oldest, so a full queue is a clean back-pressure signal.
- A speaker job is removed from the queue only after a successful play, so a
  playback error leaves the job queued for retry after recovery.

**Test results (this commit):**
- Focused `[dual]`: 12 cases / 74 assertions pass.
- Full host suite: **147 cases / 1546 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, incremental): 147 / 1546 pass, zero sanitizer
  errors.

**Honesty note:** these logical devices do NOT run Amazon's Alexa OS and make
no Alexa-product claim; "Alexa-enabled" only means the logical device consumes
the shared mic and drives the speaker via the broker. No AWS, no Alexa skill
deployment, no physical sensor.

**Resolution:** Dual-device + shared audio broker landed (F22-23). Android
backend, Linux simulator, and Alexa skill preserved. No physical peripheral
activated, no kernel module, no permission change, no AWS/credentials.

**Regression protection:** 12 new `[dual]` tests; full host + sanitizer suites
green.

---

## Incident 19: ASan Build Corrupt Objects + Runtime Integration (F22-25)

**Date/time:** 2026-10-11 ~11:15 IST
**Area:** Build tooling / runtime integration

**Expected:** Integrate simulator components into the running Application and
run the full suite + sanitizers.

**Actual:**
- Added `core/SimulatorRuntime.{h,cpp}`: an in-process, simulator-only runtime
  (in-memory consent repo + `ConsentGate`, one shared `AudioBroker`, two
  independent `LogicalDevice` identities). No DB dependency; never opens real
  hardware.
- Wired it into `Application::run()`: applies config's `media_capture_enabled`
  kill switch; when enabled, grants synthetic consent, starts the single shared
  capture session, runs 5 deterministic media cycles (each fanning out one
  synthetic mic payload to authorized consumers and draining the bounded
  arbitrated speaker), and shuts down cleanly. When disabled (default), logs
  "no media delivered" and idles. `get_simulator_runtime()` exposes it.
- Added `tests/core/test_runtime_integration.cpp`: 7 cases / 40 assertions —
  capture-off delivers nothing; capture-on + consent delivers to permitted
  consumers; no-consent delivers nothing; withdrawing one consumer blocks only
  that consumer; multi-cycle determinism + bounded memory; deterministic
  idempotent shutdown; toggling capture off blocks delivery.

**Build-tooling issue (root-certain):** the `hg_asan` incremental build produced
**0-byte object files** for the two new translation units while `make` reported
"Built target" (success). The subsequent link silently produced a binary missing
the new tests (147 cases instead of 154). Diagnosed by observing
`Permission denied` on a 0-byte binary, then `ls -la` showing 0-byte `.o` files.
**Resolution:** removed the corrupt objects, recompiled, and relinked; verified
non-empty object + binary sizes and re-ran. Root cause is a stale/corrupt
incremental artifact, not a source defect. Lesson: after an incremental build,
verify the linked binary's test count matches expectations, not just make's exit
status.

**Test results (this commit):**
- Focused `[runtime]`: 7 cases / 40 assertions pass.
- Full host suite (hg_build): **154 cases / 1583 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, clean recompile of new objects): 154 / 1583
  pass, zero sanitizer errors.
- Application runs (simulator mode): capture-off → no media + clean shutdown;
  capture-on → delivers to both consumers each cycle, speaker_played=10 over 5
  cycles, queue drains to 0, clean shutdown.

**Resolution:** Runtime integration landed (F22-25). Simulator-only; no physical
sensor, no AWS, no Alexa change. Android backend and prior simulator components
preserved.

**Regression protection:** 7 new `[runtime]` tests; full host + sanitizer suites
green across a clean recompile.

**Remaining impact:** Simulator validates the running application's consent,
fan-out, bounded-buffer, arbitration, and shutdown paths, but does NOT prove
real V4L2/ALSA/PipeWire behavior or real OS permission semantics (hardware-
gated). No physical sensor was activated.

---

## Incident 20: Runtime Event-to-Alert Integration (F22-26)

**Date/time:** 2026-10-11 ~11:25 IST
**Area:** Runtime integration / event pipeline / alerts

**Expected:** Connect the simulator runtime to the application event pipeline so
AUTHORIZED synthetic observations pass through `Application::process_event()`,
validation, correlation, and alert generation in the running application.

**Actual:**
- Added an event sink to `SimulatorRuntime` (`set_event_sink`). `tick()` now
  routes each AUTHORIZED synthetic observation through the sink. `Application::
  run()` wires the sink to `process_event()`, so observations flow through the
  real Pipeline (validation + history + TimeWindowCorrelation + alert). No
  parallel event engine or duplicate correlation logic was added — the existing
  Pipeline and rule are reused unchanged.
- Added the app's correlation rule in `Application`'s constructor
  (`TimeWindowCorrelation`, min_count=2, 60s window, synthetic info alert), so
  the running app produces a deterministic synthetic alert once ≥2 authorized
  observations arrive.
- Added `tests/core/test_event_to_alert.cpp`: 9 cases / 34 assertions —
  authorized observations reach the pipeline and fire the rule; a single
  observation does not (no false alert); valid events pass validation and fire
  on min-count; a future-timestamp event is rejected by the validator without
  corrupting pipeline state; capture-disabled startup, missing consent, and
  consent withdrawal produce no events; repeated runs are deterministic;
  shutdown discards pending work.

**Test results (this commit):**
- Focused `[event2alert]`: 9 cases / 34 assertions pass.
- Full host suite (hg_build): **163 cases / 1617 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, stale objects for changed TUs removed +
  clean recompile; non-empty object/binary sizes verified): 163 cases pass,
  zero sanitizer errors (assertion count 1614 vs host 1617 = the known ±3
  timing-loop variance, not a failure).
- Running application (simulator mode): capture-on → 9 synthetic alerts logged,
  `pipeline_history=10`, bounded speaker, clean shutdown; capture-off → 0 alerts,
  no media, clean shutdown.

**Notes (behavior, not defects):**
- The `Event` constructor forbids empty id / source / non-object payload, so the
  validator's rejection path for those is unreachable via public factories; the
  invalid-event test instead exercises the validator's future-timestamp rule,
  which `Pipeline::process` honors by returning nullopt without mutating history.
- A second valid observation within the window correctly fires the rule — the
  earlier draft test asserted "no alert" for two valid events, which was wrong.

**Resolution:** Runtime event-to-alert integration landed (F22-26). Simulator-
only; no physical sensor, no AWS, no Alexa change. Android backend, simulator,
and Alexa skill preserved.

**Regression protection:** 9 new `[event2alert]` tests; full host + sanitizer
suites green across a clean recompile.

**Remaining impact:** This proves the running application's event→alert path on
synthetic data. It does NOT claim real-world detection: the alert is a synthetic
presence notice, not a danger inference. Real V4L2/ALSA/PipeWire behavior and
real OS permission semantics remain unverified (hardware-gated). No physical
sensor was activated.

---

## Incident 21: Runtime Hardening v1 — AlertPolicy + Lifecycle (F22-27)

**Date/time:** 2026-10-11 ~12:30 IST
**Area:** Runtime hardening / alert policy / lifecycle / config

**Expected:** Harden the running application: alert dedup/cooldown, explicit
lifecycle, config fail-closed, bounded memory, deterministic operation. Reuse
existing abstractions; no parallel runtime/pipeline/correlation engine.

**Actual:**
- Added `core/AlertPolicy.{h,cpp}`: dedup/cooldown keyed by rule_name+severity,
  first occurrence always delivered, cooldown re-notification, re-arm on
  condition clear, bounded retained state, injectable clock (backward-safe).
  Applied in `Application::process_event()`; unrelated alerts stay independent.
- Added explicit `AppState` lifecycle (`Constructed/Starting/Running/Stopping/
  Stopped/Failed`) with `state()`/`is_healthy()`; partial-startup failure tears
  down already-started components; config kill switch applied before components
  start; capture disabled by default; idempotent shutdown.
- Added `docs/DEMO_SIMULATOR.md` (reproducible simulator-only demo).
- Added tests: `test_alert_policy.cpp` (11 cases/45), `test_runtime_hardening.cpp`
  (17 lifecycle + 5 hardening = 22 cases). Config fail-closed cases reuse the
  existing `Config` validator.

**Root-cause analysis of the "9 alerts from 10 observations":** the existing
`TimeWindowCorrelation::evaluate` fires on EVERY evaluation where ≥2 observations
are in the 60s window. Feeding 10 observations one-by-one means evaluations 2–10
each see ≥2 in-window events → 9 alerts. This is the rule's **intended**
behavior (fire-per-evaluation), not a defect. `AlertPolicy` is the documented
policy layered on top to collapse repeated firings of one continuous condition
into a single notification with a re-notification cooldown.

**Build-tooling note:** the `Clock` is a `std::function`, which **copies** a
by-value `FakeClock`; time-advancing tests initially used a copy so `advance_ms`
did not affect the policy's clock. Fixed by sharing the clock via `shared_ptr`
capture. Also, `AlertPolicy` holds a `std::mutex` (non-movable), so it cannot be
returned in a `std::pair`; the test helper returns only the shared clock and the
policy is constructed in place. `SimulatorRuntime::shutdown()` is terminal by
design, so restart-after-shutdown tests use a fresh runtime per cycle.

**Test results (this commit):**
- Focused: `[alertpolicy]` 11/45, `[lifecycle]` 17/107, `[hardening]` 5/13 — all pass.
- Full host suite (hg_build): **190 cases / 1712 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, stale objects for changed TUs removed + clean
  recompile; non-empty object/binary sizes verified): 190 cases pass, zero
  sanitizer errors (1709 vs 1712 = known ±3 timing-loop variance).
- Running app demo: capture-off → no media/no alerts, `final_state=Stopped`;
  capture-on → 1 alert delivered + 8 dedup-suppressed, `pipeline_history=10`,
  bounded speaker, `final_state=Stopped`.

**Resolution:** Runtime hardening v1 landed (F22-27). Simulator-only; no physical
sensor, no AWS, no Alexa change. Android backend, simulator, and Alexa skill
preserved.

**Regression protection:** 33 new hardening cases; full host + sanitizer suites
green across a clean recompile.

**Remaining impact:** The alert is a synthetic presence notice, not a real-world
danger detection; `AlertPolicy` is a notification policy, not a safety detector.
Real device behavior and OS permission semantics remain unverified
(hardware-gated). No physical sensor was activated.

---

## Incident 22: Runtime Readiness v2 — Durable Audit + Observability (F22-28)

**Date/time:** 2026-10-11 ~17:35 IST
**Area:** Persistence / observability / restart semantics

**Expected:** Make the simulator-first runtime more observable, recoverable, and
privacy-conscious, reusing the existing SQLite persistence layer. Persist only
approved metadata; no parallel storage engine.

**Actual:**
- Added `core/PersistenceAudit.{h,cpp}`: durable audit of DELIVERED alert
  metadata via the EXISTING `SQLiteDatabase` + `SchemaManager` +
  `AlertRepository` (no parallel engine). Historical-vs-current-process
  distinguishability via a recorded process-start time; age-based retention
  (`prune_older_than`, transactional); fail-safe degraded on write failure;
  corrupt DB open throws (caller runs degraded, file not deleted).
- Added `core/RuntimeStatus.{h,cpp}`: bounded, privacy-safe in-memory counters
  (reset per process) + a single-line summary with no media/secrets/PII.
- Extracted `AppState` into `core/AppState.h` to break a circular include
  (`RuntimeStatus.h` ↔ `Application.h`).
- Wired both into `Application`: persistence opens at startup (degraded on
  failure), delivered alerts are persisted, status counters updated, summary
  logged at main-loop entry and clean shutdown.
- Added `tests/core/test_persistence_audit.cpp`: 7 `[audit]` + 3 `[status]` cases
  (persist+count, metadata-only, historical-vs-current, retention, write-failure
  degradation, corrupt-DB safety, schema, bounded counters, privacy-safe summary,
  degraded summary).

**Decisions (documented, not defects):**
- AlertPolicy dedup/cooldown state is intentionally NOT persisted: the
  simulator-only lifecycle treats each process start as a fresh condition;
  persisting cooldown has no concrete requirement and would add complexity.
- Consent is never persisted as authorization; it fails closed after restart.
- `record_alert` after `close()` is treated as a write failure (degraded), not a
  silent success.

**Build-tooling note:** the `Clock`-copy and non-movable-`AlertPolicy` issues
from Incident 21 did not recur. Followed the established protocol: removed stale
objects for changed TUs in `hg_asan`, recompiled, and verified non-empty
object/binary sizes before running.

**Test results (this commit):**
- Focused: `[audit]` 7/28, `[status]` 3/13 — all pass.
- Full host suite (hg_build): **200 cases / 1753 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, stale objects removed + clean recompile; sizes
  verified): 200 cases pass, zero sanitizer errors.
- Release reproduction: fresh out-of-source build (existing dep cache) → 200/1753,
  new tests confirmed present in the fresh binary.
- Running app: capture-on → `Persistence ready. historical_alerts=0`, alert
  generated + persisted, status summary logged, `final_state=Stopped`. Restart
  (same DB) → `historical_alerts=1` (distinguishable). Capture-off → no media,
  no alerts, `final_state=Stopped`.

**Resolution:** Runtime readiness v2 landed (F22-28). Simulator-only; no physical
sensor, no AWS, no Alexa change. Android backend, simulator, and Alexa skill
preserved. Temp release-verify dir removed; existing build dirs intact.

**Regression protection:** 10 new `[audit]`/`[status]` tests; full host +
sanitizer suites green across a clean recompile.

**Remaining impact:** Persisted data is alert metadata only — no raw media,
biometrics, or consent-as-authorization. Real device behavior and OS permission
semantics remain unverified (hardware-gated). No physical sensor was activated.

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
| 18 | Dual logical-device simulation + shared audio broker | — | 12 `[dual]` tests; full host 147/1546 + ASan/UBSan clean; simulated-only, no Alexa OS claim |
| 19 | ASan build corrupt objects + runtime integration | — | Fixed 0-byte incremental objects; SimulatorRuntime wired into Application; 7 `[runtime]` tests; 154/1583 host + ASan/UBSan |
| 20 | Runtime event-to-alert integration | — | Authorized observations → process_event → Pipeline → alert; 9 `[event2alert]` tests; 163/1617 host + ASan/UBSan; running app fires 9 synthetic alerts |
| 21 | Runtime hardening v1 — AlertPolicy + lifecycle | — | Alert dedup/cooldown/re-arm + AppState lifecycle + config fail-closed; 33 hardening cases; 190/1712 host + ASan/UBSan; demo docs/DEMO_SIMULATOR.md |
| 22 | Runtime readiness v2 — durable audit + observability | — | PersistenceAudit (alert metadata via existing SQLite, historical-vs-current, retention, fail-safe degraded) + RuntimeStatus (bounded privacy-safe counters); 10 audit/status cases; 200/1753 host + ASan/UBSan; release reproduction |
| 23 | Runtime readiness v3 — integration contract, storage integrity, privacy | — | AlexaContract (versioned, authenticated, read-only contract; 14 C++ + 12 Node adapter cases, NOT deployed); SQLite integrity/migration/retention/recovery (7 cases); privacy/consent regression (7 cases); P1 rule-specific clearing; P2 scheduled retention; 235/1925 host + ASan/UBSan; release reproduction |

**Total verified incidents:** 28
**Open incidents:** 3 (Incident 11 device-capture; Incident 12 capture adapters; Incident 14 console-simulator deploy [optional] — all pending manual/approval steps; root causes resolved)
**Resolved incidents:** 25

---

## Incident 28: Hackathon P0 — Honest README + Simulated Alexa+ Web Demo (F22-35)

**Date/time:** 2026-10-12 ~13:20 IST
**Area:** Hackathon submission readiness / web demo / build reproducibility

**Expected:** Implement P0 from the AmazonAppDev 2026 compliance audit
(`docs/AMAZONAPPDEV2026_FINAL_AUDIT.md`): correct README overclaims, build a
simulated Alexa+ web experience on the real backend, and make the demo
reproducible from a clean clone. LICENSE deliberately NOT added (not approved).

**Actual:**
- **P0-1 README corrected.** Removed the false claims "Alexa+ … with MCP
  integration" and "Ring integration" (neither exists — verified by code
  search). Added an explicit "What is implemented vs simulated vs planned"
  section, a hackathon banner stating this is a simulated Alexa+ experience
  (no MCP, no live Amazon identity, no physical sensor), and accurate build/
  test commands including `hg_alexa_bridge`.
- **P0-3 simulated Alexa+ web experience.** Added:
  - `src/bridge/hg_alexa_bridge.cpp`: a `run_scenario [cycles]` control line
    that runs the REAL synthetic household scenario through the REAL
    Application pipeline (SimulatorRuntime → event sink → Pipeline →
    correlation → AlertPolicy) and persists the delivered alert to the same
    audit DB the status provider reads. No second event engine; no physical
    sensor. Logger initialized at `critical` so stdout carries only NDJSON
    (previously a `warn`-level alert line interleaved with the protocol and
    broke machine parsing).
  - `src/bridge/hg_demo_server.js`: a loopback-only static server + JSON proxy
    (Node stdlib only, no new dependency) exposing `/api/status`,
    `/api/run-scenario`, `/api/health` and serving `src/web/`.
  - `src/web/index.html`: a polished single-page UI with branding, a prominent
    "simulated" banner, live status/capture state, a Run-scenario button,
    suggested-question chips, an alert timeline, loading/empty/error states,
    and a privacy explainer. All values are read from the backend.
  - `demo.sh`: one-command startup (builds the bridge if needed, generates
    isolated 0600 test credentials in a temp dir, runs from that dir so the
    SQLite DB never lands in the repo, prints the URL, cleans up on exit).
  - `config/homeguardian.demo.json`: capture-on demo config (default stays off).
  - `docs/DEMO_WEB.md`: walkthrough, expected output, troubleshooting, and a
    suggested <3-minute video script.
- **P0-4 reproducibility.** Fixed two real bugs found by building out-of-source
  in `/tmp`:
  1. The `homeguardian_tests` CMake target used `../src/...` relative paths,
     which only resolve when the build dir is a direct child of the source
     tree. Converted all 62 to `${CMAKE_SOURCE_DIR}`-absolute.
  2. `test_config_e.cpp` "Shipped default config…" read
     `config/homeguardian.json` relative to the CWD, so it failed for any
     direct binary run outside the repo root (pre-existing; reproduces in
     `hg_build`). Now resolves via a new `HG_SOURCE_DIR` env var that ctest
     sets.
- Added `alexa_skill/test/web_demo.test.js` (8 cases) and wired all six Node
  test files into `npm test`.

**Decisions (documented):**
- **No LICENSE file created** — MIT was not approved. The repo is already
  PUBLIC with `licenseInfo: null`; this remains the top submission blocker and
  is a user-only task.
- Repository visibility was inspected read-only (`gh repo view` → PUBLIC) and
  **not changed**.
- The web demo reuses the existing C++ backend and contract; it does not
  duplicate consent/security logic, bypass the contract, or touch SQLite
  directly.

**Friction encountered:**
- The first scenario run reported `events_processed: 0` with capture on: the
  bridge's `SimulatorRuntime` is constructed disabled by `Application`'s
  constructor, so its internal gate never saw the config. Fixed by calling
  `sim.set_media_capture_enabled(true)` in the scenario (mirroring
  `Application::run()`).
- `run_scenario` then aborted with "Logger not initialized" because
  `process_event` logs. Fixed with `Logger::initialize("critical")`.
- Alerts were generated but `alert_count` stayed 0: `process_event` persists
  only when `Application::run()` has set up persistence, which the bridge never
  calls. Fixed by persisting the delivered alert explicitly in the scenario.
- A `warn`-level log line interleaved with NDJSON on stdout, so the demo server
  parsed `bad_output`. Fixed by the `critical` log level plus a defensive
  non-JSON-line filter in the demo server.
- The fresh out-of-source build initially failed (the two bugs above); after
  the fixes a build in `/tmp` passes 255/2001 + ctest 100%, and the web demo
  ran end-to-end on that fresh binary.

**Test results (this cycle):**
- C++ full host suite (hg_build): **255 cases / 2001 assertions pass; ctest
  100%.** (Passes from the repo root AND from a non-root CWD with
  `HG_SOURCE_DIR` set — the isolation bug is fixed.)
- ASan/UBSan (hg_asan, cmake reconfigure + stale objects removed + clean
  recompile): 255 cases, zero sanitizer errors.
- Node full (`npm test`): **76/76** (13 handler + 12 contract + 21 integration +
  4 restart + 18 http_adapter + 8 web_demo).
- Fresh out-of-source build in `/tmp/hg_fresh` (existing dep cache): configure,
  build, and suite all pass (255/2004 with `HG_SOURCE_DIR`, ctest 100%); the
  web demo on that fresh binary reported 10 events, 9 alerts, 1 delivered,
  8 suppressed, and a persisted `sim-alert-0`.
- Manual launch of `bash demo.sh` verified: server ready, loopback reachable,
  **LAN IP refused** (loopback-only confirmed), UI served (HTTP 200).
- Secret/artifact scan: clean; no stray DB/log files in the repo.

**Resolution:** Hackathon P0-1, P0-3, P0-4 landed (F22-35). Simulator-only;
loopback-only; no cloud, no physical sensor, no Alexa deploy, no visibility
change. Build dirs preserved; temp build dir removed.

**Regression protection:** 8 new `web_demo` Node cases (scenario, status,
capture-off fail-closed, UI served, path traversal, minimal health, loopback
bind, graceful shutdown) plus the two build-isolation fixes; full host +
sanitizer suites green across a clean recompile and a fresh out-of-source
build.

**Remaining impact:** P0-2 (LICENSE) is still open pending user approval, and
the repo is public without one. Devpost submission, the demo video, and product
feedback remain user-only tasks. No physical sensor was activated.

---

## Incident 27: HTTP Adapter v7 — Local-Only Loopback Adapter (F22-33)

**Date/time:** 2026-10-12 ~02:20 IST
**Area:** Alexa integration / local HTTP adapter / transport security

**Expected:** Build a local-only HTTP adapter around the existing AlexaContract
and AlexaCredentialStore with transport-level protections and regression tests.
Development/integration milestone, NOT production.

**Actual:**
- Added `src/bridge/hg_http_adapter.js`: a loopback-only HTTP front-end that
  binds `127.0.0.1` exclusively (asserts bound address; exits if not loopback)
  and forwards each request to the REAL compiled `hg_alexa_bridge` over NDJSON
  stdio. Uses the Node **stdlib `http` module — no new dependency**. No auth/
  authorization logic duplicated (all in the C++ contract + credential store).
- Transport controls: method allowlist (GET/OPTIONS), route allowlist
  (/status, /alert_summary, /health), body/header size bounds, request/header/
  keep-alive/bridge-call timeouts, concurrency cap (8), fixed-window rate limit
  (30 req/IP/10s, table bounded 1024), content-type allowlist, strict single
  `Bearer <token>` parse, generic 401 (never reveals why), graceful SIGINT/
  SIGTERM shutdown, no disk persistence, no shell (argument-array spawn).
- Added `alexa_skill/test/http_adapter.test.js`: 18 cases — authorized status/
  alert_summary; missing/invalid/expired/revoked → 401; authenticated-but-
  unauthorized → 403; wrong method → 405; unknown route → 404; invalid content
  type → 415; ambiguous/malformed Authorization → generic 401 (no token leak);
  oversized headers rejected + server survives; rate limiting (some 429, some
  200, server responsive); /health minimal state (no token/path); loopback-only
  (TCP connect to LAN IP fails); no token leakage; graceful SIGTERM exit;
  multiple sequential requests.
- Wired into `npm test`.

**Decisions (documented):**
- Chose Node stdlib `http` over adding cpp-httplib or hand-writing a parser:
  the milestone prohibits silently adding an unreviewed dependency and
  hand-writing a fragile parser. cpp-httplib was evaluated and deliberately NOT
  added.
- The adapter does NOT call TLS; loopback HTTP is not end-to-end TLS. Documented
  as controlled-development-only; non-local deployment requires reviewed HTTPS.
- Node's HTTP server de-dupes the Authorization header at the parser level
  (keeping the first), so a literal duplicate does not reach the handler as
  ambiguous. The real control is the adapter's strict single-token parse, tested
  directly against ambiguous/malformed values.

**Friction encountered:** first duplicate-header test expected 400 but Node
keeps the first Authorization value (→ 200 for a clean token); also a
whitespace-only case legitimately normalizes to a valid token. Reworked the test
to assert the actual control (strict parse → generic 401) and removed the
whitespace-only false case.

**Test results (this cycle):**
- HTTP adapter: **18/18**.
- Node full (`npm test`): **68/68** (13 handler + 12 contract + 21 integration +
  4 restart + 18 http_adapter).
- C++ full host suite (hg_build): **255 cases / 2004 assertions; ctest 100%.**
  (No new C++; the adapter is exercised via Node against the real binary.)
- ASan/UBSan: 255 cases, zero sanitizer errors.
- Fresh out-of-source release build (existing dep cache): app + tests + bridge
  clean; suite 255/2004.

**Resolution:** HTTP adapter v7 landed (F22-33). Simulator-only; loopback-only,
no public/LAN listener, no cloud, no TLS, no physical sensor. Existing stdio
bridge and all interfaces preserved. Build dirs intact; temp release dir
removed.

**Regression protection:** 18 new Node cases drive the real compiled bridge over
real HTTP (auth matrix, routes, rate limit, loopback bind, shutdown, no-leak).

**Remaining impact:** NOT TLS, NOT production. Non-local exposure is NO-GO
pending B2 (TLS), real LWA/OAuth identity validation, secret rotation
operations, production-scale rate limiting, and supervision. Genuine Alexa
identity verification remains unimplemented. No physical sensor was activated.

---

## Incident 26: Security Readiness v6 — Authentication & Secret Management (F22-32)

**Date/time:** 2026-10-12 ~01:50 IST
**Area:** Security / authentication / secret management

**Expected:** Implement blockers B1 (real credential verification) and B4
(secret management) from `docs/SECURITY_READINESS_V5.md`, isolated, before any
HTTP endpoint. Simulator-only; no production credentials.

**Identity-flow decision:** the repo documents the intended production flow as
Alexa account linking via OAuth 2.0 / Login with Amazon (`docs/ARCHITECTURE.md`).
Validating a real LWA token needs an LWA client id/secret (Amazon credentials),
which are prohibited here. So the LWA provider is NOT implemented; instead the
provider-independent verification/security layer is implemented, with a future
LWA/signed-request verifier slotting into the unchanged
`IAlexaCredentialVerifier` interface.

**Actual:**
- Added `core/AlexaCredentialStore.{h,cpp}`: real bearer-token verifier +
  secure secret loader. Fail-closed on missing/unreadable/malformed/
  insecurely-permissioned file, unknown/expired/revoked token (all failure
  modes indistinguishable). Constant-time byte comparison (accumulate XOR, no
  early exit; length mismatch fails immediately). Secure-file load requires a
  regular file, 0600, owned by the current euid; any deviation → load fails,
  store empty, every verify unauthorized. Secret values never logged/returned;
  load-failure reason is generic (no path/value). Runtime in-memory `revoke()`.
- Wired into `hg_alexa_bridge`: `HG_BRIDGE_SECURE_CREDS=1` selects the secure
  store (load failure → exit 3, never serving with a weaker verifier); default
  remains the local dev/test verifier. Mode chosen explicitly via env var, never
  silently. No permissive fallback.
- Added `tests/core/test_alexa_credentials.cpp`: 14 `[creds]` cases (0600 load,
  broad-perms rejected, missing/malformed/non-object/empty-token rejected,
  valid/unknown/expired/revoked, runtime revoke, constant-time equality, no
  fallback, contract+store authn-then-authz).
- Added 5 secure-mode Node integration cases (0600 auth, revoked→unauthorized,
  expired→unauthorized, 0644→exit 3 no-serve, no token in stderr).

**Decisions (documented):**
- No custom crypto / no new dependency (milestone prohibits both; OpenSSL dev
  headers absent on host). Constant-time byte equality implemented directly.
- Persistent revocation/rotation and secret issuance are OPERATIONAL procedures
  (re-provision the file out-of-band), not runtime features.
- `add_for_testing` normalizes a zero/epoch expiry to "never" so fixtures need
  not set max(); this keeps the test path consistent with the file loader.

**Friction encountered:** first `[creds]` run failed one case because a
default-constructed `expires_at` (epoch) read as already-expired via
`add_for_testing`. Fixed by normalizing zero/epoch expiry to max() in
`add_for_testing`.

**Test results (this cycle):**
- C++ full host suite (hg_build): **255 cases / 2004 assertions pass; ctest
  100%.**
- ASan+UBSan (hg_asan, cmake reconfigure + stale objects removed + clean
  recompile, sizes verified): 255 cases, zero sanitizer errors.
- Node full (`npm test`): **50/50** (13 handler + 12 contract + 21 integration +
  4 restart).
- Fresh release build (existing dep cache): app + tests + bridge clean; suite
  255/2004; `[creds]` present in the fresh binary.
- Secret/artifact scan: clean; no stray credential/db files.

**Resolution:** Security readiness v6 landed (F22-32). Simulator-only; no
endpoint, no cloud, no production credentials, no physical sensor. LWA provider
NOT implemented (needs Amazon creds). Build dirs intact; temp release dir
removed.

**Regression protection:** 14 `[creds]` + 5 secure-mode Node cases; full host +
sanitizer suites green across clean recompile and fresh release build.

**Remaining impact:** B2 (TLS), B3 (rate limiting), B5 (health/supervision)
still open; the next local-only HTTP-adapter stage is CONDITIONAL on those.
Real disk-full, real hardware capture, and live Alexa integration remain
unverified. No physical sensor was activated.

---

## Incident 25: Security Readiness v5 — Deployment Review of the Alexa Boundary (F22-31)

**Date/time:** 2026-10-11 ~23:10 IST
**Area:** Security / deployment readiness / Alexa boundary

**Expected:** Evidence-based deployment-readiness and security review of the
read-only Alexa contract + local bridge, building on v4. Determine what a future
authenticated HTTPS endpoint would require, without deploying it.

**Actual:**
- Performed a security review (authn/authz, request/process bounds, persistence/
  privacy, build/supply-chain). Full report: `docs/SECURITY_READINESS_V5.md`.
- **Fixed LOW-1:** `hg_alexa_bridge` read `config_path` from argv but never
  loaded it, so `capture_enabled` always reflected the `RuntimeStatus` default
  (false) even with a capture-on config (verified by running the binary). Fixed
  to load the real config and reflect the actual kill-switch state; a config
  load failure fails safe (capture reported disabled, bridge keeps serving).
  This is a read-only reflection — the bridge still cannot enable capture.
- Added 6 C++ `[alexa]` security-regression cases (nesting bomb, non-object
  top-level, trailing data, wrong field types, client-supplied identity claims
  ignored, credential never in response) and 2 Node integration cases (config
  capture-state reflection + config-load-failure fail-safe).

**Verified controls (evidence in the report):** fail-closed auth (missing/
invalid/expired/unknown → unauthorized); authenticated-but-unauthorized →
forbidden; client-supplied identity claims ignored; read-only ops only (no
mutation path); request size bound; malformed/oversized/nested/trailing-data
rejected; no credential leak in output; no network listener; no shell execution
(argument-array spawn); bounded process lifetime + clean exit; restart does not
restore consent/capture; historical ≠ current alerts; summaries only (no raw
media/PII); build hygiene (`-Wall -Wextra -Wpedantic`, C++20, Release); no
install hooks; no committed secrets/keys/db/binaries.

**Unverified / blockers (B1–B5):** no real authentication mechanism (test
verifier only); no TLS termination; no rate limiting/concurrency control; no
secret management/rotation; no health endpoint/supervision. Real disk-full,
real hardware capture, and live Alexa integration remain unverified.

**Recommendation:** HTTPS endpoint = **CONDITIONAL GO**, blocked pending B1–B5.
The domain contract is deployment-ready behind an HTTP adapter; transport/auth/
operational controls do not exist yet. The local stdio bridge must NOT be
network-exposed.

**Test results (this cycle):**
- C++ full host suite (hg_build): **241 cases / 1960 assertions pass; ctest
  100%.**
- ASan+UBSan (hg_asan, stale objects removed + clean recompile, sizes verified):
  241 cases, zero sanitizer errors. (Note: a transient parallel-build error
  occurred after removing an object mid-build; a clean rebuild succeeded.)
- Node full (`npm test`): **45/45** (13 handler + 12 contract + 16 integration +
  4 restart).
- Fresh release build (existing dep cache): app + tests + bridge clean; suite
  241/1960; `[alexa]` group present in the fresh binary.
- Secret/artifact scan: clean.

**Resolution:** Security readiness v5 landed (F22-31). Simulator-only; no
physical sensor, no cloud, no endpoint deployed, no credentials created. Temp
release-verify dir removed; existing build dirs intact.

**Regression protection:** 8 new cases (6 C++ `[alexa]` + 2 Node integration)
plus the LOW-1 fix; full host + sanitizer suites green; fresh release build
green.

**Remaining impact:** The local stdio bridge is safe for its single-client,
process-spawned purpose but is not a deployable network service. B1–B5 must be
implemented and tested before any HTTPS exposure. No physical sensor was
activated.

---

## Incident 24: Runtime Readiness v4 — Local End-to-End Integration Proof (F22-30)

**Date/time:** 2026-10-11 ~21:30 IST
**Area:** Alexa integration / process boundary / restart

**Expected:** Prove the JavaScript adapter and the actual C++ AlexaContract
interoperate across a real local process boundary — not just separate unit
tests and hand-written mocks. Simulator-only; no deployment.

**Actual:**
- Added `src/bridge/hg_alexa_bridge.cpp` + CMake target `hg_alexa_bridge`: a
  small LOCAL-ONLY executable exposing the REAL `AlexaContract` over newline-
  delimited JSON (NDJSON) stdio. No network listener. Each request line is
  parsed/handled by `AlexaContract::parse` + `handle` (shared validation, version
  gating, fail-closed auth). Status/alert data come from the real
  `RuntimeStatus` + `PersistenceAudit`.
- Added `alexa_skill/test/integration.test.js`: 14 process-boundary cases that
  SPAWN the actual compiled binary (argument array, no shell; output bound;
  timeout kill) and exercise authenticated status/alert_summary, missing/invalid/
  expired/unauthorized credentials, malformed JSON, unknown field, oversized
  request, unsupported version, unsupported (mutating) operations, multi-request
  ordering, and clean exit.
- Added `alexa_skill/test/restart.test.js`: 4 cases driving real processes —
  run the actual app to produce a synthetic alert into a temp DB, start a fresh
  bridge against the same data and verify the historical alert is visible; a
  fresh bridge reports zero; a missing credentials file fails closed; bad usage
  exits 2.
- Added `docs/demo_alexa_integration.sh`: reproducible demo (authenticated
  status + alert-summary, a rejected request, an unavailable/forbidden response),
  isolated temp workspace, test-only credentials, cleanup on exit.
- Wired all four Node test files into `npm test`.

**Decisions (documented, not defects):**
- Chose NDJSON stdio over a local HTTP server: no existing architecture requires
  a listener, and stdio avoids network/permission surface. Documented.
- The bridge does not call `Application::run()` (which blocks in the main loop);
  it builds a read-only status view over the real `RuntimeStatus` +
  `PersistenceAudit`, so availability reflects real persistence health without
  duplicating lifecycle logic. Capture stays disabled.
- Live Alexa-to-application integration remains NOT deployed/tested (needs an
  authenticated endpoint) — documented as PENDING. No claim of live integration.

**Friction encountered:**
- First bridge build failed to link (undefined Application/PersistenceAudit
  symbols) because the target compiled only its own TU. Fixed by compiling the
  real core sources into the bridge target (no new library, no duplicated logic).
- First smoke test returned `unavailable` for status/alert_summary because the
  provider checked `Application::state()`, which stays `Constructed` when
  `run()` is not called (no public setter). Fixed by basing availability on real
  persistence health (the bridge is the live view) rather than the unstarted
  main-loop state.

**Test results (this cycle):**
- Node process-boundary: `integration.test.js` 14/14, `restart.test.js` 4/4.
- Node full (`npm test`): **43/43** (13 handler + 12 contract + 14 integration +
  4 restart).
- C++ full host suite (hg_build): **235 cases / 1928 assertions pass; ctest
  100%.** (No new C++ test cases; the bridge is exercised via Node.)
- ASan+UBSan full suite: 235 cases, zero sanitizer errors.
- Release reproduction: fresh out-of-source build (existing dep cache) →
  app + tests + bridge build clean; fresh-repro suite 235/1928; bridge smoke
  against the fresh binary returns real state + fail-closed auth.
- Demo: `docs/demo_alexa_integration.sh` runs end-to-end (authenticated status +
  alert-summary, unauthorized, forbidden, unavailable), exit 0, temp workspace
  removed.

**Resolution:** Runtime readiness v4 landed (F22-30). Simulator-only; no
physical sensor, no AWS, no Alexa deploy, no network listener. Android backend,
simulator, and Alexa skill preserved. Temp release-verify dir removed; existing
build dirs intact.

**Regression protection:** 18 new Node cases (14 integration + 4 restart) drive
the real compiled binary; full C++ host + sanitizer suites green; demo
reproducible.

**Remaining impact:** Live Alexa integration, real filesystem-exhaustion
behavior, and real V4L2/ALSA/PipeWire capture remain unverified (hardware-/
deployment-gated). No physical sensor was activated. The Node integration tests
prove the JS↔C++ boundary over stdio; they do not prove a deployed HTTPS
endpoint.

---

## Incident 23: Runtime Readiness v3 — Integration Contract, Storage Integrity & Privacy (F22-29)

**Date/time:** 2026-10-11 ~19:45 IST
**Area:** Alexa integration contract / SQLite integrity / privacy & consent

**Expected:** Verify three boundaries — the Alexa skill's interface to the
application, SQLite schema compatibility/integrity/retention/recovery, and
privacy/consent guarantees across lifecycle transitions and restart. Reuse
existing abstractions; simulator-only; no deployment.

**Actual:**
- Added `core/AlexaContract.{h,cpp}`: a versioned (v1), authenticated,
  READ-ONLY integration contract. Operations: `status`, `alert_summary` only —
  no operation can enable capture, grant consent, change config, or activate a
  sensor. Auth boundary via injected `IAlexaCredentialVerifier`; a
  client-supplied identity is never trusted. Fail-closed: missing/invalid/
  expired → `unauthorized`; authenticated-not-permitted → `forbidden`; degraded
  app → `unavailable`. Bounded requests (≤4KB), known-fields-only, unsupported
  version rejected. Responses carry only non-sensitive summary fields; errors
  are generic.
- Added `alexa_skill/test/contract.test.js`: 12 Node adapter-boundary cases with
  a mock client mirroring the C++ contract semantics (no deploy). Wired into
  `npm test`. Existing 13 skill-handler tests unchanged and still pass.
- Added `tests/persistence/test_integrity.cpp`: 7 `[integrity]` cases — v1→v2
  migration preserves alert metadata/ids/events/indexes + PK constraint; repeat
  migration idempotent; failed migration rolls back (version stays 1); reopen
  after clean shutdown; retention deletes only expired (newer + unrelated-table
  kept); write-after-close fails loudly; corrupt DB file preserved not deleted.
- Added `tests/core/test_privacy_consent.cpp`: 7 `[privacy]` cases —
  capture-disabled delivers no media despite grants; withdrawal blocks only the
  affected consumer; unauthorized observations never reach the event sink;
  restart does not restore capture authorization; persisted alerts contain only
  documented metadata (no audio/video/payload/biometric); per-process counters
  start at zero while audit history persists as `historical`; retention removes
  only expired alerts.
- Also landed this cycle (from the P1–P5 task): P1 rule-specific condition
  clearing via `Pipeline::rule_names()` (no hardcoded `"sim-presence"`; 3
  `[clearing]` tests) and P2 scheduled retention via configurable
  `alert_retention_days` + `PersistenceAudit::maybe_prune_retention` (4
  `[retention]` tests; startup prune + bounded 10-min periodic prune).

**Decisions (documented, not defects):**
- Live Alexa-to-application integration is NOT tested: it requires a deployed,
  authenticated endpoint. Documented as PENDING. No claim of live integration.
- Storage-write failure is simulated by a closed DB (documented as distinct
  from real filesystem exhaustion). No real disk-full is simulated.
- Retention bounds logical row count; the SQLite file's physical size is not
  shrunk (no VACUUM — deleted pages may remain allocated). Reported separately
  per the milestone note.

**Build-tooling note:** followed the established ASan protocol — removed stale
objects for changed/new TUs in `hg_asan`, recompiled, verified non-empty
object/binary sizes, then ran. `std::variant` has no `operator*`; added a
`parse_ok` test helper instead of dereferencing.

**Test results (this cycle):**
- Focused: `[alexa]` 14/70, `[integrity]` 7/39, `[clearing]` 3/17,
  `[retention]` 7/34, `[privacy]` 16/66 — all pass. Node: contract 12/12,
  handler 13/13.
- Full host suite (hg_build): **235 cases / 1925 assertions pass; ctest 100%.**
- ASan+UBSan full suite (hg_asan, clean recompile, sizes verified): 235 cases,
  zero sanitizer errors.
- Release reproduction: fresh out-of-source build (existing dep cache) →
  235/1925, new groups confirmed present in the fresh binary.
- Simulator demo: capture-off → no media/alerts + final_state=Stopped;
  capture-on → alert generated + persisted + 8 dedup-suppressed, final_state=Stopped.

**Resolution:** Runtime readiness v3 landed (F22-29). Simulator-only; no
physical sensor, no AWS, no Alexa deploy. Android backend, simulator, and Alexa
skill preserved. Temp release-verify dir removed; existing build dirs intact.

**Regression protection:** 35 new C++ cases + 12 Node adapter cases; full host +
sanitizer suites green across a clean recompile and a fresh release build.

**Remaining impact:** Live Alexa integration, real filesystem-exhaustion
behavior, and real V4L2/ALSA/PipeWire capture remain unverified (hardware-/
deployment-gated). No physical sensor was activated.

## Detection framework build failure — aggregate-init default argument (2026-10-12)

**Symptom:** After adding `Observation`, `AnomalyDetector`, `FusionEngine`, and
`SyntheticActivityDetector`, `make homeguardian` failed with:
`error: default member initializer for '…::Config::min_quality' required before
the end of its enclosing class`.

**Root cause:** A constructor default argument of the form
`explicit Foo(Config cfg = Config{})` (and `= Config()`) is parsed in the
enclosing class scope, where the *nested* `Config`'s default member initializers
are not yet usable. This is the known C++ aggregate-init / default-argument
ordering restriction (ill-formed: NSDMIs not yet complete in the class's own
default argument). It is not a missing-semicolon or a member-ordering bug — the
struct was already declared above the constructor.

**Fix:** Give each nested `Config` an explicit `Config() = default;` and expose a
`static Config defaults() { return Config{}; }`; use
`explicit Foo(Config cfg = defaults())`. `defaults()` is a member function
evaluated at call time, after the class is complete, so it sidesteps the
restriction cleanly and is self-documenting.

**Evidence:** After the change, `make -j4 homeguardian homeguardian_tests` built
clean (exit 0, no warnings), and the full suite passed (275 cases / 2071
assertions; ctest 100%; Node 76/76).

**Lesson:** When a class exposes a nested `Config` used as a constructor default
argument, prefer a `static defaults()` helper over inline `Config{}`/`Config()`
default arguments — the inline form trips the NSDMI completeness rule on some
GCC versions.

## Stale ASan build reported pre-v8 counts (2026-10-12, v9 stabilization)

**Symptom:** During the v9 stabilization pass, `hg_asan/homeguardian_tests`
reported **255 cases / 2004 assertions** while the current source (`hg_build`)
reported **275 / 2071**. The sanitizer binary had not been reconfigured after the
v8 detection work, so it was testing stale object code.

**Root cause:** incremental build directories are not auto-reconfigured when new
source files are added to `CMakeLists.txt`; `hg_asan` predated the v8 detection
sources.

**Fix / procedure:** reconfigure and clean-recompile the sanitizer build from
current source before trusting its result:
`cd hg_asan && cmake . && make -j4 homeguardian_tests`, then run with
`HG_SOURCE_DIR=<repo>` set. After reconfirmation the ASan/UBSan suite passes on
current source (275/2071, zero sanitizer errors).

**Lesson:** before citing a sanitizer (or any non-default build-dir) result,
reconfigure it from current source and confirm the case count matches the
primary build; otherwise the result describes old code.

## Direct out-of-source run needs HG_SOURCE_DIR (2026-10-12, v9 stabilization)

**Symptom:** running a freshly built `homeguardian_tests` **directly** from an
out-of-source build dir (e.g. `/tmp/hg_v9`) failed one case:
`test_config_e.cpp:97` — "Shipped default config has media capture and cloud
disabled" — `REQUIRE(f.is_open())` failed because `config/homeguardian.json`
was not found relative to the build dir and `HG_SOURCE_DIR` was unset.

**Root cause:** that test resolves the shipped config relative to CWD, then falls
back to the `HG_SOURCE_DIR` environment variable. `ctest` sets `HG_SOURCE_DIR`
(test property), so the documented `ctest` path passes; a bare binary run from an
arbitrary out-of-source dir does not.

**Resolution (no code change needed):** this is an invocation detail, not a
source defect. Verified: `ctest` in the fresh build passes 275/2071, and a
direct run passes 275/2071 once `HG_SOURCE_DIR=<repo>` is exported.

**Documented for reproducibility:** the reliable commands are
`ctest --test-dir <build>` (sets HG_SOURCE_DIR automatically) or
`HG_SOURCE_DIR=<repo> <build>/homeguardian_tests`. Recorded in
`docs/DEVELOPMENT.md` so a judge reproducing results from a clean clone uses the
working path.

## Demo bypassed the v8 detection framework (2026-10-12, v10)

**Symptom:** Runtime Readiness v8 built and tested `Observation`,
`AnomalyDetector`, `FusionEngine`, and `SyntheticActivityDetector`, but the demo
journey (`run_demo_scenario` in `src/bridge/hg_alexa_bridge.cpp`) fed simulator
events straight into the pipeline and never invoked the detection layer. From a
judge's perspective the v8 code was unreachable dead code — the
`Observation -> Detection/Fusion -> Event pipeline` link was severed.

**Fix:** `run_demo_scenario` now runs `SyntheticActivityDetector` + `FusionEngine`
over a deterministic energy sequence (incl. one near-threshold value that
correctly resolves to `inconclusive`, not a fabricated detection), feeds each
resulting Observation through the EXISTING pipeline via `to_event()` ->
`process_event()` (no second event engine), and emits a `detection` summary block
(provenance, detector/model id, detected/not_detected/inconclusive counts, fusion
verdict/confidence/reason) that the web UI renders, clearly labelled synthetic.
The detection stage runs only when capture is enabled, so the fail-closed
capture-off path is unchanged (events_processed=0, no detection block).

**Build gotcha:** the four detection sources were registered in the
`homeguardian` and `homeguardian_tests` CMake targets in v8 but NOT in the
separate `hg_alexa_bridge` target, so the bridge initially failed to link
(`undefined reference to SyntheticActivityDetector::detect` /
`Observation::validate` / `Observation::to_event` / `FusionEngine::fuse`). Fixed
by adding the four sources to the `hg_alexa_bridge` target and reconfiguring.

**Verified:** demo scenario now reports events_processed=15 (10 simulator + 5
detection), detection block with 4 detected / 1 inconclusive, fusion verdict
`detected` (confidence 0.75, 4 contributing); capture-off run reports
capture_enabled=false, events_processed=0, detection absent; C++ 275/2071;
Node 76/76; ctest 100%.

**Lesson:** when a new module is added, register its sources in EVERY target
that references it (app, tests, and any separate bridge/executable targets), and
confirm the demo path actually exercises it rather than only its unit tests.

## v14 native HTTP demo: foundation review and integration defects (2026-10-13)

**Context.** Branch `h04/native-http-low-footprint` was created off v13
(`7c3c323`) carrying one foundation commit `6a996daf`
("feat(native-http): add bounded loopback C++ HTTP transport foundation"),
adding `src/bridge/NativeHttp.h` (174 lines) only. (Reported context: an earlier
attempt to write the file via the GitHub web UI did not land; the commit is the
first successful recording of the foundation. No compiler/test failure is
claimed for that attempt — it is not observable from the repository.) The
foundation was treated as an unverified starting point, not a finished or
security-reviewed component.

**Review outcome (foundation):** it compiles clean (`-std=c++20 -Wall -Wextra`,
no warnings) and was a reasonable skeleton — loopback bind, GET-only, bounded
input, timeouts, reject-body/framing headers. But it was **not** ready to use as
is. Defects found and fixed (all recorded in `docs/SECURITY_READINESS_V8.md` §6):

1. **Unvalidated `Host`.** Any `Host` was accepted. A malicious page that
   rebinds a name to 127.0.0.1 could drive the API (DNS-rebinding). Fixed: the
   `Host` value must be a loopback authority (`127.0.0.1[:port]` / `localhost[:port]`).
2. **No cross-site guard on the state-advancing route.** `GET
   /api/run-scenario` has a side effect and was reachable from a cross-site
   request (the response is CORS-blocked, but the side effect fires). Fixed: a
   non-loopback `Origin` is rejected (403).
3. **No security headers on the HTML.** Added CSP, `X-Frame-Options: DENY`,
   `Referrer-Policy: no-referrer`, `Cross-Origin-Resource-Policy`.
4. **Shutdown could not unblock `accept()`.** `signal()` installs handlers with
   SA_RESTART by default, so a blocking `accept()` is auto-restarted. Fixed with a
   1 s listener timeout + an atomic stop flag (async-signal-safe).

**Integration defects (found by running the native server, not just building):**

5. **Second scenario run delivered nothing (0 events).** The legacy demo spawned
   a *fresh bridge process per request*, so per-run state was implicitly fresh.
   A single long-lived native process exposed three latent issues:
   - `SimulatorRuntime::shutdown()` and `AudioBroker::shutdown()` permanently
     latch a `shutdown_` flag; `start()`/`start_capture()` then refuse.
   - `SimulatorRuntime::grant_consent()` was **not idempotent**: the scenario
     re-granted on every run, creating two active grants for the same
     subject+purpose. The gate *correctly* fails closed on that
     (`DenyReason::ambiguous`), so run #2 delivered 0 events — a real, silent
     failure (no exception, no error string).
   **Root cause evidenced:** run #1 = 15 events; run #2/#3 = 5 events (detection
   only, 0 simulator events).
   **Fix:** (a) `SimulatorRuntime::reset_for_reuse()` clears the runtime shutdown
   latch between runs; (b) `grant_consent()` is now idempotent — it mirrors the
   gate's "latest decision governs" rule and does not append a duplicate grant
   when an active grant already exists (an explicit re-grant after a withdrawal
   still appends). Plus a per-run reset of `RuntimeStatus` counters,
   `AlertPolicy`, and pipeline history so each run reports its own numbers
   (matching the documented demo). **Result:** runs #1/#2/#3 = 15/14/1/13 each,
   persisted alert count 1→2→3 (durable history grows), as documented.
6. **A scenario error could take down the server.** `sim.start()`/`tick()` were
   not guarded; an exception would propagate out of the HTTP loop and terminate
   the process. Fixed: the scenario is wrapped; a failure is reported in the JSON
   (`error: simulator_unavailable`) and the server stays up.
7. **Test-harness bug (not product).** The first native-HTTP tests asserted a
   handler call count on a by-value functor; `std::function` **copies** the
   callable, so the counter never advanced (4 spurious failures). Fixed by
   sharing the counter by reference. Recorded so the trap is not repeated.

**Footprint observation (honest, not a win):** for `GET /api/run-scenario` the
native in-process server measured **slower** (≈46–116 ms) than the legacy model's
per-request child (~12.5 ms warm) in this environment, despite the native server
being far lighter in memory/CPU/processes. The cause was not isolated in this
pass and is left as a documented follow-up (likely per-run backend re-init: the
scenario rebuilds the broker via `set_media_capture_enabled` and re-runs
SQLite writes each call). Native wins clearly on idle RSS (~6.4 MB Release / ~7 MB Debug vs ~49 MB),
peak RSS (~6.9 MB / ~7.3 MB vs ~57 MB), CPU (~0.5 vs ~21), process count (1 vs
2+), binary size (3.1 MB Release vs the Node runtime), and dependency surface.
The native binary referenced in the earlier Debug-dir measurement is ~25.8 MB;
compare like-for-like with a Release build (`scripts/bench_demo.sh`).

**Regression coverage:** 18 new `[nativehttp]` adversarial cases (framing,
Host/Origin, method, traversal, cycles bounds, smuggling headers, security
headers, handler-not-reached on rejection), plus the existing H-03/privacy and
detection suites. Full suite 304 cases / 2217 assertions; ASan/UBSan clean.

## Native scenario latency — root-caused to SQLite fsync, not the native design (2026-10-13, v15)

**Symptom (carried from v14):** the native `/api/run-scenario` measured slower
(≈50–180 ms) than the legacy Node→C++ path (≈7–16 ms) even though the native
server is far lighter in memory/CPU/processes.

**Reproduced (Release, 40 trials):** native scenario median 46.5 ms / p95 437 ms
/ max 602 ms (sd 120 ms) — high variability; native `/api/status` 0.61 ms,
`/api/health` 0.25 ms (so HTTP parsing/dispatch is fine); legacy scenario
6.1 ms.

**Investigated, not guessed.** Bounded `HG_PROFILE=1` phase timing put **99 % of
the latency in the `persist` phase (the single SQLite alert commit)**: median
91.8 ms / p95 520 ms; every other phase (reset, `set_media_capture_enabled`,
consent grants, simulator start+5 ticks+shutdown, detection+fusion, contract
calls) was **< 1 ms**.

**Decisive control:** same DB file, same window, identical code — only
connection lifetime differed. Fresh connection (legacy child model) persist
0.11 ms; long-lived connection (native) 30.6 ms. `strace -T -e fsync,fdatasync`
showed **both** paths fsync heavily (11 fsyncs each, **28–410 ms per fsync**);
the difference is *where* the sync lands, not whether it happens. A plain
`sqlite3` test with no HomeGuardian code settled it:

```
PRAGMA synchronous=FULL  (SQLite default): median 92.7 ms/commit
PRAGMA synchronous=NORMAL (WAL)          : median  0.03 ms/commit
```

**Root cause:** `journal_mode=WAL` with the SQLite default `synchronous=FULL`
does one `fsync` per commit; this host's ext4 volume (/dev/sdc3) charges
**~50–410 ms per fsync**. The scenario performs exactly one durable commit (the
alert), hence ~93 ms median with a heavy tail. It is a **storage + durability-
setting** property, **not** a defect of the native single-process architecture —
v14's "native is slower" conclusion is therefore corrected.

**Resolution / outcome:** **no safe code optimization removes the cost.**
Eliminating the fsync means weakening durability (`synchronous=NORMAL` or async
batching), which the mission explicitly forbids without approval; and every other
phase is already sub-millisecond. The durable commit is left intact. The single
lever (`synchronous=NORMAL`, scenario → ~1–2 ms) is documented and **left for
explicit user approval** (`docs/PERF_SCENARIO_LATENCY_V15.md` §5). Do not bypass
SQLite or drop the write.

**Regression protection:** the profiling instrumentation is **default-off**
(`HG_PROFILE=1` only) and adds only numeric fields when enabled — verified inert
when unset (default response has no `timing_ms` and counts are unchanged). No
correctness, security, durability, or footprint change was made.

**Remaining limitation:** absolute numbers are host-specific; the phase
attribution and the FULL-vs-NORMAL ratio are the portable findings.

## v16: Apache-2.0 licensing, durability decision closed, release readiness (2026-10-13)

**Licensing (hackathon P0-2).** The owner approved **Apache License 2.0**. The
repo previously carried an **MIT** `LICENSE` (added in `f1a923b`), so P0-2 was
"license choice", not "missing license" — a v15 statement that the public repo
had "no license" was **inaccurate** and is corrected here.

Actions: replaced root `LICENSE` with the complete, **unmodified** Apache-2.0
text (verified: sha256 `cfc7749b…` matches both the committed file and the
official text fetched from `apache.org/licenses/LICENSE-2.0.txt`); added root
`NOTICE` (copyright + third-party attribution); updated `README.md`.

Attribution review (no relicensing of third-party code): SQLite (public domain,
bundled), Catch2 (BSL-1.0, bundled), spdlog + nlohmann/json (MIT, fetched at
build time), ask-sdk-core/model (Apache-2.0, optional npm). Their own notices are
preserved; `NOTICE` summarizes them.

**Ownership:** `Copyright 2026 Swarna Sekhar Dhar` — evidenced by the repository
owner, 45 of 46 commits, and the prior MIT copyright line. **One commit is
authored by a different name** (`6a996daf`, the `NativeHttp.h` foundation by
"MAMATA RAJBANSHI DHAR"); the owner should confirm whether a second copyright
line is wanted. No ownership was invented.

**SQLite durability decision closed.** Owner confirmed **KEEP
`synchronous=FULL`**. Verified: `SQLiteDatabase` sets `PRAGMA journal_mode=WAL`
and no explicit `synchronous`, so SQLite's default **FULL (2)** applies; a live
run's audit DB reported `journal_mode=wal`, `synchronous=2`. No durability or
transaction change was made; the v15 performance investigation stays documented
in `docs/PERF_SCENARIO_LATENCY_V15.md`, and the default-off `HG_PROFILE`
instrumentation remains off unless requested.

**Release readiness re-verified** (all executed this milestone): fresh
out-of-source **Release** build (GEN 0, all targets) + ctest 100 %; full C++ suite
**304 cases / 2214 assertions**; `[nativehttp]` adversarial **18 / 97**; ASan/UBSan
**304 / 2214, zero errors**; **Node legacy 76/76**; `demo.sh` with **Node removed
from PATH** starts a single native process (Node never invoked); loopback-only
bind; health/status/scenario + embedded dashboard OK; repeated scenario grows
durable history (1→2); capture-off fail-closed (events 0, detection absent);
privacy `enforcement_ok` true; graceful SIGINT/SIGTERM shutdown; no secrets,
databases, logs, or binaries tracked.

**No P0 blockers remain.** Remaining items are user-only (record/upload the video,
capture screenshots, complete and verify the Devpost submission).

## v17: multi-scenario household-safety demonstration with explainable alerts (2026-10-13)

**Milestone.** Implement a small, deterministic SCENARIO CATALOGUE (A normal, B
unusual, C inconclusive, D consent-denied, plus the backwards-compatible
default), run every scenario through the REAL pipeline, and make the dashboard
show backend-derived alert detail instead of hardcoded text.

**What was wrong before (verified defects found this milestone):**

1. **The persisted alert was a hardcoded placeholder.** `hg_alexa_bridge.cpp`
   constructed `Alert::create("sim-alert-N", Severity::info, "synthetic
   simulator presence notice", {}, 0.9, "sim-presence", {})` and wrote THAT to
   SQLite whenever `alerts_delivered > 0` — it was not the alert the pipeline
   produced. The dashboard then ALSO hardcoded the same string
   (`src/web/index.html`), so the "why" shown to a user was a literal in two
   places and was never derived from backend state.
   *Fix:* `Application` gained a passive `set_alert_sink()` observer invoked only
   for DELIVERED alerts; the scenario runner captures those real alerts and
   `persist_run_alerts()` writes exactly them. The dashboard now renders
   `/api/alerts` (real durable rows). No fabricated alert is inserted anywhere.

2. **Consent state leaked between scenario runs in one process.** Consent
   grants/withdrawals live in the simulator's in-memory repo and survived
   `reset_for_reuse()`. In the long-lived demo process, running `unusual` (both
   consumers granted) and then `inconclusive` (one consumer) produced TWO
   observations and an alert — the `inconclusive` scenario silently stopped being
   inconclusive. Order of execution changed results.
   *Fix:* added `SimulatorRuntime::reset_consent_state()` (clears the synthetic
   repo only) and the runner calls it at the start of every scenario, so each
   scenario is deterministic and isolated. Regression test:
   "scenarios are isolated within one long-lived process".

**Behaviour noted (not a defect).** The correlation rule is re-evaluated against
the recent-event window on EVERY processed event. After the 10 authorized
observations of a 5-cycle scenario the window still holds >=2, so the 5
detection-stage inference events also re-fire the rule: 15 events -> 14 rule
firings. The alert policy delivers exactly ONE and deduplicates the other 13, so
the user-visible behaviour is correct; only the `alerts_generated` counter is
inflated. Documented and asserted as measured behaviour rather than "fixed", to
avoid changing established semantics late in the schedule.

**Detection-stage energies.** Threshold 0.5, min_quality 0.3
(quality = min(1, |e-0.5|/0.3)). Energies within 0.09 of 0.5 are inconclusive.
Scenario C uses {0.52, 0.49, 0.51} -> 3 inconclusive observations and fusion
`unavailable` ("only 0 usable input(s)"), i.e. the system refuses to invent a
verdict. Scenario B uses {0.95, 0.90, 0.88, 0.92, 0.85} -> 5 detected, fusion
`detected` (confidence 1.0).

**Security/behaviour changes.** `/api/run-scenario` now accepts a strictly
allowlisted `scenario=` token in addition to `cycles=`; unknown/malformed tokens
are rejected (400 for malformed syntax, `{"ok":false,"error":"unknown_scenario"}`
for a well-formed but unknown id, consistent with the existing API-error
convention). New read-only routes `/api/alerts` and `/api/scenarios`. The Alexa
contract was deliberately left AGGREGATE-ONLY (it never returns alert detail):
the demo dashboard reads its own loopback `/api/alerts` from the durable store, so
the contract's documented privacy property is unchanged.

**Measured footprint (like-for-like Release builds).** Bridge binary 3,185,400 B
vs 3,128,392 B (v16) = **+57 KB / +1.8 %**. Idle RSS 6,864 kB; after a scenario
7,408 kB; peak 7,408 kB; 1 thread; 1 process. Scenario latency ~67-83 ms
(dominated by the retained SQLite `synchronous=FULL` fsync). `synchronous=FULL`
unchanged.

**Verification.** 320 C++ test cases / 2,409 assertions pass; ASan/UBSan clean;
node-free native demo; loopback-only; unknown/malformed scenario rejected;
duplicate suppression; persistence + restart; capture-off fail-closed.

## L-11: detection-driven, explainable alerts (2026-10-13, v18)

**Correction to the v17 entry.** v17 correctly removed the *hardcoded* alert and
made the persisted alert the pipeline's own output — but the alert was still
raised by `TimeWindowCorrelation("sim-presence")`, which counts RAW
`EventType::observation` events. The detection/fusion stage ran alongside and had
NO influence on whether an alert existed. So the alert could fire with the
detector reporting `not_detected`/`unavailable`, and a run whose fusion said
`available`/`inconclusive` still produced a "presence" alert. The v17 claim that
alerts were "pipeline-derived" was true; the claim implied by the docs that
detection *caused* them was not. That is what L-11 fixes.

**What changed.**
1. New `DetectionFusionRule` (core) replaces `sim-presence` as the pipeline's
   default rule. It reconstructs `Observation`s from the inference events in the
   pipeline window (`Observation::from_event`, a new round-trip with
   `to_event()`), fuses them with the existing `FusionEngine`, and returns an
   alert ONLY when the fused verdict is `detected` with confidence >= 0.5 and at
   least 2 usable votes. `inconclusive` / `unavailable` / `not_detected` /
   conflicting evidence never produce an alert.
2. Alert severity is derived from the fused confidence: >= 0.8 -> `warning`,
   otherwise `info` (never `critical`; no emergency claim is made). The alert's
   `details` carry `fusion_verdict`, `fusion_confidence`, `contributing`,
   `inconclusive`, `provenance` and the fusion `reason` — all non-sensitive.
3. Detection now runs ONLY when an AUTHORIZED acquisition occurred: the scenario
   runner (and `Application::run()`) take one detection step per tick that
   actually delivered media. No acquisition (capture off, consent withdrawn, no
   authorized consumer) => no detection => no detection-driven alert.
4. Raw observations no longer raise alerts — they are the detector's input.

**Defect found and fixed.** The recurring nested-`Config` default-argument error
(`default member initializer ... required before the end of its enclosing class`)
struck again on `DetectionFusionRule`: fixed with the `static Config defaults()`
pattern already documented for `AnomalyDetector`.

**Behaviour noted (measured, not "fixed").** Because the pipeline re-evaluates
every rule against the recent-event window on EVERY processed event, once >= 2
agreeing detector observations are in the window each subsequent processed event
re-fires the detection rule (raw observation events included). The alert policy
delivers the qualifying alerts and deduplicates the repeats, so the user-visible
result is correct; only `alerts_generated` is inflated. Carried over from the
v17 note.

**Measured scenario behaviour (capture on, live demo).**

| Scenario | media | events | generated | delivered | suppressed | persisted | detection | fusion |
|---|---|---|---|---|---|---|---|---|
| normal | 4 | 6 | 0 | 0 | 0 | 0 | 2 not_detected | not_detected (0.80) |
| unusual | 10 | 15 | 10 | 1 | 9 | 1 | 5 detected | detected (1.00) -> warning |
| inconclusive | 3 | 6 | 0 | 0 | 0 | 0 | 3 inconclusive | unavailable |
| consent-denied | 1 | 1 | 0 | 0 | 0 | 0 | not run | - |
| default | 10 | 15 | 10 | 2 | 8 | 2 | 4 detected + 1 inconclusive | detected (0.75) -> warning + info |

`default` delivers TWO alerts because the evidence produces two different
severities (a `warning` at confidence 0.92 and an `info` at 0.61), and the alert
policy keys deduplication on rule+severity — different severities are independent
conditions. That is the intended prioritisation behaviour, now driven by evidence.

**Footprint.** See the milestone report; the detection rule adds one small
translation unit and no runtime dependency, no extra process and no thread.
`synchronous=FULL` and WAL are unchanged.

## L-11 follow-up: a stale binary nearly produced a false verification (2026-10-13)

**What happened.** After changing `Application` (default rule set + detection
step), only `hg_alexa_bridge` and `homeguardian_tests` were rebuilt. The
standalone app binary `hg_build/homeguardian` was NOT, so when it was run to
capture the real log output for `docs/DEMO_SIMULATOR.md` it still printed the
OLD message (`Alert generated: synthetic simulator presence notice`) and the old
counts — i.e. the app appeared not to have been changed at all.

**Why it mattered.** The intended documentation edit was to quote the app's real
new output. Quoting the stale binary's output (or worse, "predicting" the new
output) would have written a false verification into a judge-facing document.

**Fix / rule.** Rebuild EVERY shipped target (`homeguardian`,
`homeguardian_tests`, `hg_alexa_bridge`) before verifying or documenting runtime
behaviour, and quote only output observed from a binary built after the change.
Re-measured after rebuilding: `alerts_gen=10 alerts_deliv=2 alerts_supp=8`, with
the message `Alert generated: synthetic activity detected: fusion confidence
0.92 from 2 contributing detector observation(s)`.

**Documentation sweep.** Living docs that still described the pre-L-11 behaviour
were corrected (README vision/current-status, ARCHITECTURE, PRIVACY,
DEMO_SIMULATOR, DEMO_WEB walkthrough + video script, SUBMISSION_PACKAGE video
script + test counts, AMAZONAPPDEV2026_FINAL_AUDIT scenario step). The claims
table in the audit is left as a historical record with an explicit "superseded"
addendum, and dated FRICTION_LOG/STATUS rows are preserved unchanged. The README
also no longer implies fall detection is implemented (H-01/H-02 are listed as
planned).
