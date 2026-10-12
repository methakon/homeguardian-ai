# HomeGuardian AI — Implementation Roadmap

## Phase A — Orchestration and Repository Foundation

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| A-01 | Initialize project and verify toolchain | — | C++20, CMake, spdlog, nlohmann/json, Catch2 all working | DONE |
| A-02 | Initialize Git repository | A-01 | Repo exists at `/home/swarna-sekhar-dhar/projects/homeguardian-ai` on `main` branch | DONE |
| A-03 | Create initial documentation | A-02 | README, ARCHITECTURE, ROADMAP, DEVELOPMENT, TESTING, ORCHESTRATION, DECISIONS, STATUS all exist and are committed | DONE |
| A-04 | Review and first commit | A-03 | All docs reviewed, `.gitignore` present, initial commit created | DONE |
| A-05 | Create skill inventory | A-03 | `docs/SKILL_INVENTORY.md` created with all relevant skills documented | DONE |
| A-06 | Update architecture for three-tier design | A-03 | `docs/ARCHITECTURE.md` updated with Tier 1/2/3 descriptions | DONE |

## Phase B — Minimal Vertical Slice (Tier 2 — PC-Side C++)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| B-01 | CMake build system | A-06 | `cmake` configures and `make` builds an empty executable | DONE |
| B-02 | Application entry point | B-01 | `main()` starts, initializes logging, loads config, enters main loop | DONE |
| B-03 | Structured logging | B-01 | spdlog initialized with configurable level and output | DONE |
| B-04 | Configuration loading | B-02 | JSON config loaded and validated; defaults applied for missing values | DONE |
| B-05 | Graceful shutdown | B-02 | SIGINT/SIGTERM handled; resources cleaned up; clean exit | DONE |
| B-06 | Initial test suite | B-01–B-05 | Catch2 tests pass for all above modules | DONE |

## Phase C — Event Model and Pipeline

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| C-01 | Core event types | B-06 | Event, Alert types defined; JSON serialization tested | DONE |
| C-02 | Event ingestion interface | C-01 | IEventSource interface + SimulatedEventSource implemented | DONE |
| C-03 | Event processing pipeline | C-01, C-02 | Pipeline: validate → normalize → correlate → alert | DONE |
| C-04 | Correlation rules | C-03 | TimeWindowCorrelation with configurable window | DONE |
| C-05 | Event validation | C-01 | EventValidator with deterministic normalization | DONE |
| C-06 | Config extension | C-03 | max_event_history, correlation_window_ms, alert_confidence_threshold | DONE |
| C-07 | Application integration | C-03 | Application::process_event() delegates to Pipeline | DONE |
| C-08 | Test suite | C-01–C-07 | 13 test cases covering all modules | DONE |

## Phase D — Persistence

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| D-01 | SQLite persistence | C-03 | Events, alerts, profiles, consent, routines stored and queried | DONE |
| D-02 | Retention policies | D-01 | Automatic data expiration per configurable policies | DONE |

## Phase E — Family Profiles, Consent, Routines and Configuration

This phase establishes a privacy-first, testable foundation before any camera,
audio, or heavier AI inference is introduced. It is deliberately separate from
the earlier "Sensing and Inference" plan, which is deferred to a later phase.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| E-01 | Family profile model | D-01 | Versioned profile with opaque ID, display name, optional age band; no sensitive attributes | DONE |
| E-02 | Consent model | D-01 | Explicit granted/denied/withdrawn states; default-deny; expiry; withdrawal blocks future processing | DONE |
| E-03 | Routine model | D-01 | Versioned routine with validated schedule and time zone; configuration only | DONE |
| E-04 | Persistence + migration | D-01 | Additive v1→v2 schema migration; FK cascade; transactional and rerunnable | DONE |
| E-05 | Configuration | D-01 | Validated Phase E config; media capture and cloud OFF by default | DONE |
| E-06 | Retention/export/deletion | D-01 | Documented lifecycle; profile deletion cascades to dependents; no media stored | DONE |
| E-07 | Tests | E-01–E-06 | Profiles, consent states, routines, restart recovery, FK/uniqueness, rollback, config | DONE |

## Phase E2 — Sensing and Inference (deferred)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| E2-01 | Simulated event source | C-03 | Generates synthetic events for testing | TODO |
| E2-02 | Camera interface + OpenCV | C-03 | Capture frames from video source | TODO |
| E2-03 | Face analysis (OpenCV DNN) | E2-02 | Face detection and emotion estimation | TODO |
| E2-04 | Audio capture + voice analysis | C-03 | Microphone input with VAD | TODO |
| E2-05 | ONNX Runtime integration | C-03 | Load and run ONNX models | TODO |

## Phase F1 — Simulated Sensing and Consent-Gated Inference

Establishes the consent gate and a simulated sensor path before any real
hardware. No real camera, microphone, biometric, or cloud processing.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F1-01 | Simulated sensor source | E-07 | Deterministic synthetic events; tagged synthetic; configurable timestamps; malformed input; disconnect/reconnect | DONE |
| F1-02 | Consent gate | E-02 | Single fail-closed decision point; checks profile, purpose, category, grant, expiry, withdrawal | DONE |
| F1-03 | Consent-gated acquisition | F1-01, F1-02 | Denied requests produce no payload; authorized requests acquire; synthetic only | DONE |
| F1-04 | Tests | F1-01–F1-03 | Granted/denied/missing/expired/withdrawn/unknown/mismatched; timestamps; malformed; reconnect; pipeline regression | DONE |
| F1-05 | Docs | F1-01–F1-04 | Threat model, gate placement, simulated-vs-real distinction in PRIVACY/ARCHITECTURE | DONE |

## Phase F2.1 — Device Interface and Lifecycle

Abstract real camera/audio behind an interface with consent-guarded lifecycle.
No real hardware integration in this milestone (no NDK/device available).

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F21-01 | IMediaDevice interface | F1-02 | Explicit states, start/stop, deterministic cleanup; no hardware code | DONE |
| F21-02 | ConsentGuardedDevice | F21-01, F1-02 | Gate required before init/start/acquire; withdrawal/expiry/permission/failure/shutdown force stop+release; fail-closed | DONE |
| F21-03 | Lifecycle tests (mock) | F21-01, F21-02 | Denied start, authorized start/stop, withdrawal, expiry, failure, permission revocation, destructor cleanup | DONE |
| F21-04 | Docs | F21-01–F21-03 | Interface/lifecycle, Android env finding, F2.2 milestone in PRIVACY/ARCHITECTURE | DONE |

## Phase F2.2 — Real Android Device Integration

Requires NDK install and a physical device. NDK installed and backends
cross-compile; hardware acceptance testing is NOT done (no device attached).

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F22-01 | Install NDK; confirm API level | F21-04 | NDK r26d installed side-by-side; API 34 assumed, device unconfirmed | DONE |
| F22-02 | Camera2 backend implementing IMediaDevice | F21-01, F22-01 | Cross-compiles for arm64-v8a; fail-closed until device confirmed | DONE (compile only) |
| F22-03 | AAudio backend implementing IMediaDevice | F21-01, F22-01 | Cross-compiles for arm64-v8a; fail-closed until device confirmed | DONE (compile only) |
| F22-04 | On-device lifecycle + permission tests | F22-02, F22-03 | Real permission revocation and device failure force stop | BLOCKED (no device) |
| F22-05 | Actual capture on a physical device | F22-04 | Verified real frame/sample capture gated by consent | BLOCKED (no device) |
| F22-06 | Delivery-time consent gate + fake-backend tests | F22-02, F22-03 | authorize_delivery() re-checks before every frame/sample; withdrawal/expiry/permission/failure stop delivery; resource cleanup on repeated start/stop/close | DONE (host mock) |
| F22-07 | Defect fix: errored device resurrection + camera stop handle release | F22-06 | Errored device stays Error until re-init; stop() releases capture handles | DONE |
| F22-08 | armeabi-v7a / API 27 build + on-device non-capture self-test | F22-06 | Builds for confirmed device; runs on device via adb; 17/17 non-capture checks pass | DONE |
| F22-09 | Real camera/audio capture in APK harness + on-device capture acceptance | F22-08 | Wired NDK callbacks gated by authorize_delivery; permission revocation/withdrawal stop delivery on device | BLOCKED (needs APK + runtime permissions + approval) |
| F22-10 | Camera1-JNI adapter (legacy android.hardware.Camera) + host contract tests | F22-09 | Camera1Bridge + nativeOnPreviewFrame delivery gate; 6 host contract tests; APK builds; on-device launch clean (no capture) | DONE (host + build + launch) |
| F22-11 | Camera1 capture acceptance on device | F22-10 | Real Camera1 preview frames gated by authorize_delivery; withdrawal stops delivery on device | BLOCKED (awaiting approval) |
| F22-12 | Ubuntu (Linux) sensor backend: discovery + interfaces + host tests | — | Non-capture SensorDiscovery (V4L2/PipeWire metadata); consent-enforcement host tests reusing ConsentGuardedDevice | DONE (non-capture, host) |
| F22-13 | Ubuntu camera/mic/speaker capture adapters | F22-12 | V4L2 camera + ALSA/PipeWire mic + speaker adapters behind IMediaDevice; delivery-gated; bounded buffers | PROPOSED (awaiting approval) |
| F22-14 | Docker container for C++ core service | — | Documented trade-offs; no privileged container; sensors stay on host; consent gate in delivery path | PROPOSED (not implemented) |
| F22-15 | Alexa+ integration (custom/Smart Home skill) | — | AVS deprecated for Alexa Built-in; viable route = Alexa skill → authenticated endpoint → C++ service | RESEARCH ONLY (not integrated) |
| F22-16 | Simulator-first backend + host safety tests | — | Deterministic SimulatedSensorBackend behind IMediaDevice/ConsentGuardedDevice; 12 host tests (lifecycle, consent deny/withdraw/expire, delivery boundary, bounded buffers, error recovery, shutdown) | DONE (host, simulator) |
| F22-17 | Real Linux capture (V4L2/ALSA/PipeWire) or Docker env if simulator insufficient | F22-16 | Only if a requirement is proven un-simulatable; simulator currently covers all in-scope behaviors | DEFERRED (simulator sufficient) |
| F22-18 | Alexa custom skill (ASK SDK v2) + handler tests | — | HomeStatusIntent/GetAlertSummaryIntent/GetRoutineStatusIntent/Help/Stop with mock backend behind a documented interface; 13 Node tests pass | DONE (handler + unit tests) |
| F22-19 | Alexa skill deployment + console-simulator verification | F22-18 | Deploy to Alexa-hosted skill (auto Lambda, $0); build interaction model; run text simulator; record results | OPTIONAL (manual console steps — see alexa_skill/README.md) |
| F22-20 | Realistic Linux sensor simulation (config, signals, speaker) | F22-16 | Config struct: camera dimensions + frame-rate pacing; mic sample_rate/channels + noise/sine/square/silence signals; speaker play/error; configurable ring capacity | DONE (host, simulator) |
| F22-21 | End-to-end Linux simulator workflow + failure/consent scenarios | F22-20 | camera+mic+speaker → consent-gated delivery → synthetic test processor → pipeline rule/alert → safe speaker response; ordering, bounded memory, cleanup; failure & consent scenarios | DONE (host, simulator) |
| F22-22 | Product-readiness audit (build reproducibility, docs accuracy, portability) | F22-21 | Reproduced documented build in a fresh dir; fixed stale README + hardcoded CMake paths; hardened .gitignore; full regression + sanitizers green | DONE (audit) |
| F22-23 | Dual logical-device simulation over a shared Ubuntu audio broker | F22-22 | Two independent logical devices (simulated_alexa_enabled_device, simulated_alexa_assistant); one shared mic fan-out with per-consumer consent; bounded arbitrated speaker output; failure/consent/shutdown scenarios | DONE (host, simulator) |
| F22-24 | Product-readiness + reproducibility audit | F22-23 | Reproduced documented build; refreshed STATUS/DEVELOPMENT; full regression + sanitizers green | DONE (audit) |
| F22-25 | Runtime integration: wire simulator into the running application | F22-24 | SimulatorRuntime + Application::run() drive the consent gate, shared audio broker, event pipeline, and clean shutdown; 7 runtime integration tests; simulator-only | DONE (app lifecycle) |
| F22-26 | Runtime event-to-alert integration | F22-25 | Authorized synthetic observations routed through Application::process_event → Pipeline validation + TimeWindowCorrelation + alert; 9 event-to-alert tests; capture-off/withdrawal/missing-consent produce no events | DONE (app event path) |
| F22-27 | Runtime hardening v1 (alert policy, lifecycle, config, stress) | F22-26 | AlertPolicy dedup/cooldown/re-arm (injectable clock, bounded); explicit AppState lifecycle + partial-startup cleanup; config fail-closed; 27 hardening tests; reproducible demo | DONE (app hardening) |
| F22-28 | Runtime readiness v2 (durable audit + observability + restart) | F22-27 | PersistenceAudit (alert metadata via existing SQLite, historical-vs-current, retention, fail-safe degraded) + RuntimeStatus (bounded privacy-safe counters); restart distinguishability verified; release reproduction | DONE (app readiness) |
| F22-29 | Runtime readiness v3 (integration contract, storage integrity, privacy) | F22-28 | AlexaContract (versioned, authenticated, read-only integration contract + auth boundary; local mock tests only — live deploy pending); SQLite integrity/migration/retention/recovery tests; privacy/consent regression tests; P1 rule-specific clearing; P2 scheduled retention | DONE (local contract + integrity + privacy; live Alexa integration NOT deployed) |
| F22-30 | Runtime readiness v4 (local end-to-end integration proof) | F22-29 | hg_alexa_bridge (local NDJSON stdio bridge over the real AlexaContract) + Node process-boundary integration (14) and restart (4) tests driving the actual compiled C++ binary; reproducible demo; no network/deploy | DONE (local process-boundary proof; live Alexa integration NOT deployed) |
| F22-31 | Security readiness v5 (deployment-readiness & security review) | F22-30 | Security review of the Alexa boundary (docs/SECURITY_READINESS_V5.md); fixed LOW-1 (bridge reflects real config capture state, fail-safe); +6 C++ `[alexa]` +2 Node security-regression cases; HTTPS endpoint CONDITIONAL GO (blocked on real authn/TLS/rate-limit/secrets) | DONE (review + fixes; no deploy) |
| F22-32 | Security readiness v6 (authentication & secret management: B1+B4) | F22-31 | AlexaCredentialStore (real bearer-token verification: expiry/revocation, constant-time compare, 0600 secure-file load, fail-closed, no mock fallback); bridge HG_BRIDGE_SECURE_CREDS mode; 14 `[creds]` + 5 secure-mode Node cases; docs/SECURITY_READINESS_V6.md | DONE (authn/secrets real; LWA provider NOT implemented — needs Amazon creds) |
| F22-33 | HTTP adapter v7 (local-only loopback adapter) | F22-32 | src/bridge/hg_http_adapter.js (loopback-only HTTP over the real C++ bridge; stdlib http, no new dependency; method/route allowlists, size/timeout/concurrency, rate limiting, generic auth failures, graceful shutdown); 18 http_adapter Node tests; docs/HTTP_ADAPTER_V7.md | DONE (local dev only; NOT TLS, NOT production; non-local exposure NO-GO pending B2+real identity) |
| F22-34 | AmazonAppDev 2026 hackathon compliance audit | F22-33 | Audit vs official rules; chose Alexa+ Route B (simulated web experience — official rules permit it, no MCP required); AWS Builder + Open Source mini-challenges DO NOT QUALIFY; blockers: README overclaims (MCP/Ring), no LICENSE on public repo, no user-facing demo; docs/AMAZONAPPDEV2026_FINAL_AUDIT.md; CONDITIONAL GO pending P0 | DONE (audit only — no code changes) |
| F22-35 | Hackathon P0: honest README + simulated Alexa+ web demo | F22-34 | README corrected (removed false MCP/Ring claims; implemented/simulated/planned split); simulated Alexa+ web experience (src/web/ + src/bridge/hg_demo_server.js + demo.sh + config/homeguardian.demo.json) driving the REAL backend via a new `run_scenario` bridge mode (real events → alerts → dedup → durable persistence); 8 web_demo Node tests; docs/DEMO_WEB.md; fixed CMake out-of-source path bug + test CWD-isolation bug for clean-clone reproducibility | DONE (P0-1, P0-3, P0-4); LICENSE still pending approval (P0-2) |

Verification status is tracked separately per layer:
- Compile (Android arm64-v8a/API 34): VERIFIED.
- Compile (Android armeabi-v7a/API 27): VERIFIED.
- Mock integration (host fake backend): VERIFIED.
- Camera1 adapter contract (host): VERIFIED (6 cases).
- APK build + on-device launch with Camera1 backend (no capture): VERIFIED.
- Physical hardware capture (Camera1): NOT YET RUN — awaiting explicit approval.
- Ubuntu sensor discovery + consent host tests (non-capture): VERIFIED (4 cases).
- Simulator-first backend + host safety tests (non-capture): VERIFIED (12 cases).
- Realistic Linux sensor simulation (config/signals/speaker): VERIFIED (11 more cases; 23 total).
- End-to-end simulator workflow + failure/consent scenarios: VERIFIED (9 `[e2e]` cases).
- Sanitizers (ASan+UBSan) full suite: VERIFIED clean (135 cases / 1472 assertions).
- Documented build reproduction (fresh out-of-source dir): VERIFIED (135/1472; portable CMake).
- Dual logical-device + shared audio broker simulation: VERIFIED (12 `[dual]` cases).
- Runtime integration (running Application drives the simulator): VERIFIED (7 `[runtime]` cases; capture-off delivers no media + clean shutdown; capture-on delivers to permitted consumers, bounded speaker).
- Runtime event-to-alert (authorized observations → process_event → Pipeline → alert): VERIFIED (9 `[event2alert]` cases; capture-off/withdrawal/missing-consent produce no events; running app fires 9 synthetic alerts with pipeline_history=10).
- Runtime hardening v1 (alert dedup/cooldown, lifecycle, config fail-closed, stress): VERIFIED (11 `[alertpolicy]` + 17 `[lifecycle]` + 5 `[hardening]` cases; reproducible demo `docs/DEMO_SIMULATOR.md`).
- Runtime readiness v2 (durable alert audit via existing SQLite, observability counters, restart distinguishability, retention, fail-safe degraded): VERIFIED (7 `[audit]` + 3 `[status]` cases; fresh out-of-source release reproduction).
- Runtime readiness v3 (versioned authenticated read-only Alexa integration contract, SQLite integrity/migration/retention/recovery, privacy/consent regression, P1 rule-specific clearing, P2 scheduled retention): VERIFIED locally (14 `[alexa]` + 12 Node adapter + 7 `[integrity]` + 7 `[privacy]` cases). LIVE Alexa integration NOT deployed (needs an authenticated endpoint) — documented as pending.
- Runtime readiness v4 (local end-to-end integration over a real process boundary): VERIFIED locally (`hg_alexa_bridge` NDJSON stdio over the REAL AlexaContract; 16 Node integration + 4 restart cases against the actual compiled binary; reproducible demo `docs/demo_alexa_integration.sh`). No network listener, no deploy, no physical sensor.
- Security readiness v5 (deployment-readiness & security review): VERIFIED locally — fail-closed auth, read-only ops, no credential leak, no network listener, no shell exec, bounded lifetime, build/supply-chain hygiene (`docs/SECURITY_READINESS_V5.md`). HTTPS endpoint = CONDITIONAL GO, blocked on real authn/TLS/rate-limit/secrets. NOT deployed.
- Security readiness v6 (authentication & secret management, B1+B4): VERIFIED locally — `AlexaCredentialStore` real bearer-token verification (expiry/revocation, constant-time compare, 0600 secure-file load, fail-closed, no mock fallback); bridge `HG_BRIDGE_SECURE_CREDS` mode; 14 `[creds]` + 5 secure-mode Node cases (`docs/SECURITY_READINESS_V6.md`). LWA/OAuth provider NOT implemented (needs Amazon credentials). Next HTTP-adapter stage = CONDITIONAL (B2/B3/B5 still open).
- HTTP adapter v7 (local-only loopback HTTP adapter): VERIFIED locally — `src/bridge/hg_http_adapter.js` binds 127.0.0.1 only, reuses the real C++ bridge/contract/credential-store, stdlib `http` (no new dependency); method/route allowlists, size/timeout/concurrency limits, rate limiting, generic auth failures, graceful shutdown; 18 `http_adapter` Node cases (`docs/HTTP_ADAPTER_V7.md`). NOT TLS, NOT production; non-local exposure NO-GO pending B2 + real identity.
- AmazonAppDev 2026 hackathon compliance audit: DONE (audit only) — official rules inspected 2026-10-12; **Alexa+ Route B (simulated web experience) selected** (official rules permit it; no MCP/framework required, but simulation source must be in-repo and shown working); **AWS Builder: DOES NOT QUALIFY** (no AWS service/SDK/Kiro usage); **Open Source: DOES NOT QUALIFY** (no verifiable upstream contribution in window); blockers = README overclaims (MCP/Ring absent), no LICENSE on a public repo, no user-facing demo; **CONDITIONAL GO pending P0-1..P0-5** (`docs/AMAZONAPPDEV2026_FINAL_AUDIT.md`). Deadline Oct 23 2026 12:00 PDT = Oct 24 2026 00:30 IST.
- Hackathon P0 (honest README + simulated Alexa+ web demo): DONE for P0-1/P0-3/P0-4 — README corrected to distinguish implemented vs simulated vs planned (false MCP/Ring claims removed); `src/web/index.html` + `src/bridge/hg_demo_server.js` + `demo.sh` + `config/homeguardian.demo.json` provide a one-command loopback-only web demo that drives the REAL C++ backend (new bridge `run_scenario` mode → real events → alerts → dedup → durable SQLite persistence); 8 `web_demo` Node cases; `docs/DEMO_WEB.md`. Clean-clone reproducibility fixed (CMake absolute paths + `HG_SOURCE_DIR`). **P0-2 (LICENSE) still pending explicit approval** — repo is already PUBLIC with no license.
- Alexa custom skill handler unit tests (no deployment): VERIFIED (13 Node cases).
- Alexa console-simulator end-to-end: OPTIONAL — requires manual console steps (F22-19).
- Ubuntu camera/mic/speaker capture: NOT IMPLEMENTED — awaiting explicit approval.
- Docker deployment: NOT IMPLEMENTED (deferred — simulator sufficient for current scope).
- Alexa+ integration: NOT INTEGRATED (research only).

Confirmed device: MediaTek MT6580 "Ruby" reference device (model string
"S25_Ultra" is cosmetic and not corroborated), Android 8.1.0 / API 27,
armeabi-v7a (32-bit only), ~962 MB RAM, 2 cameras, microphone present.

## Phase F — HTTP and MCP

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| F-01 | HTTP server (Boost.Beast) | C-03 | REST API for dashboard and alerts | TODO |
| F-02 | MCP Streamable HTTP server | F-01 | MCP protocol implemented and tested | TODO |
| F-03 | Alexa+ integration | F-02 | Alexa+ can invoke HomeGuardian tools | TODO |

## Phase G — Dashboard and Notifications

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| G-01 | Web dashboard | F-01 | HTML/CSS/JS dashboard shows events and status | TODO |
| G-02 | Notification service | C-03 | Email/push/webhook alerts | TODO |
| G-03 | Ring simulator | C-03 | Simulated Ring events for development | TODO |

## Phase H — Safety and Hardening

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| H-01 | Fall detection pipeline | E-02, E-03 | Pose-based fall detection with configurable sensitivity | TODO |
| H-02 | Emergency workflow | G-02 | Configurable escalation: notify → call → emergency services | TODO |
| H-03 | Privacy audit | D-01 | All data access logged; consent enforcement verified | TODO |
| H-04 | Security review | All | Input validation, injection prevention, access control | TODO |

## Phase L — Observation and inference framework (added 2026-10-12)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| L-01 | Observation model (provenance, detector/model id, explicit status) | Event model | Validates; converts to `EventType::inference`; bounded payload | DONE |
| L-02 | Explainable anomaly detection | L-01 | z-score baselines, warm-up, missing-observation, bounded memory, reason on every flag | DONE |
| L-03 | Multimodal fusion | L-01 | Confidence propagation, conflict→inconclusive, time window, unavailable handling | DONE |
| L-04 | Synthetic activity detector | L-01 | Deterministic, no I/O, out-of-range→inconclusive, feeds demo/tests | DONE |
| L-05 | Detection test coverage | L-01..L-04 | 20 cases / 67 assertions; full suite 275/2071; ctest 100%; Node 76/76 | DONE |
| L-06 | Real face/pose visual inference | OpenCV/ONNX/MediaPipe (none installed) | Actual inference on a real/fixture image | DEFERRED — no ML stack on 8 GB host; pulling one needs licensing/build/memory review |
| L-07 | Real audio VAD / event detection | audio ML stack (none installed) | VAD + input-quality checks on synthetic PCM | DEFERRED — same |
| L-08 | Lip-reading | offline model + compatible license (none present) | No fabricated transcripts | DEFERRED — no suitable offline model present |

**Blocked / deferred rationale (L-06..L-08):** no OpenCV, ONNX Runtime,
MediaPipe, torch, or cv2 is installed on this 8 GB machine. Adding one requires
the licensing / binary-size / memory / build-impact review that project policy
mandates before any large ML dependency. The consuming framework (observation
model, anomaly detection, fusion) is implemented and tested now so a real
detector plugs in without touching the pipeline.

**Not in scope / never:** covert identity recognition, continuous personal
tracking, behavioural profiling, or learning new authorization rules.

### Phase L — demo integration (added 2026-10-12, v10)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| L-09 | Wire detection/fusion into the demo journey | L-01..L-05 | run_demo_scenario runs SyntheticActivityDetector + FusionEngine, feeds inference events through the existing pipeline, and surfaces a synthetic detection block in the scenario JSON + web UI; fail-closed capture-off path unchanged | DONE |
| L-10 | Keep detection behind the consent gate | L-09 | capture-off => detection stage does not run, events_processed=0, no detection block | DONE (verified) |
