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

**Total verified incidents:** 11
**Open incidents:** 1 (Incident 11 device-capture acceptance pending approval; root cause of Incident 10 resolved)
**Resolved incidents:** 10
