# HomeGuardian AI — Status

**Last updated:** 2026-10-23

## Private remote MCP readiness — 2026-10-23

**BLOCKED:** the existing VPS WireGuard server is up and boot-enabled, but the
phone's supplied public key is not enrolled and its matching local profile has
no address or peer configured. There is no current phone handshake. Cloud UDP
ingress is restricted to an old single source that does not match the worker's
current public IP. The private coordinator is not deployed. Phone MCP remains
healthy, authenticated, read-only and loopback-only; no public MCP port was opened.
See [verified facts, minimum preparation and approval gates](PRIVATE_VPN_REMOTE_MCP_READINESS.md).
The phone already has a boot-registered foreground service; restart after first
unlock is configured, but a real reboot was not re-tested.

## P1 physical-integration checkpoint — 2026-10-23

**Existing phone runtime verified:** a fresh authenticated on-device MCP self-probe
at 02:17:34 IST completed initialize, tool discovery and a status call in 51 ms.
The Mi A3's foreground EdgeService reported engine `Running`, `read_only=true` and
`capture_enabled=false`. The core runtime and read-only MCP are working on the
phone; this is separate from acceptance of the physical alert pipeline below.

**NOT ACCEPTED:** Android → C++ → durable alert → MCP physical acceptance remains
open. The Mi A3 sensor app is installed and real 320×240 camera processing was
observed, but recorded delivery counters stayed at zero and the isolated audit
database contains no physical alert. APK installation and host tests are not
end-to-end acceptance.

Candidate fixes cover network permission, wire vocabulary, Android keystore
compatibility and transport identity. USB repeatedly re-enumerated with kernel
error `-71`; a temporary authenticated Wi-Fi ingress was authorized, but phone
configuration and delivery were not completed. Test listeners are now stopped;
the existing EdgeService was preserved. See
[the acceptance report](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md) for measured
evidence, candidate-only changes, provenance limits and remaining exit criteria.

## Security Audit & Release Candidate Status
- **Candidate `52d3d1436e115fe9061127a35f33b35d10c8dd73`:** Evaluated by Hermes Session 1 (P0-1 PASS, P0-2 PASS, P0-3 FAIL). Failed due to hardcoded physical device seed secrets, unauthenticated re-pairing, and lack of heartbeat sequence replay protection.
- **Successor Candidate (Branch `fix/hg-sec-06-device-secrets`):** Implements complete physical-device credential remediation (Fix 1-5: CSPRNG 256-bit secrets, POSIX 0700/0600 storage protection, authenticated re-pairing, sequence replay protection across restarts). Complete details in `docs/RELEASE_AUDIT_STATUS.md`.

## Current Phase

Simulator-first development with separately authorized physical-device acceptance.
The deterministic simulator, shared audio broker and consent-gated acquisition
remain distinct from physical verification. Camera capture on the Mi A3 was
authorized and observed; microphone capture was not run in this checkpoint.
Physical alert delivery and restart recovery remain unverified, not complete.

## Completed Work

| Task | Evidence | Date |
|------|----------|------|
| Repository initialized | `homeguardian-ai` on `main` branch | 2026-10-09 |
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
| Detection: Observation model | `src/core/Observation.{h,cpp}` — provenance/detector/status; converts to Event, reuses pipeline | 2026-10-12 |
| Detection: AnomalyDetector | `src/core/AnomalyDetector.{h,cpp}` — explainable z-score baselines, warm-up, missing-observation, bounded | 2026-10-12 |
| Detection: FusionEngine | `src/core/FusionEngine.{h,cpp}` — confidence propagation, conflict->inconclusive, time window | 2026-10-12 |
| Detection: SyntheticActivityDetector | `src/core/SyntheticActivityDetector.{h,cpp}` — deterministic synthetic detector, no I/O | 2026-10-12 |
| Detection tests | `tests/core/test_detection.cpp` — 20 cases / 67 assertions; full suite 275/2071 | 2026-10-12 |
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
| Security readiness v5 | Deployment-readiness & security review of the Alexa boundary; fixed LOW-1 (bridge now reflects real config capture state, fail-safe); +6 C++ `[alexa]` security-regression cases +2 Node integration cases; report `docs/SECURITY_READINESS_V5.md`; HTTPS endpoint = CONDITIONAL GO (blocked on real authn/TLS/rate-limit/secrets) | 2026-10-11 |
| Test suite | 241 test cases, 1960 assertions, all passing; Node 45/45 (13 handler + 12 contract + 16 integration + 4 restart) | 2026-10-11 |
| Security readiness v6 | `core/AlexaCredentialStore` (real bearer-token verification: expiry/revocation, constant-time compare, 0600 secure-file load, fail-closed, no mock fallback; B1+B4); bridge `HG_BRIDGE_SECURE_CREDS` mode; 14 `[creds]` + 5 secure-mode Node cases; report `docs/SECURITY_READINESS_V6.md` | 2026-10-12 |
| Test suite | 255 test cases, 2004 assertions, all passing; Node 50/50 (13 handler + 12 contract + 21 integration + 4 restart) | 2026-10-12 |
| HTTP adapter v7 | `src/bridge/hg_http_adapter.js` (local-only loopback HTTP adapter over the real C++ bridge; stdlib `http`, no new dependency; method/route allowlists, size/timeout/concurrency limits, rate limiting, generic auth failures, graceful shutdown); 18 `http_adapter` Node tests; report `docs/HTTP_ADAPTER_V7.md`; NOT TLS, NOT production | 2026-10-12 |
| Test suite | 255 test cases, 2004 assertions, all passing; Node 68/68 (13 handler + 12 contract + 21 integration + 4 restart + 18 http_adapter) | 2026-10-12 |
| Hackathon P0 | README corrected (removed false MCP/Ring claims; documented implemented vs simulated vs planned); simulated Alexa+ web experience (`src/web/` + `src/bridge/hg_demo_server.js` + `demo.sh` + `config/homeguardian.demo.json`); bridge `run_scenario` mode (real pipeline → real alerts → durable persistence); 8 `web_demo` Node tests; fixed CMake out-of-source path bug + test CWD-isolation bug; `docs/DEMO_WEB.md` | 2026-10-12 |
| Test suite | 255 test cases, 2001 assertions, all passing; Node 76/76 (13 handler + 12 contract + 21 integration + 4 restart + 18 http_adapter + 8 web_demo) | 2026-10-12 |
| Detection framework (v8) | Observation model, AnomalyDetector, FusionEngine, SyntheticActivityDetector; suite now 275 cases / 2071 assertions; Node 76/76 | 2026-10-12 |
| Stabilization pass (v9) | Verified 275/2071, ctest 100%, Node 76/76, fresh out-of-source build green, ASan reconfirmed on current source (275/2071, zero sanitizer errors), demo loopback+scenario OK, persistence-after-restart OK | 2026-10-12 |
| Demo completion (v10) | Wired v8 detection/fusion framework into the demo scenario: SyntheticActivityDetector + FusionEngine now run in run_demo_scenario, feed inference events through the EXISTING pipeline, and surface a synthetic detection block in the scenario JSON + web UI. Fail-closed capture-off path unchanged (detection absent, events=0). Added detection sources to hg_alexa_bridge target. Suite 275/2071; Node 76/76; ctest 100% | 2026-10-12 |
| Judge-ready validation (v11) | Re-ran full end-to-end demo + failure-path matrix (capture-off fail-closed, inconclusive/conflicting fusion, dedup, restart persistence, malformed requests, loopback) — all PASS; C++ 275/2074, ctest 100%, Node 76/76, ASan 275/2074 zero errors, fresh out-of-source build green; no P0/P1 defects; recording recovery steps added to submission package | 2026-10-12 |
| Privacy audit (v13) | Roadmap H-03: PrivacyAuditLog + verify_enforcement(); wired into ConsentGatedAcquisition + SimulatorRuntime (passive observer); demo `privacy_audit` block. 11 new tests; suite 286/2120; Node 76/76; ctest 100% | 2026-10-13 |
| Native HTTP demo (v14) | Roadmap H-04 + native transport: `hg_alexa_bridge --http` serves the embedded dashboard + JSON API in ONE C++ process (Node eliminated from the demo); foundation reviewed (4 defects fixed: Host/Origin/CSP/shutdown); integration defects fixed (simulator+broker reuse latch, idempotent `grant_consent`, guarded scenario) ; 18 `[nativehttp]` adversarial cases; suite 304/2217; ctest 100%; ASan clean. Footprint: idle RSS ~7 MB vs ~49 MB node, peak ~7.3 MB vs ~57 MB, 1 proc vs 2+, no Node dep | 2026-10-13 |
| Scenario latency investigation (v15) | Root-caused native `/api/run-scenario` latency to SQLite `synchronous=FULL` fsync per commit (~93 ms/commit on this host); NOT a native-design defect (legacy child fsyncs too, 28-220 ms each). 99% of latency in the persist phase; all other phases <1 ms. No safe optimization without weakening durability; `synchronous=NORMAL` option documented and left for approval. Added bounded default-off profiling (`HG_PROFILE=1`). Suite 304 cases / 2214 assertions; ctest 100%; `[nativehttp]` 18/97 | 2026-10-13 |
| Licensing + release readiness (v16) | Apache-2.0 LICENSE (sha256-verified vs apache.org) + NOTICE + README; hackathon P0-2 DONE. SQLite `synchronous=FULL` RETAINED (decision closed; verified WAL + synchronous=2 at runtime; no durability change). Roadmap reconciled: F-01/G-01 DONE in scope via the native transport + embedded dashboard; F-02/F-03/G-02/G-03 DEFERRED. v15 durability decision marked resolved. Release-readiness verified (build/tests/ASan/demo/loopback/persistence/capture-off/privacy/shutdown) | 2026-10-13 |
| Multi-scenario demo (v17) | 5 deterministic scenarios (normal / unusual / inconclusive / consent-denied / default) run through the REAL pipeline; persisted alerts are the pipeline output (the old hardcoded placeholder is removed); scenario consent state is reset per run (order-independent); /api/run-scenario?scenario= allowlisted, /api/alerts + /api/scenarios added; dashboard rewritten with a scenario selector, backend-derived alert detail (severity/rule/message/explanation) and safe DOM (textContent). 320 C++ cases / 2,409 assertions; ASan clean; binary +1.8% (3.19 MB), idle RSS 6.9 MB, 1 process, 1 thread | 2026-10-13 |
| L-11 detection-driven alerts (v18) | Alerts are now CAUSED by detection/fusion evidence: the pipeline's default rule is DetectionFusionRule (fusion of the detector observations carried by the pipeline); alerts only on a `detected` verdict above a confidence floor with >=2 usable votes, never on inconclusive/unavailable/not_detected/conflict. The earlier raw-observation `sim-presence` rule is removed. Detection runs only after an AUTHORIZED acquisition. Alert severity follows fused confidence (>=0.8 warning, else info); alert details carry fusion verdict/confidence/contributing/provenance/reason; API + dashboard surface them. Scenario results re-measured. 329 C++ cases / 2,453 assertions; [l11] 9/40; ASan clean | 2026-10-13 |
| L-12 anomaly detection in pipeline (v19) | New core AnomalyMonitor feeds the bounded activity feature of each AUTHORIZED acquisition to the statistical AnomalyDetector; results become detection Observations (detector_id activity-anomaly) that flow through the SAME pipeline. DetectionFusionRule gained an explicit evidence allowlist (default = activity detector only), so anomaly evidence is ADVISORY: reported/audited/persisted but not a vote unless explicitly allowed (both directions unit-tested). 4 new scenarios (anomaly-warmup/normal/deviation/missing). Measured: warm-up 2/4 no anomaly; normal mean 0.710 sd 0.00894 status normal; deviation status high, 2 anomalies, z=2.442; missing gap probe. L-11 results unchanged (unusual 10/1/9, default 10/2/8). 346 cases / 2648 assertions; [l12] 17; ASan clean; standalone and bridge agree | 2026-10-13 |
| H-01s simulated critical impact + severity prioritisation (v20) | New core `SyntheticImpactClassifier` (deterministic transient-then-stillness pattern: transient >= 0.80, stillness <= 0.25 within 4 samples; a lone transient stays `inconclusive`) and `SyntheticImpactRule` (a CONFIRMED pattern with >= 2 supporting observations maps to `Severity::critical`). `Pipeline` now evaluates every registered rule and returns the HIGHEST severity, so a critical impact alert is never lost behind an unrelated info/warning alert (single-rule behaviour unchanged). Fixed a latent lossy `Observation::from_event` (it kept only activity `energy`/`detect_threshold`; it now carries the bounded feature payload, so detector-specific features such as the impact `phase` reach the rules). New scenario `synthetic-critical-impact` (catalogue now 10). Measured: media 12, events 30, generated 23, delivered 2 (1 CRITICAL `synthetic-impact` + 1 INFO `detection-fusion`), suppressed 21, persisted 2; impact 6 samples / 1 transient / 1 confirmation; the anomaly layer stayed advisory (status `low`, 2 anomalies, `fused_into_alert` false). Regressions unchanged: unusual 10/1/9 warning, default 10/2/8, normal/inconclusive/consent-denied 0. 364 cases / 2811 assertions; `[h01s]` 18/147; Release bridge 3,320,856 B (+60,896 B, +1.87% vs L-12), idle RSS 6,432 kB, 1 process / 1 thread | 2026-10-13 |
| v21 final integration + stability verification (RC) | Fresh out-of-source Release build of all three shipped targets (0 errors). All 10 scenarios verified with asserted expectations (10/10); one critical-capable scenario unchanged. HTTP adversarial matrix 51/51, e2e/stability PASS (deterministic repeated runs, no state leak, RSS/fd flat), retention regression 8/8, fail-closed 7/7, browser smoke test with 0 JS errors, Node 76/76. Three verified defects FIXED with regression coverage: (1) an unsupported scenario id returned HTTP 200 -> now 400 with the same generic JSON error body (handler now owns the business status; transport validation unchanged, dashboard unaffected); (2) an unwritable working directory made the logger throw out of main -> SIGABRT (exit 134); the file sink is now best-effort (console-only fallback); (3) the bridge's own durable alert store never applied alert_retention_days -> unbounded growth; now pruned at startup and bounded-periodically, honouring the config (0 = keep forever). 369 C++ cases / 2841 assertions (was 364/2811); CTest 100%; ASan/UBSan clean; legacy Node 76/76. Release bridge 3,342,280 B (+21,424 B, +0.65%); 1 process / 1 thread; idle RSS 6,392 kB; peak after runs 7,224 kB (= RSS); idle CPU 0.00%; startup 3-4 ms; critical scenario 72-111 ms. WAL + synchronous=FULL retained. `docs/RELEASE_CANDIDATE_V21.md` -> GO (RC only; no merge, no publication). Follow-up finding fixed: 11 test fixtures used fixed CWD-relative DB names, so two suite instances in one working directory produced phantom "disk I/O error" failures (31/27 cases) — now process-unique, with `scripts/verify_suite_concurrency.sh` proving two concurrent Release AND ASan instances both pass with zero disk-I/O errors. A fifth defect was found and fixed while starting the demo: the dashboard was embedded at CONFIGURE time with no dependency on `src/web/index.html`, so a dashboard-only edit left a stale page inside an already-built binary (demo.sh served the previous UI). Generation is now a build-time custom command (`cmake/generate_web_assets.cmake`) with the HTML as a real dependency; verified by touching the HTML and running `make` alone, plus a fresh configure/build and the served page | 2026-10-13 |
| D-01 hub container (branch `feature/distributed-homeguardian`) | The owner changed the target architecture to a central C++20 hub in Docker plus real Android acquisition nodes; v21 remains the tested hackathon fallback. D-01 ships a multi-stage container (build → test → runtime): debian:trixie-slim in both stages because the project requires CMake >= 3.28.3 and builder/runtime glibc must match; no Node.js and no Python in the image; runs as uid 10001 with cap_drop ALL and no-new-privileges; no privileged mode, no device passthrough, no docker.sock; **no published ports** (the loopback-only HTTP server is deliberately not exposed) with an opt-in 127.0.0.1 host override; health check via /dev/tcp (no curl); persistent named volume at /data; credentials mounted read-only as a Compose secret (never in a layer); exec'd entrypoint for clean SIGTERM; mem/cpu/pids limits and log rotation; restart `unless-stopped`. `docker build --target test` runs the existing suite inside the container toolchain; `scripts/verify_docker_hub.sh` asserts health, real-core responses, persisted alerts, survival across container replacement, clean SIGTERM, isolation, WAL header in the volume, and reports footprint. **D-06 genuine Alexa: OFFICIAL ALEXA DOCKER RUNTIME BLOCKED** — no official Alexa/Alexa+ container image exists, AVS developer tools are withdrawn, the AVS Device SDK was archived 2024-01-16, and the Alexa+ Category SDK / MCP Toolkit are partner-only; the official skill route needs owner approval. Details and primary-source references: `docs/DISTRIBUTED_DEPLOYMENT.md` | 2026-10-13 |
| D-02 Android acquisition node (branch `feature/distributed-homeguardian`) | Real acquisition application on the existing no-Gradle APK pipeline (JDK + SDK 27 + NDK r26d, armeabi-v7a). New core: `CaptureAccounting` (bounded, authorized, timestamped payload contract; thread-safe; saturating counters; drop reasons) and `NodeIdentity` (opaque hashed node id + sanitised bounded labels) — the D-03 registration identifier. **AAudio backend completed**: it previously opened a stream but registered no data callback, so no PCM was ever read; it now builds a PCM_I16 stream at the configured rate/channels, registers data + error callbacks, authorizes EVERY buffer through the consent gate, accounts it, tracks negotiated rate/channels and counts xruns/disconnects. Camera1 bridge gained front/back selection, size (320x240/640x480/854x480) and FPS (5/10/15) selection, per-frame wall-clock timestamps and a local frame count, keeping the per-frame delivery gate. MainActivity is the node UI: identity + display name, consent grant/**withdraw**, camera and microphone controls, a live RECORDING banner, live counters + last refusal reason, runtime permission flows, and a non-capture self-test. Capture is off by default; starting needs an explicit tap + OS permission + consent record; leaving the activity stops and releases everything (no foreground service, no background capture — deliberately). Verified on the host: `[d02]` 18 cases / 212 assertions, full suite 387 cases / 3053 assertions (was 369/2841), ctest 100%, fresh out-of-source Release build of all 3 host targets with 0 errors (homeguardian 3,210,648 B, hg_alexa_bridge 3,342,280 B), ASan/UBSan 387/3053 with 0 sanitizer reports, APK built+signed+aligned with no invokedynamic in the dex, all 12 JNI entry points exported, `scripts/verify_android_node.sh` 18/18. Footprints: APK 794,159 B, native library 1,140,532 B. **D-02 PHYSICAL CAPTURE ACCEPTANCE: BLOCKED — NO DEVICE ATTACHED** (no frame/sample/battery numbers are reported, because none were observed) | 2026-10-13 |
| Skills validation and detection integration (branch `feature/skills-detection-integration`, D-02x) | Inspected three requested skills and implemented only what is feasible and honest. `animal-calamity-precursor-detection` and `home-security-sensor-nodes` found; **no skill named `Home Assistant hub` exists** (nearest artefacts: the installed Hermes plugin `homeassistant` v2.0.1 and the sensor-nodes skill's HA tooling) — reported with the exact identifiers. New pure-C++20 layer, no new dependencies: `BoundedFeatures` (bounded audio RMS/peak/ZCR + motion changed-fraction from permitted PCM/luma, refusing oversize with a reason, no ML, no media retention), `ActivityDetectors` (`ActivityChangeDetector` EMA baseline + sustained confirmation; `VectorDriftDetector` cosine drift with a baseline that only updates while normal), `DempsterShafer` (Dempster's rule over an 8-element power set with conflict mass K; suppresses rather than averages when K>0.9; frame = {routine, activity_change, environmental_anomaly} — a calamity hypothesis is unrepresentable), `PrecursorDetection` (result vocabulary limited to activity_change_detected / audio_activity_deviation / possible_animal_activity / environmental_correlation / insufficient_evidence), `EnvironmentalCorrelation` (authorized env samples + windowed correlation with reported lag; Category-B evidence at info only), `SecurityObservation` (device id, source type, acquisition + ingestion timestamps, real/synthetic provenance, confidence/quality, consent/authorization context, bounded validated payload; strict `from_json`; availability tracker that keeps a dead sensor distinguishable from a silent one; bounded cooldown de-duplication), `HomeAssistantAdapter` + `HttpClient` (read-only GET /api/states, disabled by default, explicit entity allowlist, token from a mounted 0600 file refused if group/other-readable, CR/LF header injection refused, 64 KiB response cap, https refused because no TLS is linked, no actuation path). Integration: new observations convert to the standard `Event` and flow through the EXISTING pipeline; new detector ids are ADVISORY by default so all historical scenarios behave identically; opt-in via the existing evidence allowlist. No new path can reach `Severity::critical`. THREE REAL DEFECTS found by the new tests and fixed: (1) `DempsterShafer::belief` summed SUPERsets (commonality) instead of subsets, reporting belief 1.0 on total ignorance — `plausibility()` added to show the interval honestly; (2) the environmental-precedence rule required the environmental hypothesis to top the DST fusion, which strong activity evidence prevents, so the rule could never fire; (3) the availability observation was built by copying a moved-from object (empty device id, empty consent context, defaulted provenance). Verification: `[precursor]` 6/200, `[sensors]` 7/487, `[ha]` 6/179, `[integration][skills]` 7/65; **full host suite 413 cases / 3,984 assertions** (was 387/3,053); ctest 100%; fresh out-of-source Release build of all three targets with 0 errors (homeguardian 3,210,648 B and hg_alexa_bridge 3,342,280 B unchanged — the new sources are registered where they are referenced, i.e. the test target, and will be linked into the hub when D-03/D-04 add the ingestion route); ASan/UBSan 413/3,984 with 0 sanitizer reports. NOT implemented, with reasons recorded: YAMNet/embeddings, YOLO, Farneback optical flow, MediaPipe pose, Frigate, MQTT, Home Assistant WebSocket events, actuator control, Alexa (still externally blocked), and any species identification. Full matrix, evidence grading and per-method records: `docs/SKILLS_INTEGRATION_VALIDATION.md` | 2026-10-14 |
| Skills integration, second increment: dashboard surface + section-J coverage (branch `feature/skills-detection-integration`, D-02x) | **Dashboard (section K):** new read-only `/api/integrations` route whose every value is DERIVED from real state - the Home Assistant block is built from `HomeAssistantConfig::problems()` (so the reason it is off comes from the adapter's own validation), the device count from a real `DeviceAvailabilityTracker`, and the label vocabulary from the detection layer's closed result set. Rendered as **Sensor & integration status** with the three provenance labels stated verbatim (LIVE SENSOR DATA / SYNTHETIC TEST DATA / HYPOTHESIS / UNVALIDATED CORRELATION) plus a per-label legend mapping each result to observation, correlation or hypothesis; every empty surface shows WHY it is empty, and the payload carries no calamity, identity or fall claim at all. **New verification:** `scripts/verify_integration_dashboard.sh` - 31 API assertions + 5 shell checks, 0 failed (counts are zero, every empty surface has a reason, the vocabulary is exactly the permitted set, the hypothesis is labelled a hypothesis, forbidden tokens are absent, existing routes still 200, and a query string is refused 404). **Verified defect fixed (Incident 33):** a malformed credential table aborted the bridge - `json.exception.type_error.306` escaped `main()` as SIGABRT/core dump on the DEFAULT credential path (the secure path already returned 3). The load is now guarded: bounded fail-closed message, exit 3, no permissive fallback, nothing served; the harness includes that adversarial case. **Missing-observation guard:** `PrecursorSuite::fusion(now)` degrades a stale finding to `insufficient_evidence` (a stale finding is not an update) after `stale_after` (default 300 s) instead of repeating it as current. **Section-J gap suite** `tests/core/test_skill_gaps.cpp` `[gaps]` (4 cases / 84 assertions): multiple devices reporting simultaneously (independent liveness, per-device dedup keys, three devices coexisting in one pipeline history, no path to critical), unauthorized source rejected and consent withdrawal stopping delivery with zero further requests and resuming only on re-authorization, upstream authentication failure (401 -> errors counted, zero observations, entity not marked alive, no token echoed), and resource bounds under sustained input (5000-sample activity stream, oversized vector refused, 500-device registry capped, 500-entry dedup capped, oversized feature payload refused). Also fixed by these tests: the multi-device case asserted a precondition it never established (Incident 35 - the default 90 s timeout versus the test's own 200/400 s arithmetic). Full host suite now **417 cases / 4,068 assertions** (`[precursor]` 6/200, `[sensors]` 7/487, `[ha]` 6/179, `[integration][skills]` 7/65, `[gaps]` 4/84). Documentation split into the four required files: `SKILL_VALIDATION_REPORT.md` (matrix, A/B/C grading, per-method records, proof-bearing inventory, correction ledger), `ANIMAL_ACTIVITY_DETECTION.md`, `HOME_SECURITY_SENSOR_INTEGRATION.md`, `HOME_ASSISTANT_INTEGRATION.md` | 2026-10-14 |
| D-03 secure device transport (branch `feature/secure-device-transport`) | A separate, mutually authenticated TLS ingestion boundary between device and hub, so the loopback-only demo server is untouched and unpublished. **Credential model:** a private CA (`tools/provision_ca.sh`) issues one certificate per device whose CN IS the device id; the hub requires BOTH that the chain verifies against the CA AND that the SHA-256 fingerprint equals the `credential_id` the registry binds to that device - two independent bindings, because either alone would let one phone act as another. Identity comes from the certificate, never the payload. **Protocol** (`src/net/DeviceProtocol.*`): versioned, 4-byte big-endian length + JSON, 4 KiB hard cap validated before allocation, enumerated kinds, and a closed reject vocabulary (identity_mismatch, device_unknown, device_revoked, device_unpaired, consent_missing, consent_withdrawn, capture_disabled, replayed_sequence, duplicate_observation, malformed_timestamp, stale_timestamp, invalid_value, unknown source/observation type, oversized, queue_full, bad_version, bad_frame). **Provenance is derived:** real requires authenticated peer AND paired AND recorded consent AND capture enabled AND live acquisition; a `real` claim the hub cannot substantiate is recorded synthetic and flagged, and the trust facts are read from the registry rather than from the caller. **Transport** (`src/net/TlsTransport.*`): OpenSSL, TLS 1.2 minimum, `SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT` against the private CA with the verify result checked explicitly, socket timeouts, no custom crypto, verification never disabled in any mode including tests. **Registry** (`src/net/DeviceRegistry.*`): bounded per-device record (id, name, paired, credential_id, revoked, type, capabilities, camera/mic, last heartbeat, last accepted sequence, last authorized observation timestamp, online, consent) with strict persistence that deliberately does NOT restore capture or liveness on restart; revocation is total and is re-read from a revocation file on every connection, so it takes effect on the next message instead of at the next restart. **Accept path** (`src/net/ObservationIngest.*`): every check independent and ordered, and the sequence watermark commits ONLY after all of them pass, so a refused message cannot consume a sequence number. **Ingress** (`src/net/DeviceIngressServer.*`, `src/main_ingress.cpp`): one protocol on the published port (no dashboard, no commands, no SQLite), bounded connection queue, bounded worker pool, idle timeouts, clean SIGTERM, alerts persisted through the existing `PersistenceAudit` (WAL + synchronous=FULL unchanged). **D-02x limitation closed:** `src/net/IngestDetectors.*` wires ActivityChangeDetector, the existing AnomalyMonitor, SensorDeduplicator and DeviceAvailabilityTracker per device into the SAME pipeline; VectorDriftDetector and the PrecursorSuite fusion are deliberately not wired to a single scalar. Derived ids stay advisory (not in the evidence allowlist) and no path can reach `Severity::critical`. **Android:** transport implemented in the APK (`ObservationTransport.java`: bounded 128-entry queue with drop-oldest accounting, one worker thread, bounded reconnect backoff, mutual TLS from a PKCS#12, no credential in any payload or log) - compile-verified only; no phone attached, so physical delivery is BLOCKED. **Verification:** host suite `[net]` 9 cases / 396 assertions; full host suite **426 cases / 4,464 assertions** (Debug, Release and ASan/UBSan all agree; D-02x was 413/3,984); ctest 100% on both Debug and Release; 0 sanitizer reports; the container test stage runs the full suite AND the real-TLS acceptance (`scripts/verify_device_transport.sh`: handshake, unknown device, wrong credential, revocation without restart, replay, future and ancient timestamps, unlisted source type, oversized frame, missing handshake, consent withdrawal, TLS-only port, clean SIGTERM, durable alert count across restart); D-01 container acceptance re-run at 26/26. **End-to-end (the decisive acceptance):** with both containers running, a mutually authenticated client delivered 8 observations over mutual TLS; the hub's own alert count, read through its dashboard API from inside the hub container, moved 0 -> 1; the served history carried a `detection-fusion` alert; the ingested evidence and the alert row were durable in the shared SQLite store (main file plus `-wal`); no critical severity was produced; the alert survived container replacement (1 -> 1). The same run refused an identity-mismatched device, a revoked device without a restart, and an unregistered device. Acceptance totals: D-03 TLS **21/21** (inside the container image build too), D-03 end-to-end detection **32/32**, D-01 hub container **26/26**. Docs: `docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md`, `docs/ANDROID_TRANSPORT_D03.md` | 2026-10-14 |

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
Local process-boundary integration: hg_alexa_bridge (NDJSON stdio over the REAL AlexaContract) driven by Node — 16 integration + 4 restart cases against the actual compiled binary; reproducible demo docs/demo_alexa_integration.sh; no network listener, no deploy
Simulated Alexa+ web experience (`demo.sh`): `bash demo.sh` builds the bridge and serves a loopback-only web UI that drives the REAL backend (consent gate → event pipeline → correlation → alert dedup → SQLite persistence); `run_scenario` bridge mode produces real alerts; 8 `web_demo` Node cases; `docs/DEMO_WEB.md`. Simulated bridge only — no Alexa device, no live Amazon identity, no physical sensor. (The native C++ Model Context Protocol server `hg_mcp_server` was subsequently added in PR #20).
Clean-clone reproducibility (hackathon P0): fixed the tests-target `../src/...` relative paths (now `${CMAKE_SOURCE_DIR}` absolute) so an out-of-source build from ANY directory configures and builds; fixed a test that read `config/homeguardian.json` relative to CWD (now resolves via `HG_SOURCE_DIR`). Verified: fresh build in `/tmp` → 255/2001 + ctest 100%, and the web demo ran end-to-end on the fresh binary
Security review v5: docs/SECURITY_READINESS_V5.md — fail-closed auth, read-only ops, no credential leak, no network listener, no shell exec, bounded lifetime all VERIFIED; HTTPS endpoint CONDITIONAL GO (blocked on real authn/TLS/rate-limit/secrets)
Release reproduction: fresh out-of-source build (existing dep cache) → 241/1960 + bridge, new tests included
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

- **Remote:** `origin` → `https://github.com/methakon/homeguardian-ai.git`
- **Repository:** `homeguardian-ai`
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

## D-03.1 — security and reliability hardening (complete)

Implemented and verified: server identity verification on both sides, split credential
storage (offline CA / runtime / device keys), delivery-time authorization with fail-closed
handling, durable restart-surviving replay protection with a resume point, Android
acknowledgment/queue/lifecycle correctness, and honest processing-failure reporting.

Executed: host suite 430 cases / 4,565 assertions; ctest 100 % (Debug and container
Release); TLS acceptance 21/21; identity matrix 9/9; CA isolation 14/14; end-to-end
detection 38/38 (including replay-after-restart and mid-session revocation); D-01 26/26;
suite concurrency PASS; retention 8/8; context-exclusion probe 6/6; footprint measured.

Still blocked: physical Android acceptance (no device), Alexa (Amazon), live Home
Assistant (needs TLS).

## D-03.2 — replay/processing consistency (complete)

Corrected the ordering defect the external audit found (a durable watermark that advanced
before processing, turning a sink failure into silent loss), made the documented durability
real (file fsync + directory fsync, failures reported), and made replay protection mandatory
for a live ingress with a loopback-only isolated-test opt-out.

Executed: host suite 432 cases / 4,642 assertions; ctest 100%; ASan/UBSan clean; TLS
acceptance 24/24 (incl. 3 new mandatory-replay cases); TLS identity 9/9; CA isolation 14/14;
end-to-end 38/38; D-01 26/26; suite concurrency PASS; retention 8/8. Fault injection at
every boundary: validation, begin, sink, complete, repeat, concurrency, unwritable store,
dead-process marker.

Still blocked: physical Android acceptance (no device); Alexa (Amazon); live Home Assistant
(needs TLS).

## H-01 — real hardware activation (H-01a partial, H-01b-e software complete, H-01f blocked)

Discovered: a real microphone (built-in ALC897 analog input via PipeWire 1.0.5, 48 kHz) and a
real speaker (default sink is the Bluetooth `YO-457BT`; built-in IEC958 also available). The
Android phone the milestone reports as connected is **NOT** visible: `lsusb` shows no phone
and `adb devices -l` is empty.

Implemented and built on `feature/h01-hardware-activation`: a real PipeWire-backed microphone
adapter (`PwAudioCaptureDevice`), a real speaker adapter with a bounded queue, volume limit,
cancel and a locally generated tone (`LocalSpeaker`), the explainable audio activity session
(`AudioActivitySession`), and the host-side permission-gated service (`hg_audio_bridge`) that
keeps audio hardware OUT of every container. No ALSA headers exist on this host and `sudo`
needs a password, so the adapters drive PipeWire's CLI rather than linking libasound.

Safety, verified: `hg_audio_bridge --start` without the confirmation flag refuses (exit 3,
nothing opened); the capture adapter throws unless capture was explicitly authorized; the
whole `[audio]` suite is silent and deaf (dry-run/refusal paths only). Nothing has yet opened
the physical microphone: the first capture and the first playback are the operator's actions.

Amazon: **BLOCKED — no verified supported runtime** (no Amazon image exists, AVS tooling is
not generally available, and the in-repo Alexa skill is a simulated web demo, not a runtime).

Docs: `docs/HARDWARE_ACTIVATION_H01.md`.

## H-02A local voice feedback - test results (feature/h02a-local-voice-feedback)

| suite | result |
|---|---|
| full C++ suite | **460 cases / 5,547 assertions**, all passing |
| `[voice]` (new) | **17 cases / 133 assertions**, all passing - no sound is played by any of them |
| `[audio]` (H-01) | 20 cases / 1,599 assertions, all passing |
| ctest | 100% |

Sustained capture (H-01b) remains PASS: 35 s, 1,605,632 samples, 489 windows, 0 dropped, clean stop.
Bluetooth output (H-01 tone): OPERATOR CONFIRMED audible.
Spoken feedback (H-02A): implemented and unit-tested; physical speech acceptance BLOCKED pending
`sudo apt-get install -y espeak-ng`. Numbers that need the engine (first/subsequent utterance
latency, synthesis memory) are recorded as NOT MEASURED rather than estimated.

## H-02B corrections - test results (feature/h02a-local-voice-feedback)

| suite | result |
|---|---|
| full C++ suite | **464 cases / 5,564 assertions**, all passing |
| `[voice]` | **21 cases / 151 assertions** (4 new regression tests; no test plays a sound) |
| ctest | 100% (70.81 s) |

Real speech: `engine_name` = espeak-ng, `completed_within_wait=true`, `playback_jobs=1`, rc 0,
3.79 s per sentence, 7.5 MB peak RSS, 2% CPU. Real microphone->detection->speech session:
`voice_spoke: "Unusual sound activity was detected. The source has not been identified."` from a
real sustained_increase finding. HomeGuardian's spoken sentence is NOT marked physically verified
until the operator confirms hearing it.

## H-03 Android Wi-Fi node - status

| item | state |
|---|---|
| APK | BUILT + SIGNED, 882,617 B (`hg_apk_build/homeguardian-harness.apk`) |
| Transport wiring | IMPLEMENTED + COMPILED (was dead code before this milestone) |
| Artifact verification | dex + .so inspected; 0 `invoke-custom` |
| USB/ADB | **BLOCKED - DEVICE NOT ENUMERATED** |
| Phone model/API/ABI | UNKNOWN (not assumed; earlier docs assumed API 27) |
| Install / permissions / capture / Wi-Fi / USB-unplug | **BLOCKED** (no device) |
| Live remote CCTV | NOT YET IMPLEMENTED |

No hardware figure is reported because no hardware was reachable. The exact operator steps are in
`docs/ANDROID_WIFI_HARDWARE_ACCEPTANCE_H03.md` section 3.

## H-03 physical device pass (2026-10-14)

| item | state |
|---|---|
| Device | MediaTek MT6580 clone (`0e8d:201c`), API 27, armeabi-v7a, ~962 MB RAM, ~12.1 GB storage |
| App | installed (v0.2.0, 883,891 B), launched clean, idle by design, permissions granted |
| Node identity | `hg-443a03b54537c21f` (reported by the app's own native layer) |
| Provisioning | CA + server cert (SAN contains IP 10.20.30.6) + unique device cert; credential_id matches the registry |
| Ingress | running as a least-privilege container, `tls=mutual(min 1.2)`, `registry_devices=1 paired=1`, publishing 10.20.30.6:8443 |
| Hub accepts this device | **VERIFIED**: `accepted=3 rejected=0` over mutual TLS; spoofed id refused `identity_mismatch`; watermark durable at `last_sequence=3` |
| Phone over Wi-Fi | **PENDING OPERATOR** (phone is locked; Import + Connect + consent + Start are on-device actions) |
| Camera/mic on hardware | **PENDING OPERATOR** |
| USB unplug acceptance | NOT STARTED (must follow Wi-Fi delivery) |
| Remote live CCTV | NOT YET IMPLEMENTED |

## H-03 JNI defect (fixed) - status

| item | state |
|---|---|
| UnsatisfiedLinkError | **FIXED** (extern "C" region; symbols now unmangled, verified from the ELF) |
| Build-time JNI gate | ADDED (`scripts/verify_jni_exports.sh`, run by the APK build): 16 declarations / 16 exports |
| Gate non-vacuity | PROVEN (fails on zero declarations and on a .so with no JNI entry points) |
| APK + reinstall | 884,425 B, armeabi-v7a only; `adb install -r` Success, app data preserved |
| Launch | alive, native library loaded, 0 UnsatisfiedLinkError / 0 FATAL / 0 SIGSEGV / 0 ANR |
| 30-second ticker proof | **BLOCKED - PHONE LOCKED** (`mDreamingLockscreen=true`, activity STOPPED, tick counter 0) |
| H-03 acceptance (import, connect, consent, camera/mic, Wi-Fi) | **PENDING OPERATOR UNLOCK** |

## H-03 JNI fix - historical lab audit on test device

| item | state |
|---|---|
| UnsatisfiedLinkError | **FIXED + VERIFIED at runtime** (tick runs; both new entry points resolve) |
| 30-second ticker proof | **PASS**: heartbeats #20/#40/#60/#80, 10.25 s apart (20 x 500 ms), across a background/foreground cycle |
| Faults during the run | UnsatisfiedLinkError 0, app FATAL 0, SIGSEGV 0, SIGABRT 0 |
| JNI export gate | 16 Java declarations / 16 unmangled exports; build fails on any mismatch; non-vacuity proven |
| APK | 884,530 B, armeabi-v7a only, `adb install -r` Success (app data preserved) |
| H-03 acceptance (import credentials, connect, consent, camera/mic, Wi-Fi delivery, USB unplug) | **PENDING OPERATOR** - not assumed to have passed |

## H-03 consent lifecycle (fixed)

| item | state |
|---|---|
| Earlier denial reason (device) | `ambiguous - multiple active grants for the same purpose` |
| Root cause | the JNI store APPENDED a granted record on every press; the gate refuses >1 active grant |
| Fix | `SingleActiveGrantConsentRepo`: <=1 active grant per (subject,purpose); idempotent grant; withdrawal removes every granted record |
| Gate | **unchanged** - a test still forces `ambiguous` when genuinely conflicting grants are supplied |
| Tests | `[consent_lifecycle]` 13 cases / 45 assertions; full suite **477 / 5,609**, all passing |
| Denial visibility | reason + explanation in the returned string, the HGNode log, and `nativeConsentState()` polled each tick |
| `wi-fi: unknown` | **diagnosed**: `getActiveNetworkInfo()` needs `ACCESS_NETWORK_STATE` (was undeclared) -> SecurityException. Declared now (verified in the APK) |
| APK | 928,459 B, armeabi-v7a only, JNI gate 17 declarations / 17 exports, `adb install -r` Success |
| Physical sensor counters | **PENDING OPERATOR** - grant consent (deliberately more than once), then Start camera / Start microphone |

## Ingress health check (H-03 follow-up) - corrected

| item | state |
|---|---|
| Defect | the ingress inherited the HUB's image-level probe (HTTP on 8080) and reported `unhealthy` while serving mutual TLS on 8443 |
| Fix | service-specific probe `hg_ingress_probe` + `deploy/docker/ingress-healthcheck.sh` + a compose `healthcheck` override; the image-level hub probe is unchanged |
| What it verifies | real TLS handshake; server cert chained to the private CA AND covering the expected name/address; mutual-TLS demand reported separately - **not** a TCP port check, and **not** device authentication (`device_auth=NOT_PERFORMED`) |
| Acceptance (container test stage) | `scripts/verify_ingress_healthcheck.sh` - all checks pass, including the negatives (wrong name, non-TLS listener, missing CA, missing configured CA) |
| Regressions | D-03 TLS acceptance 24/24, TLS identity 9/9, CA isolation 14/14 (1 skipped), full ctest green |
| Live deployment | `docker inspect` -> `status=running health=healthy` (uid 10001, 0 restarts); the health log carries the probe's real output; the listener the phone dials is up on 10.20.30.6:8443 |
| Operator prerequisites confirmed | the mounted credential tree must be readable by the container uid (`sudo chown 10001:10001`, else exit 78) and the data volumes writable by it |
| H-03 physical acquisition / standalone Wi-Fi | **STILL PENDING** - operator-gated, not claimed |

## H-04 planning refinements + H-03 acceptance readiness

| item | state |
|---|---|
| Mail gate | **functional preservation**, not byte-identical output: availability, TLS certificate identity, mailbox integrity, DNS, delivery, latency. Queue depth/counters/logs are expected to move (owner review, refinement 1) |
| SSH | **unchanged** for this migration; any restriction deferred to a separate approval, only after alternative admin/recovery access is verified (refinement 2) |
| x402 retirement | only after HomeGuardian connectivity is **validated** (not merely configured) AND the rollback has been **exercised** from the **off-VPS** backup (refinement 3) |
| Read-only pre-flight bundle | `scripts/oracle_preflight_readonly.sh` - no writes, no privilege escalation (0 escalation commands), redacted output; **validated by running it on a real Linux host**: 11 sections, rc=0, no secret-shaped text, privilege-needing commands degrade to SKIPPED |
| H-03 acceptance harness | `scripts/verify_h03_physical_acceptance.sh` - checks the seven physical items from real device + hub evidence; items it cannot observe report NOT OBSERVED and FAIL (never a vacuous pass); `--post-usb` uses hub-side watermark evidence only, because adb is gone after the unplug |
| Phone instrumentation | the app now logs its counters every ~10 s (`counters video=... frames_processed=... samples=... obs_emitted=...` and `counters transport queued=... sent=...`), because UI scraping is unusable on this device. APK 928,615 B, JNI gate 17/17, reinstalled - and its line is **not yet observed** because the phone is locked (the tick runs only while the activity is resumed) |
| H-03 physical acceptance | **PENDING OPERATOR** - no result claimed or simulated |
| Oracle VPS | **not contacted, not modified** |

## Camera1 / NDK backend-selection defect (FIXED)

| item | state |
|---|---|
| Reported failure | `camera: initialization failed (NdkCameraDevice: no camera available)` - the Camera1 path was unreachable behind a failing NDK enumeration |
| Device evidence | `dumpsys media.camera` reports **2 camera devices**; the NDK camera2 API enumerates **0** on this MT6580/API 27 unit |
| Fix | explicit backend selection (`camera1` / `camera2` / `auto`), a native lifecycle representation for the Java Camera1 bridge (`Camera1Device`, Android-free and host-tested), a Java-start failure that clears the native capturing state and the video flag, a per-kind accountant stop, and a Java-side reconciler that releases Camera1 whenever the native layer leaves capture |
| Preserved | consent gate + per-frame authorization, CAMERA permission checks, withdrawal, activity-pause stop, bounds checks, capture accounting, and the NDK/camera2 backend for devices that support it |
| Tests | `[camera1]` 9 new cases / 46 assertions (delta-proven: `[camera1]` tag 6 -> 15 cases); full suite **486 / 5,652** (was 477 / 5,609) |
| Build | APK 965,451 B, JNI gate 21 declarations / 21 exports, `install -r` Success, clean launch, `camera availability: legacy(Camera1)=2` |
| Physical camera acceptance | **PENDING OPERATOR** - requires consent + Start camera, then real preview callbacks and rising frame counters; not claimed, not simulated |
| NDK camera2 hardware status | preserved, **NOT hardware-verified** (incomplete capture-session implementation; this device cannot exercise it) |

## Frame processing: 518 authorized frames, 0 processed (FIXED)

| item | state |
|---|---|
| Measured geometry | **720x576 NV21** - 622,080 bytes/frame exactly (322,237,440 / 518); byte accounting CORRECT |
| Exact rejection | `pixels <= FeatureBounds::kMaxPixels` - 414,720 > 307,200 (640x480); `++frames_processed` lives inside that block, so every authorized frame was skipped silently |
| Fix | `PreviewSizePolicy` selects the closest SUPPORTED size inside the bounds; the bridge REFUSES when nothing is processable; the frame path uses one bounds predicate; skips are counted by reason, logged rate-limited, and exposed as `proc_skip_*` + `last_geo` in every counters line |
| Not done on purpose | silently downsampling an oversized frame (it would change what the motion descriptors mean) and relaxing the bounds |
| Tests | `[previewsize]` 5 cases / 43 assertions, incl. the measured geometry as a regression; full suite **491 / 5,698** |
| Build | APK 997,465 B, JNI gate 22/22, install Success |
| Physical acceptance | **PENDING OPERATOR** - next Start camera reports the negotiation; rising `frames_processed` + real motion observations required, not claimed |
| Video flag | `video=off` in the sampled ticks is consistent: capture had stopped, and the 518 frames are cumulative - not evidence of active capture |


## MCP server (HG-MCP-01)

| item | state |
|---|---|
| Protocol | MCP **2025-11-25** (Streamable HTTP); also accepts 2025-06-18 / 2025-03-26 clients and answers anything else with 2025-11-25 |
| Tools | `get_homeguardian_status`, `get_alert_summary` - both read-only, both delegated to the existing `AlexaContract` |
| Transport | `POST /mcp` (JSON or SSE), `GET /mcp` -> 405 + Allow, `DELETE /mcp` -> session termination; sessions minted on initialize, idle-expired, capped |
| Security | bearer auth via `AlexaCredentialStore` (0600, owner-only, fail-closed, no fallback); Origin allowlist -> 403; loopback bind with an explicit opt-in required for anything else; header/body caps, timeouts, bounded workers and in-flight cap |
| Refusals | no credential store -> exit 3; world-readable credential file -> exit 3; non-loopback bind without opt-in -> exit 4; no unauthenticated mode exists |
| Tests | `[mcp]` **19 cases / 89 assertions**; full suite **510 cases / 5,787 assertions** (baseline before this work: 491 / 5,698) |
| End-to-end | `scripts/verify_mcp.sh` -> **43 passed / 0 failed** against the real binary over loopback |
| Not implemented | OAuth 2.1 client registration, TLS, server-initiated SSE stream, resources/prompts, per-token quotas, session resumability - see docs/MCP_SERVER.md section 7 |
| Unchanged | engine, web demo, NDJSON bridge, and every existing target; `main` untouched |
