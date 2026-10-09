# HomeGuardian AI — Friction Log

**Project:** HomeGuardian AI
**Hackathon:** Amazon Developer Hackathon 2026
**Last updated:** 2026-10-09

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

**Total verified incidents:** 6
**Open incidents:** 0
**Resolved incidents:** 6
