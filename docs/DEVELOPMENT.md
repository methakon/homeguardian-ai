# HomeGuardian AI — Development Guide

## Physical integration checkpoint — 2026-10-23

Use [Android → C++ → MCP physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md)
for the current P1 result and exit criteria. Real Mi A3 camera processing is
observed; physical observation delivery, durable alert creation and MCP readback
after restart are **not accepted**. Android-stub host tests, APK installation and
frame counters prove different layers and must not be reported as a physical
end-to-end pass.

The report describes a separately built acceptance candidate, including Android
PKCS#12 compatibility, exact transport identity extraction and wire-vocabulary
corrections. It does not assert those candidate fixes are merged into `main`.
Do not bypass TLS/SAN validation, upgrade unknown provenance, lower detector
thresholds or insert an alert by hand to close the physical gate. Keep real device
identifiers, addresses, credentials and media out of public documentation.

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
The workflow includes git, builds, tests, and verification.

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
HG_HTTP_TOKEN=demo-token hg_build/hg_alexa_bridge --http config/homeguardian.demo.json /tmp/creds.json --port 0
Routes: `/`, `/index.html`, `/api/health`, `/api/status`, `/api/run-scenario` (POST with session authentication).

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

**Honest caveat:** for `/api/run-scenario` (originally `GET /api/run-scenario` in NativeHttp; state mutations in v21+ require authenticated POST/CSRF) the native in-process server
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
HG_PROFILE=1 HG_HTTP_TOKEN=demo-token hg_build/hg_alexa_bridge --http config/homeguardian.demo.json <creds> \
  --port 0
curl -s 'http://127.0.0.1:<port>/api/status' | python3 -m json.tool
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
HG_HTTP_TOKEN=demo-token /tmp/hg_rc/hg_alexa_bridge --http config/homeguardian.demo.json <creds> --port 8497
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


## v21 — the test suite is safe to run twice in one working directory (2026-10-13)

Fixture databases are now **unique per process** (`<name>.db.<pid>`). Eleven
fixtures across ten test files previously used fixed CWD-relative names and
unlinked them on construction, so a second `homeguardian_tests` instance in the
same directory deleted the first one's open database; SQLite reported that as
`SQL error: disk I/O error` on `PRAGMA journal_mode = WAL` (31 and 27 phantom
failures in a direct two-instance reproduction). Verify with:

```bash
HG_TESTS_PATH=$PWD/hg_build/homeguardian_tests bash scripts/verify_suite_concurrency.sh
# both instances: "All tests passed (2841 assertions in 369 test cases)"
# 0 "disk I/O error" occurrences; no leftover *.db in the working directory
```

If you add a new fixture that opens a database, suffix its path with
`std::to_string(::getpid())` (or use
`std::filesystem::temp_directory_path()` plus a stamp) — do not reuse a fixed
filename in the working directory.


## v21 — editing the dashboard (2026-10-13): no cmake re-run needed

`src/web/index.html` is embedded into the bridge at **build** time by
`cmake/generate_web_assets.cmake`, driven by an `add_custom_command` that depends
on the HTML, the `cmake/hg_web_assets.h.in` template and the generator script.
A plain `make -j4 hg_alexa_bridge` after editing the dashboard prints
`Embedding the dashboard (src/web/index.html)` and relinks; the header is only
rewritten when its content changes.

Check it without a browser:

```bash
grep -c 'sevbadge\|impactbox' <build>/generated/hg_web_assets.h   # expect 9
curl -s http://127.0.0.1:<port>/ | grep -c 'sevbadge\|impactbox'  # expect 9
```

Both zero would mean a stale embed (the failure mode fixed in v21: the header was
generated at configure time with no dependency on the HTML, so `demo.sh` could
serve the previous dashboard after a dashboard-only edit).


## D-02 — building and testing the Android node (2026-10-13)

```bash
# native lib + dex + package + sign (no Gradle; JDK/SDK/NDK only)
bash apk/build_apk.sh

# host-side acceptance: 18 checks + device presence report
bash scripts/verify_android_node.sh

# the node contracts on their own
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[d02]"
```

Toolchain (all already on this host): JDK 17 at `$HOME/android-jdk`, SDK at
`$HOME/Android/Sdk` (build-tools 27.0.3, platform android-27), NDK r26d. The
script exports `JAVA_HOME` **and** puts `$JAVA_HOME/bin` on `PATH` — `dx`,
`apksigner` and `zipalign` are shell wrappers that invoke `java`, so setting
`JAVA_HOME` alone fails with `dx: line 89: exec: java: not found`.

Java style constraint: no lambdas. The build dexes with `dx` and deliberately
omits `--min-sdk-version`, because that enables invokedynamic desugaring which
the API 27 dex verifier on the target device rejects. `verify_android_node.sh`
checks the dex for `invoke-custom`/`invoke-polymorphic` so this cannot silently
regress.

Pitfalls hit while implementing D-02:

1. **`` in a string literal is greedy.** `"with\x01ctl"` parses as the hex
   escape `\x01c` followed by `tl`, so a sanitisation test asserted the wrong
   expected value. Append the control byte explicitly (`raw.push_back('\x01')`)
   when the next character is a hex digit.
2. **Two clocks.** Java sends `System.currentTimeMillis()` as the frame
   timestamp because the native accountant validates against
   `system_clock`; `SystemClock.elapsedRealtime()` (boot-relative) would fail the
   plausibility check as a then-future/zero timestamp. Audio uses the native
   clock directly.
3. **`clock_t` is a C type.** A `using clock_t = std::chrono::system_clock;`
   alias in a test collides with the C `clock_t` from `<ctime>` and fails with
   "template argument 1 is invalid". Use a distinct name (`Clock`).
4. **aapt spells optional features differently.** A manifest feature declared
   `required="false"` is reported as `uses-feature-not-required:` (not
   `uses-feature:`), so a checker grepping the wrong key reports a false failure.


## Testing the skills-integration layer (2026-10-14, D-02x)

```bash
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[precursor]"            # 6 cases / 200 assertions
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[sensors]"              # 7 cases / 487 assertions
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[ha]"                   # 6 cases / 179 assertions
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[integration][skills]"  # 7 cases /  65 assertions
```

The `[ha]` suite includes a **real loopback exchange**: a one-shot HTTP server is bound to
127.0.0.1 on an ephemeral port inside the test process, so the socket path, the request head,
the status line and the response body are exercised without any external service. The
production transport is otherwise injected through `IHomeAssistantTransport`, so the adapter
contracts (allowlist, de-duplication, liveness, provenance) are deterministic and network-free.

Registering a new core source: add it to the target that REFERENCES it. This milestone's
sources are registered in `homeguardian_tests` only, which is why the Release sizes of
`homeguardian` and `hg_alexa_bridge` did not move. Adding an unreferenced translation unit to a
shipped target grows the binary without changing behaviour.


## Verifying the dashboard status surface (2026-10-14)

```bash
bash scripts/verify_integration_dashboard.sh [port]   # 31 API assertions + 5 shell checks
```

The script starts the bridge on a free loopback port with a temporary 0600 credential table,
asserts the integration surface (counts are zero, every empty surface carries a reason, the
vocabulary is exactly the permitted set, the hypothesis is labelled a hypothesis, forbidden
tokens and credentials are absent, a query string is refused), then repeats with a MALFORMED
credential table and asserts the process refuses cleanly - non-zero exit, a fail-closed message,
no core dump and no serving. The credential file is a TABLE keyed by token, exactly as `demo.sh`
writes it; copying that shape rather than inventing one is what the script's own incident entry
records.


## D-03 — building and testing the network-facing boundary

The device-ingestion binaries (`hg_device_ingress`, `hg_device_client`) link OpenSSL, so CMake builds
them **only where OpenSSL development headers exist**; everything else in the D-03 work (protocol,
registry, ingest, detectors) is OpenSSL-free and is compiled and tested everywhere. If the headers are
missing the build says so loudly rather than silently skipping the target.

```
# host, with libssl-dev present
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release --target hg_device_ingress hg_device_client -j4

# D-03 end-to-end acceptance (real TLS, real certificates, real binaries)
bash scripts/verify_device_transport.sh
```

Without the toolchain, the acceptance script prints a SKIP with the reason and exits 0. A SKIP is not
a pass: the executed evidence is the run inside the container image, where the toolchain is
guaranteed, and that run is recorded in `docs/DISTRIBUTED_DEPLOYMENT.md`.

The full container acceptance is the `test` stage:

```
docker build --progress=plain --target test -f deploy/docker/Dockerfile -t homeguardian-hub:test .
```

which builds, runs `ctest`, and then runs the D-03 TLS acceptance against a freshly started ingress.

### Rules the tests encode

- **Feed before you filter.** Any suppression layer runs AFTER the detector/accumulator consumes the
  sample (Incident 34).
- **Compute the precondition.** A liveness or staleness test injects the clock and drives it past the
  documented timeout instead of asserting a state the test never created.
- **The watermark is committed last.** A refused message must not consume a sequence number; there is
  a test for exactly that.
- **Verification is never disabled to make a test pass.** Tests use a real private CA, generated on
  the fly by `tools/provision_ca.sh`.
- **A SKIP must say what to install.** A missing toolchain is reported with the reason, never as a
  green result.

## Running the D-03.1 battery

```bash
# host suite (the TLS targets are skipped loudly without libssl-dev)
cmake --build hg_build --target homeguardian_tests -j4
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests

# everything that needs OpenSSL: built and run INSIDE the test stage
docker build -f deploy/docker/Dockerfile --target test -t homeguardian-ai:test .
#   -> ctest, TLS acceptance, TLS identity matrix, CA isolation

# deployment surfaces
docker build -f deploy/docker/Dockerfile --target runtime -t homeguardian-hub:local .
bash scripts/verify_docker_hub.sh            # D-01 acceptance
env -u HG_PROJECT -u HG_CERTS_DIR bash scripts/verify_detection_end_to_end.sh
bash scripts/verify_build_context_exclusions.sh
HG_BRIDGE_PATH=$PWD/hg_build/hg_alexa_bridge bash scripts/verify_retention.sh
HG_TESTS_PATH=$PWD/hg_build/homeguardian_tests bash scripts/verify_suite_concurrency.sh

# footprint (needs the TLS binaries, so run it in the test image)
docker run --rm -v "$PWD:/repo:ro" \
  -e HG_INGRESS_PATH=/src/build/hg_device_ingress \
  -e HG_CLIENT_PATH=/src/build/hg_device_client \
  homeguardian-ai:test bash /repo/scripts/measure_device_transport.sh
```

Provisioning (split storage — note the three directories):

```bash
tools/provision_ca.sh init --ca-dir ~/offline-ca --runtime-dir deploy/docker/certs \
  --device-dir ~/device-keys --server-names "DNS:homeguardian.local,IP:10.20.30.10"
tools/provision_ca.sh issue <device-id> --ca-dir ~/offline-ca \
  --runtime-dir deploy/docker/certs --device-dir ~/device-keys
```

## D-03.2 verification commands

```bash
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[net][replay]"    # two-phase record, idempotent repeat
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[net][processing]" # fault injection per boundary
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[net][policy]"     # replay protection is mandatory
```

The ingress now REFUSES to start with an empty `replay_state_path` unless
`allow_ephemeral_replay_protection` is set AND the bind address is loopback. If a test or
scratch run fails with `replay_state_path is required`, that is the control working: pass a
path, or opt out explicitly on loopback.

## Local voice feedback (H-02A) commands

```bash
hg_build/hg_audio_bridge --voice-status                      # engine, sink, availability, last failure
hg_build/hg_audio_bridge --speak-test --sink <sink>          # one fixed test sentence (refuses if no engine)
hg_build/hg_audio_bridge --start --i-understand-this-opens-the-microphone --voice --seconds 60
hg_build/hg_audio_bridge --start --i-understand-this-opens-the-microphone --voice --mute
hg_build/hg_audio_bridge --start --i-understand-this-opens-the-microphone --no-voice
HG_SOURCE_DIR=$PWD hg_build/homeguardian_tests "[voice]"     # 17 cases, no sound is played
```

The voice tests NEVER open a speaker: they drive the `IVoiceOutput` seam with a fake, and the only
assertion about the real output is its argv (a pure function).

## H-03 Android node commands

```bash
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/android-ndk-r26d
bash apk/build_apk.sh                       # builds + signs hg_apk_build/homeguardian-harness.apk
adb devices -l                              # BLOCKED here: no device enumerates
adb install -r hg_apk_build/homeguardian-harness.apk     # keeps app data; never uninstalls anything
adb shell am start -n org.homeguardian.harness/.MainActivity
```

Artifact check (the wiring must be IN the APK, not just in the source):

```bash
unzip -o hg_apk_build/homeguardian-harness.apk -d /tmp/apkx
strings /tmp/apkx/classes.dex | grep -c NodeSession            # >0
strings /tmp/apkx/lib/armeabi-v7a/libhg_harness.so | grep -c extract_motion_features
strings /tmp/apkx/classes.dex | grep -c 'invoke-custom'        # must be 0 (API 27 verifier)
```

## JNI export gate (H-03)

```bash
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/android-ndk-r26d
bash apk/build_apk.sh          # runs the gate before packaging; a missing/mangled export fails the build

# run it on its own against any built library
bash scripts/verify_jni_exports.sh hg_apk_build/lib/libhg_harness.so \
  apk/src/org/homeguardian/harness \
  $ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm
```

A Java `native` method binds to `Java_<pkg>_<Class>_<method>`. A definition outside an `extern "C"`
region is C++-mangled (`_Z52Java_...`) and JNI cannot resolve it - the device then throws
`UnsatisfiedLinkError` on the first call. The gate compares the Java declarations with the symbols
REALLY defined in the compiled ELF, so this class of defect cannot ship again.
