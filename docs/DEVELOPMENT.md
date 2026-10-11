# HomeGuardian AI — Development Guide

## Toolchain

| Tool | Version | Purpose |
|------|---------|---------|
| g++ | 13.3.0 | C++20 compiler |
| CMake | 3.28.3 | Build system |
| Git | 2.x | Version control |
| make | — | Build tool (Unix Makefiles generator) |

### Optional Dependencies
| Library | Version | Status |
|---------|---------|--------|
| Ninja | — | Not installed; use `make` instead |
| OpenCV | — | To be installed for Phase E |
| ONNX Runtime | — | To be installed for Phase E |
| Boost | 1.83.0 | Partially installed (iostreams, locale, thread) |

## Build Instructions

```bash
mkdir build && cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DHOMEGUARDIAN_BUILD_TESTS=ON ..
make -j4
```

The `-j4` flag limits parallelism to 4 jobs, respecting the 30% CPU budget.

## Testing

```bash
cd build
ctest --output-on-failure -V
```

Tests use Catch2 v2.13.10 (chosen for `CATCH_CONFIG_MAIN` support).
A single test executable `homeguardian_tests` runs all test cases.
Registered with CTest via `add_test()`.

Current test coverage:
- Config: default values, validation (port, log level, history, window, threshold), load/save round-trip
- Logger: initialization, log level, file output, uninitialized throws
- Application: construction, run/shutdown on worker thread
- Event: construction, validation, JSON round-trip
- EventValidator: validation, normalization, provenance preservation
- SimulatedEventSource: replay order, exhaustion
- TimeWindowCorrelation: fires, insufficient, outside window
- Pipeline: no alert, alert generation, history bounds
- Alert: creation, JSON round-trip
- Persistence: SQLite repositories, schema migration, constraint error subtypes
- Consent: ConsentRecord model, gate decisions, fail-closed evaluation
- ConsentGuardedDevice: gate before init/start/acquire; withdrawal/expiry/permission/failure force stop; delivery-time authorize_delivery
- Simulator: deterministic camera frames + PCM mic samples; bounded ring; Config (dimensions, frame-rate, channels, noise/sine/square/silence); speaker play/error/recovery
- Ubuntu backend: sensor discovery metadata; consent enforcement on the shared gate
- Dual-device + audio broker: device independence, shared-source fan-out, per-consumer consent denial/withdrawal, bounded speaker queue, arbitration, playback recovery, shutdown
- End-to-end simulator: camera/mic/speaker → consent-gated delivery → synthetic processor → pipeline rule/alert → safe speaker response; failure and consent scenarios
- Runtime integration: SimulatorRuntime driven by Application::run() — capture-off delivers no media; capture-on + consent delivers to permitted consumers; per-consumer withdrawal; determinism + bounded memory; idempotent shutdown; capture toggle
- Event-to-alert: authorized synthetic observations → Application::process_event() → Pipeline validation + TimeWindowCorrelation + alert; capture-off/withdrawal/missing-consent produce no events; invalid/future-timestamp events rejected without corrupting pipeline state
- AlertPolicy: dedup/cooldown/re-arm keyed by rule+severity; injectable clock; bounded retained state; first occurrence always delivered; unrelated keys independent
- Lifecycle/hardening: idempotent start/shutdown; partial-startup cleanup; capture kill-switch toggle; bounded event/alert history; consent withdraw/restore cycles; config fail-closed; deterministic repeated runs
- PersistenceAudit: durable alert metadata via existing SQLite (AlertRepository); historical-vs-current-process distinguishability; retention (delete_older_than); fail-safe degraded on write failure; corrupt DB throws (no silent success)
- RuntimeStatus: bounded privacy-safe counters (state, capture, media auth/denied, events processed/rejected/failed, alerts gen/delivered/suppressed, queue depth/overflow, persistence health); reset per process

Run a focused group with a tag filter, e.g. `./homeguardian_tests "[audit]"`,
`"[status]"`, `"[alertpolicy]"`, `"[clearing]"`, `"[retention]"`, `"[alexa]"`,
`"[integrity]"`, `"[privacy]"`, `"[lifecycle]"`, `"[hardening]"`,
`"[event2alert]"`, `"[runtime]"`, `"[sim]"`, `"[dual]"`, or `"[e2e]"`. Full
suite: `ctest --output-on-failure`.

The Alexa skill tests run with Node's built-in runner:
`cd alexa_skill && npm test` (runs `test/handler.test.js` for the skill handlers,
`test/contract.test.js` for the integration-contract adapter boundary,
`test/integration.test.js` for process-boundary tests against the real compiled
`hg_alexa_bridge`, and `test/restart.test.js` for restart/degraded cases; all use
mock/test-only credentials, no deployment).

The integration/restart tests require the bridge to be built first:
`cmake --build hg_build --target hg_alexa_bridge`. A reproducible walkthrough is
in `docs/demo_alexa_integration.sh`.

A reproducible simulator demonstration of the running app is in
`docs/DEMO_SIMULATOR.md`.

## Coding Standards

- C++20 baseline
- RAII and smart pointers for all resource management
- Const-correctness
- Explicit error handling (no silent failures)
- Thread-safe boundaries between pipeline stages
- Bounded queues and timeouts for all external calls
- No global mutable state
- SOLID principles: single responsibility, open/closed, Liskov substitution,
  interface segregation, dependency inversion

## Resource Budget

The machine has 40 logical CPUs (20 cores × 2 threads) and 15 GB RAM.
HomeGuardian development is limited to **30% of CPU capacity**:

- Maximum 12 logical CPUs for builds and tests
- `make -j4` for compilation (conservative)
- Thread pool size: 4 threads (default)
- No persistent background workers during development

## Implementation Workflow

All code is implemented directly with full architectural review.
Hermes manages git, builds, tests, and verification.

## Debugging

- Set `LOG_LEVEL=debug` in config for verbose logging
- Structured logs go to stdout and optionally to a file
- Each component logs with its name as the context

## Configuration

Configuration is JSON, loaded from `config/homeguardian.json`:

```json
{
  "log_level": "info",
  "server": {
    "host": "0.0.0.0",
    "port": 8080
  },
  "consent": {
    "default_allow": false,
    "zones": []
  },
  "retention": {
    "events_days": 30,
    "alerts_days": 90
  }
}
```

Missing values fall back to documented defaults.

## Adding a New Module

1. Create the source files under `src/<module>/`
2. Define interfaces in header files
3. Implement in `.cpp` files
4. Add tests under `tests/<module>/`
5. Register in `CMakeLists.txt`
6. Update this guide and `docs/ARCHITECTURE.md`
