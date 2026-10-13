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

A deployment-readiness and security review of the Alexa boundary is in
`docs/SECURITY_READINESS_V5.md` (threat model, verified controls, proposed HTTPS
deployment boundary, and a CONDITIONAL GO recommendation). The local stdio
bridge is NOT a deployed HTTPS service.

Authentication and secret management (B1+B4) are implemented in
`core/AlexaCredentialStore.{h,cpp}`: real bearer-token verification with
expiry/revocation, constant-time comparison, 0600 secure-file loading,
fail-closed behavior, and no mock fallback. The bridge selects the secure store
with `HG_BRIDGE_SECURE_CREDS=1` (a load failure exits 3, never serving with a
weaker verifier); the default local verifier is for development/tests only.
Tests: `homeguardian_tests "[creds]"` (14 cases) and the secure-mode cases in
`integration.test.js`. Report: `docs/SECURITY_READINESS_V6.md`.

A local-only loopback HTTP adapter (`src/bridge/hg_http_adapter.js`) exposes the
real C++ boundary over HTTP on `127.0.0.1` only, using the Node stdlib `http`
module (no new dependency). It enforces method/route allowlists, size/timeout/
concurrency limits, rate limiting, and generic authentication failures, and
never logs or returns tokens. Run it for development with:
`HG_BRIDGE_SECURE_CREDS=1 node src/bridge/hg_http_adapter.js --bridge
hg_build/hg_alexa_bridge --config config/homeguardian.json --creds <creds.json>
--port 0`. Tests: `node --test test/http_adapter.test.js` (18 cases). **It is
NOT TLS and NOT production** — loopback development use only; see
`docs/HTTP_ADAPTER_V7.md`.

## Simulated Alexa+ web demo

One command: `bash demo.sh`. It builds the C++ bridge if needed, then serves a
loopback-only web UI (`src/web/index.html` via `src/bridge/hg_demo_server.js`)
that drives the **real** backend: consent gate → event pipeline → correlation →
alert dedup → SQLite persistence. The bridge's `run_scenario` mode runs the real
synthetic household scenario and persists the resulting alert, so every number
and alert shown in the UI comes from running code, not from the page.

Tests: `node --test test/web_demo.test.js` (8 cases). Full walkthrough,
expected output, troubleshooting, and a suggested <3-minute video script:
`docs/DEMO_WEB.md`.

**Clean-clone reproducibility:** the tests CMake target previously used
`../src/...` relative paths, which only worked when the build directory was a
direct child of the source tree. These are now `${CMAKE_SOURCE_DIR}`-absolute,
and one test that read `config/homeguardian.json` relative to the CWD now
resolves via the `HG_SOURCE_DIR` environment variable (set by ctest). An
out-of-source build from any directory now configures, builds, and passes.

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

## Detection layer (added 2026-10-12)

Source under `src/core/`:
- `Observation.{h,cpp}` — provenance/detector/status model; `to_event()` → Event.
- `AnomalyDetector.{h,cpp}` — explainable statistical anomalies.
- `FusionEngine.{h,cpp}` — confidence-propagating, conflict-aware fusion.
- `SyntheticActivityDetector.{h,cpp}` — deterministic synthetic detector.

Tests: `tests/core/test_detection.cpp`. Run the detection group with:
`hg_build/homeguardian_tests "[detection]"`.

All four are registered in `CMakeLists.txt` (both the `homeguardian` and
`homeguardian_tests` targets). No new third-party dependency was introduced —
the layer is pure C++ standard library.

Note on the nested-`Config` default-argument pattern: constructors use a
`static Config defaults()` helper rather than an inline `Config{}` default
argument (see `docs/FRICTION_LOG.md`, 2026-10-12). Follow this pattern for any
new class exposing a nested config with a default constructor argument.

## Running tests reproducibly (added 2026-10-12, v9)

Use one of these so the shipped-config test resolves the repo root correctly:

- Preferred: `ctest --test-dir <build>` — sets `HG_SOURCE_DIR` automatically.
- Direct binary run: `HG_SOURCE_DIR=<repo-root> <build>/homeguardian_tests`.

A bare `<build>/homeguardian_tests` run from an out-of-source dir without
`HG_SOURCE_DIR` fails one config-path test (see `docs/FRICTION_LOG.md`,
2026-10-12). After any new source files are added, reconfigure every build dir
you cite (`cmake . && make`) — incremental dirs do not auto-pick up new sources.

## Privacy audit (added 2026-10-13, roadmap H-03)

`src/core/PrivacyAuditLog.{h,cpp}` — a bounded, thread-safe, metadata-only log of
consent-gated data-access decisions. Attach it as a passive observer:

- `ConsentGatedAcquisition::set_privacy_audit(&log)`
- `SimulatorRuntime::set_privacy_audit(&log)`

Never attach it inside `ConsentGate` — the gate is a pure decision function. The
log observes decisions; it must not create them.

Tests: `tests/core/test_privacy_audit.cpp`. Run with:
`HG_SOURCE_DIR=<repo> hg_build/homeguardian_tests "[privacy][audit]"` (or the
`[audit]` tag, which also includes the PersistenceAudit cases).

Sources are registered in all three CMake targets (`homeguardian`,
`homeguardian_tests`, `hg_alexa_bridge`) — remember to add new core sources to the
bridge target too (see `docs/FRICTION_LOG.md`, 2026-10-12).

## Native demo (no Node.js) — build & run (added 2026-10-13, v14)

The demo now runs as ONE native C++ process. `bash demo.sh` builds
`hg_alexa_bridge` if needed and starts it in HTTP mode; Node.js is not required.

Manual run:
```
# 1. build
cmake -S . -B hg_build -DCMAKE_BUILD_TYPE=Release && cmake --build hg_build --target hg_alexa_bridge -j4
# 2. creds (synthetic, local only)
cat > /tmp/creds.json <<'JSON'
{"demo-token":{"principal":"demo","allow_status":true,"allow_alert_summary":true,"expires_at_ms":0,"revoked":false}}
JSON
# 3. serve (loopback only; port 0 = ephemeral, prints "DEMO_READY <port>")
hg_build/hg_alexa_bridge --http config/homeguardian.demo.json /tmp/creds.json --port 0 --token demo-token
```
Routes: `/`, `/index.html`, `/api/health`, `/api/status`, `/api/run-scenario?cycles=N` (GET only).

The dashboard is embedded at configure time from `src/web/index.html`
(`cmake/hg_web_assets.h.in` -> `generated/hg_web_assets.h`). **Re-run `cmake` after
editing the HTML** so the embedded copy refreshes.

Legacy Node demo server (historical reference / A-B benchmark only):
`HG_DEMO_LEGACY_NODE=1 bash demo.sh`.

Footprint benchmark: `bash scripts/bench_demo.sh` (native vs legacy Node).

Tests: `HG_SOURCE_DIR=<repo> hg_build/homeguardian_tests "[nativehttp]"` (18 cases).

## Footprint benchmark results (2026-10-13, measured)

`bash scripts/bench_demo.sh` (native vs legacy Node, same workload). Native
measured with a **Release** build (`-DCMAKE_BUILD_TYPE=Release`); a Debug build
is ~25.8 MB and has different RSS, so compare like-for-like
(`HG_BRIDGE_PATH=<release-binary>`).

| Metric | Native (Release) | Legacy Node | Notes |
|---|---|---|---|
| Idle RSS | **~6.4 MB** | ~49 MB | node ~7.7× heavier |
| Peak RSS (after scenario) | **~6.9 MB** | ~57 MB | node ~8.3× heavier |
| CPU% (idle-ish sample) | **~0.5** | ~21 | |
| Runtime processes | **1** | 2+ (node + spawned bridge/request) | |
| Binary / deployed size | **3.1 MB** (Release) | node runtime + 7 KB script + C++ bridge | |
| Runtime dependencies | libc/libstdc++/libm/libgcc (ldd) | a full Node.js runtime | |
| `/api/health` latency | avg ~0.0 ms, max ~0.4 ms | avg ~0.0 ms, max ~7 ms | |
| `/api/status` latency | avg ~0.0 ms, max ~0.5 ms | avg ~0.3 ms, max ~12 ms | |
| `/api/run-scenario` | ~0.05–0.18 s | ~0.007–0.016 s | **node faster here — see below** |
| Dashboard files at runtime | 0 (embedded) | 1 (served from disk) | |

**Honest caveat:** for `GET /api/run-scenario` the native in-process server
measured **slower** than the legacy per-request child process in this
environment. The cause was not isolated in this pass and is a documented
follow-up. Native wins decisively on memory, CPU, process count, dependency
surface, and self-containment; latency for the read routes (`/health`,
`/status`) is equal or better.

## Profiling the scenario path (added 2026-10-13, v15)

Set `HG_PROFILE=1` to add steady-clock phase timings to the scenario response
(`timing_ms` inside `scenario`, and `handler_timing_ms` at the top level). It is
**off by default and inert when unset** (verified: the default response contains
no timing fields and all counts are unchanged). It records only numeric
durations — no credentials, paths, or media.

```
HG_PROFILE=1 hg_build/hg_alexa_bridge --http config/homeguardian.demo.json <creds> \
  --port 0 --token demo-token
curl -s 'http://127.0.0.1:<port>/api/run-scenario?cycles=5' | python3 -m json.tool
```

Findings and the reproduction methodology: `docs/PERF_SCENARIO_LATENCY_V15.md`.

## v17 — Running the scenarios (2026-10-13)

Scenarios are executed by `src/core/DemoScenarioRunner.cpp` and can be driven
three ways:

1. **Web demo (default):** `bash demo.sh`, then pick a scenario in the dashboard
   selector and click "Run scenario". The page calls
   `/api/run-scenario?scenario=<id>`.
2. **HTTP directly:**
   `curl "http://127.0.0.1:<port>/api/run-scenario?scenario=unusual"`.
   `cycles=N` may be added to override the scenario's own cycle count.
3. **NDJSON bridge (tests):** `run_scenario [cycles] [scenario_id]` on stdin.

Catalogue: `default` (backwards-compatible), `normal`, `unusual`,
`inconclusive`, `consent-denied` (see `docs/DEMO_WEB.md`). Unknown ids are
rejected.

Tests: the scenario suite is `tests/core/test_demo_scenarios.cpp` and the
allowlist/route tests are in `tests/core/test_native_http.cpp`.

    cd hg_build && cmake . && make -j4 homeguardian_tests
    HG_SOURCE_DIR=<repo> ./homeguardian_tests "[scenario]"

Note: scenario tests construct `Application`, which is a process-wide singleton;
each test scopes its own instance. `Logger::initialize("error")` is required
because the alert path logs.

## L-11 — How the detection→alert path works (2026-10-13)

- The default correlation rule is `DetectionFusionRule` (`src/core/DetectionFusionRule.{h,cpp}`).
  It is the only alert source. To change what can alert, change its `Config`
  (`min_usable`, `min_confidence`, `warning_confidence`, `max_contributing`) —
  not a hardcoded string.
- Detection inputs come from AUTHORIZED acquisitions: the scenario runner takes
  one detection step per tick that delivered media, using the scenario's
  `detection_energies` (indexed by tick). With no acquisition there is no
  detection and therefore no detection-driven alert.
- `Observation::from_event()` rebuilds an `Observation` from the inference event
  in the pipeline window, so the rule fuses exactly the evidence that flowed
  through the pipeline.
- Focused tests: `HG_SOURCE_DIR=<repo> ./homeguardian_tests "[l11]"` (causal
  path + negative cases) and `"[scenario]"` (end-to-end scenario results).
- Energies: detector threshold 0.5, min_quality 0.3 (quality = min(1,|e-0.5|/0.3)).
  `e >= 0.59` -> detected, `e <= 0.41` -> not_detected, `|e-0.5| < 0.09` ->
  inconclusive.

## L-12 — Anomaly stage: how to change and verify it (2026-10-13)

- `src/core/AnomalyMonitor.{h,cpp}` wraps `AnomalyDetector` and gives it a
  pipeline role. Tune it via `AnomalyMonitor::Config`
  (`zscore_threshold` default 2.0, `min_samples` default 4, `max_window` 32,
  `max_gap` 3s, `detector_id` "activity-anomaly").
- The feature fed to it is the scenario's per-tick synthetic activity energy —
  the same input the activity detector sees, but judged against a LEARNED
  baseline instead of a fixed threshold (relative vs absolute).
- Evidence policy lives in `DetectionFusionRule::Config::evidence_detector_ids`
  (default `{"activity-energy"}`; empty list = accept all). Add
  `"activity-anomaly"` to make anomaly evidence vote; both directions are covered
  by `tests/core/test_anomaly_monitor.cpp` (`[l12][causal]`).
- Run it: `HG_SOURCE_DIR=<repo> ./homeguardian_tests "[l12]"`, or the demo
  scenarios `anomaly-warmup`, `anomaly-normal`, `anomaly-deviation`,
  `anomaly-missing`.
- Recall: energy >= 0.59 with quality >= 0.3 is `detected` for the activity
  detector; the anomaly detector instead compares each sample with the rolling
  mean/stddev of the same key and flags |z| > threshold.


## H-01s — Synthetic impact stage: how to change and verify it (2026-10-13)

- `src/core/ImpactClassifier.{h,cpp}` — the deterministic classifier. Tune it via
  `SyntheticImpactClassifier::Config` (`transient_threshold` 0.80,
  `stillness_threshold` 0.25, `max_window` 4, `detector_id` "impact-signal").
  `Config::defaults()` exists because a nested `Config` cannot be used as a
  default argument in an aggregate initialiser (same pitfall as v8).
- `src/core/SyntheticImpactRule.{h,cpp}` — the critical-severity mapping
  (`min_supporting` 2 by default). It reads only `impact-signal` observations.
- `src/core/Pipeline.cpp` — `severity_rank()` (critical 2, warning 1, info 0) and
  the evaluate-all-rules loop. If you add a rule, it participates in
  prioritisation automatically; a rule that returns `nullopt` never affects it.
- Scenario definition: `src/core/DemoScenarios.cpp` → `synthetic-critical-impact`
  (`impact_intensities`, `run_impact`). The runner feeds one bounded sample per
  AUTHORIZED acquisition (`src/core/DemoScenarioRunner.cpp`).
- Tests: `tests/core/test_synthetic_impact.cpp` (`[impact][h01s]`, 18 cases)
  covers each acceptance criterion; `[h01s]` runs in ~0.2 s.

Verify:

```bash
cd hg_build && cmake . && make -j4 homeguardian hg_alexa_bridge homeguardian_tests
HG_SOURCE_DIR=$PWD/.. ./homeguardian_tests "[h01s]"      # focused
HG_SOURCE_DIR=$PWD/.. ./homeguardian_tests               # full: 364 cases / 2811 assertions
ctest --test-dir .                                       # 100%
```

Two pitfalls specific to this feature:

1. **The pipeline window is time-based.** `Pipeline::get_recent_events()` drops
   events older than `correlation_window_ms` (5 s). A test (or a producer) that
   timestamps observations with a fixed past instant will silently see an empty
   window and no rules will fire. Use `std::chrono::system_clock::now()`.
2. **Round-trip features.** `Observation::from_event` must carry the bounded
   feature payload, not just the activity keys; the impact rule matches on the
   `phase` feature. `[h01s]` fails loudly if that regresses.

Expected numbers for the scenario (deterministic): media 12, events 30,
generated 23, delivered 2 (1 CRITICAL + 1 INFO), suppressed 21, persisted 2,
impact 6 samples / 1 transient / 1 confirmation.


## v21 — release-candidate verification: how to run it (2026-10-13)

Three defects were fixed in v21. Re-verify with the matrix below; every figure in
`docs/RELEASE_CANDIDATE_V21.md` came from a fresh out-of-source Release build.

```bash
# 1. fresh Release build (never measure from hg_build -- it is a Debug build)
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=$PWD/hg_build/_deps/spdlog-src \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$PWD/hg_build/_deps/nlohmann_json-src /tmp/hg_rc
cd /tmp/hg_rc && make -j4 homeguardian homeguardian_tests hg_alexa_bridge
ctest --test-dir .          # 100%

# 2. retention regression (defect 3)
HG_BRIDGE_PATH=/tmp/hg_rc/hg_alexa_bridge bash scripts/verify_retention.sh

# 3. live matrices (start the bridge first, in a writable scratch dir)
/tmp/hg_rc/hg_alexa_bridge --http config/homeguardian.demo.json <creds> --port 8497 --token demo-token
```

Pitfalls confirmed in v21:

1. **An unwritable CWD used to abort the process.** The logger's file sink threw
   out of `main()`. It is now best-effort; the `[logger]` test pins it (it places a
   *directory* named `homeguardian.log` in a scratch dir, which fails
   deterministically even for root).
2. **A read-only or corrupt audit DB is a supported state**, not a crash: the
   status route reports `unavailable` and no persistence is claimed. Test it by
   running the bridge from a `chmod 500` directory or with a garbage
   `homeguardian_bridge_audit.db`.
3. **The bridge's alert store is bounded by `alert_retention_days`.** When
   checking retention, remember there are two databases: the application's
   `homeguardian_audit.db` and the bridge's `homeguardian_bridge_audit.db`
   (relative to the bridge's CWD). Seed rows in the one the route actually serves.
4. **`pgrep -f hg_alexa_bridge` matches your own shell command.** Enumerate with
   `pgrep -ax hg_alexa_bridge` and read `/proc/<pid>` by exact PID.
5. **The unknown-scenario contract changed to 400** (see
   `docs/ARCHITECTURE.md`). Update any client or test that assumed `200` plus
   `{"ok":false}`.
6. `scripts/verify_retention.sh` needs `sqlite3`; if it is not on `PATH`, set
   `HG_SQLITE3=/path/to/sqlite3`.
