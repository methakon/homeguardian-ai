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
| F-01 | HTTP server | C-03 | REST API for dashboard and alerts | DONE (in scope) — implemented as the native C++ loopback transport `hg_alexa_bridge --http` (GET-only subset, no external dependency). Boost.Beast deliberately NOT adopted (dependency-avoidance). Broader/production REST (auth, TLS, full HTTP) remains absent. |
| F-02 | MCP Streamable HTTP server | F-01 | MCP protocol implemented and tested | DEFERRED / SUPERSEDED — Route B selected (simulated Alexa+ experience); no MCP server is implemented or claimed. |
| F-03 | Alexa+ integration | F-02 | Alexa+ can invoke HomeGuardian tools | DEFERRED — no live Alexa+ identity/device integration. The in-repo simulated Alexa+ web experience is NOT equivalent to production Alexa+ integration. |

## Phase G — Dashboard and Notifications

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| G-01 | Web dashboard | F-01 | HTML/CSS/JS dashboard shows events and status | DONE — vanilla HTML/CSS/JS dashboard (`src/web/index.html`), embedded in the native binary, shows status/alerts/detection/privacy-audit and drives the real backend. Built on the native transport (F-01), not Boost.Beast. |
| G-02 | Notification service | C-03 | Email/push/webhook alerts | DEFERRED — not implemented (out of v16 scope). |
| G-03 | Ring simulator | C-03 | Simulated Ring events for development | DEFERRED — not implemented; no Ring integration is claimed. |

## Phase H — Safety and Hardening

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| H-01 | Fall detection pipeline | E-02, E-03 | Pose-based fall detection with configurable sensitivity | TODO |
| H-02 | Emergency workflow | G-02 | Configurable escalation: notify → call → emergency services | TODO |
| H-03 | Privacy audit | D-01 | All data access logged; consent enforcement verified | DONE |
| H-04 | Security review | All | Input validation, injection prevention, access control | DONE (native HTTP transport reviewed + hardened; see docs/SECURITY_READINESS_V8.md) |

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

### Phase H — privacy audit implementation (added 2026-10-13, v13)

H-03 is now implemented as a bounded, thread-safe, metadata-only audit log of
consent-gated data-access decisions.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| H-03a | PrivacyAuditLog component | ConsentGate | Bounded ring buffer + counters; string fields clamped; never throws; thread-safe; entries carry scope/purpose/category/decision/reason/acquired only (no raw media) | DONE |
| H-03b | Consent enforcement invariant | H-03a | `verify_enforcement()` returns false if any entry records an acquisition that was not authorized; unit-tested with a deliberately bogus entry | DONE |
| H-03c | Wire into the data-access path | H-03a | `ConsentGatedAcquisition` and `SimulatorRuntime` accept an optional audit observer (non-owning, passive — never changes a decision); every delivery/acquisition decision is recorded | DONE |
| H-03d | Surface in the demo | H-03c | `run_demo_scenario` reports an `privacy_audit` block (entries/authorized/denied/acquired/enforcement_ok); the web UI displays it; capture-off shows a zeroed, enforcement-OK trail | DONE |
| H-03e | Tests | H-03a..H-03d | 11 new cases (unit + integration): authorized/denied logging, reasons, bounded capacity, field clamping, enforcement invariant (incl. negative), clear, concurrency, passive-observer; full suite 286 cases / 2120 assertions | DONE |

**Scope note:** the audit log is in-memory + bounded and is surfaced through the
demo/API. Durable database persistence of audit entries (via the existing SQLite
layer) is a deliberate follow-up and is not required by the H-03 acceptance
criteria; the structured logger already emits a durable record.

## Phase N — Native HTTP demo transport (added 2026-10-13, v14)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| N-01 | Review the committed `NativeHttp.h` foundation | — | Compiles clean; framing/size/timeout/method/route review; defects corrected before use | DONE (4 defects fixed) |
| N-02 | Native integration into the C++ bridge | N-01 | `hg_alexa_bridge --http` serves `/`, `/index.html`, `/api/health`, `/api/status`, `/api/run-scenario` using the real backend + SQLite; API contracts preserved | DONE |
| N-03 | Embed the dashboard; drop the Node runtime dependency | N-02 | HTML embedded via CMake; `demo.sh` starts ONE native process; Node not invoked | DONE |
| N-04 | Per-run semantics for a single long-lived process | N-02 | Repeated scenario runs each deliver a fresh alert and grow durable history (matches legacy per-process behavior) | DONE |
| N-05 | H-04 security review + adversarial tests | N-02 | Input validation, Host/Origin enforcement, CSP, resource limits; 18 `[nativehttp]` cases incl. handler-not-reached-on-rejection | DONE |
| N-06 | Footprint benchmark (native vs legacy Node) | N-03 | Measured RSS/CPU/startup/latency/process count/deps via `scripts/bench_demo.sh`; recorded (incl. the unflattering run-scenario latency) | DONE |
| N-07 | Legacy Node adapter retained as reference | N-03 | `src/bridge/hg_demo_server.js` kept; `HG_DEMO_LEGACY_NODE=1` path; not required by default | DONE |

## Phase O — Scenario latency investigation (added 2026-10-13, v15)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| O-01 | Reproduce the v14 scenario-latency gap | N-03 | 40-trial Release measurement with median/p95/variability | DONE (native 46.5 ms median / 437 ms p95) |
| O-02 | Phase-profile the scenario path | O-01 | Steady-clock phases via default-off `HG_PROFILE=1`; no sensitive data | DONE (99 % in `persist`) |
| O-03 | Isolate the cause | O-02 | Control + `strace` + isolated `sqlite3` measurement | DONE (SQLite `synchronous=FULL` fsync ≈ 93 ms/commit) |
| O-04 | Safe optimization | O-03 | Improve latency **without** durability/security/footprint regressions | **CLOSED (v16)** — decision: KEEP `synchronous=FULL`; storage-related latency accepted. No safe lever exists; durability retained. See `docs/DECISIONS.md` (D-sqlite-synchronous-full-retained). |
| O-05 | Keep instrumentation, default-off, reported | O-02 | `HG_PROFILE=1` inert when unset; no credential/media leakage | DONE |

**Phase F / G review vs the implemented native transport** (requested):

- **F-01 "HTTP server (Boost.Beast) — REST API for dashboard and alerts":**
  **superseded / satisfied in scope.** A loopback REST-ish API for the dashboard
  and alerts now exists as the **native `hg_alexa_bridge --http`** transport
  (GET-only subset, no external dependency), rather than Boost.Beast. The
  original dependency choice is deliberately NOT adopted (dependency-avoidance
  goal). Not a production HTTP server: no TLS, no real auth, handwritten subset.
- **G-01 "Web dashboard (HTML/CSS/JS) — shows events and status":**
  **implemented**, but built on the native transport (F-01 above) rather than the
  never-built Boost.Beast F-01. The dashboard is vanilla HTML/CSS/JS
  (`src/web/index.html`), embedded into the binary, showing status, alerts, the
  detection stage, and the privacy audit; it drives the real backend.
- **F-02 (MCP), F-03 (Alexa+ integration), G-02 (notifications), G-03 (Ring
  simulator), H-01 (fall detection):** **NOT implemented / deferred** — unchanged.
  No MCP, live Alexa, physical sensing, or production HTTP deployment is claimed.

## Phase P — Licensing and release readiness (added 2026-10-13, v16)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| P-01 | Close the SQLite durability decision | O-03/O-04 | `synchronous=FULL` retained; verified in code (WAL) and at runtime (`synchronous=2`); no durability change | DONE |
| P-02 | Apache-2.0 license (hackathon P0-2) | user approval | Root `LICENSE` = unmodified Apache-2.0 (sha256-verified vs apache.org); `NOTICE` with attribution; README updated | DONE |
| P-03 | Third-party attribution review | P-02 | SQLite/Catch2/spdlog/nlohmann-json (and optional ask-sdk) licenses identified and preserved; no relicensing | DONE |
| P-04 | Roadmap reconciliation (F-01/G-01/F-02/F-03) | — | Accurate status: local demo HTTP + dashboard DONE in scope; MCP/Alexa+/notifications/Ring DEFERRED; Boost.Beast not adopted | DONE |
| P-05 | Release-readiness verification | — | Clean-clone Release build, C++/CTest/ASan, native adversarial suite, Node-free demo, loopback, persistence, capture-off, privacy, graceful shutdown, no artifacts committed | DONE |

**Completed records preserved:** H-04 (security hardening for the documented
local transport scope) is DONE; Phase N N-01..N-07 are DONE; Phase O established
the v15 performance root cause and the durability decision is now resolved with
**no code-level durability change**.

## Phase Q — Multi-scenario demo with explainable alerts (added 2026-10-13, v17)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| Q-01 | Deterministic scenario catalogue (core) | L-09 | Data-only fixtures (no pre-baked outcomes); ids form an allowlist; default entry preserved | DONE |
| Q-02 | Scenario runner through the REAL pipeline | Q-01 | Runs SimulatorRuntime -> event sink -> Pipeline -> correlation -> AlertPolicy; no second engine | DONE |
| Q-03 | Scenario A normal | Q-02 | Activity detected, but no alert (below the >=2 correlation threshold) | DONE |
| Q-04 | Scenario B unusual | Q-02 | Alert traceable to the rule/evidence; duplicates suppressed | DONE |
| Q-05 | Scenario C inconclusive | Q-02 | Detector + fusion preserve inconclusive/unavailable; no alert, no fabricated confidence | DONE |
| Q-06 | Scenario D consent denied | Q-02 | Denied acquisition blocked; no event from the denied consumer; enforcement_ok true | DONE |
| Q-07 | Real alerts captured + persisted | Q-02 | The alert persisted is the pipeline output (the old hardcoded placeholder is removed) | DONE |
| Q-08 | Scenario isolation in one process | Q-02 | Consent state reset per run; results independent of execution order | DONE |
| Q-09 | Allowlisted scenario selection over HTTP | Q-02 | scenario= strictly validated; unknown rejected; Host/Origin/loopback preserved | DONE |
| Q-10 | Dashboard: selector + backend-derived detail + safe DOM | Q-07 | Real alert severity/rule/message/explanation; textContent only; explains what/why/persisted/consent | DONE |
| Q-11 | Honest simulated assistant | Q-10 | Answers grounded in real status + last scenario; explicitly not a language model | DONE |
| Q-12 | Tests + docs + footprint | Q-03..Q-11 | Per-scenario tests, isolation, persistence, malformed ids, injection-safety; footprint documented | DONE |

## Phase R — L-11 detection-driven, explainable alerts (added 2026-10-13, v18)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| L-11 | Detection-driven alert generation | L-01..L-05, L-09 | The alert exists only because the detection/fusion evidence qualified; the previous independent raw-observation rule is removed | DONE |
| R-01 | DetectionFusionRule (core) | FusionEngine | Fuses reconstructed detection observations; alerts only on `detected` >= confidence floor with >= 2 usable votes; never on inconclusive/unavailable/not_detected/conflict | DONE |
| R-02 | Observation::from_event round-trip | Observation | Rebuilds an Observation from the inference event carried by the pipeline; rejects non-inference/unknown-status events | DONE |
| R-03 | Evidence-derived severity + metadata | R-01 | severity from fused confidence; `details` carry fusion_verdict/confidence/contributing/inconclusive/provenance/reason | DONE |
| R-04 | Detection gated by authorized acquisition | R-01 | No acquisition (capture off / consent withdrawn / no authorized consumer) => no detection step => no detection-driven alert | DONE |
| R-05 | Alert metadata surfaced (API + dashboard) | R-03 | Per-alert reason/severity/provenance from backend state; safe DOM only | DONE |
| R-06 | Uncertainty + regression tests | R-01..R-05 | 9 `[l11]` cases prove the causal path and the negative cases; scenario suite updated to measured results | DONE |

## Phase S — L-12 statistical anomaly detection in the live pipeline (2026-10-13)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| S-01 | AnomalyMonitor core component | AnomalyDetector | Feeds the bounded numeric feature of each AUTHORIZED acquisition; maps results to detection Observations; bounded, non-sensitive status | DONE |
| S-02 | Detection Observations through the SAME pipeline | S-01, L-11 | Validation -> history -> detection rule -> alert policy; no second pipeline, no synthetic alert | DONE |
| S-03 | Explicit evidence allowlist | L-11 | DetectionFusionRule accepts only allowlisted detector ids as votes; anomaly evidence advisory by default; influence explicit + causal + tested | DONE |
| S-04 | Scenario catalogue: warm-up / normal / deviation / missing | S-01 | Deterministic baseline before deviations; deliberate gap probe; documented per-run baseline reset | DONE |
| S-05 | API + dashboard exposure | S-01 | baseline ready, phase, status (warm-up/normal/high/low/missing), deviation, mean/sd, z-score, reason, gap reason, relationship to the alert | DONE |
| S-06 | Tests + regression + stale-binary safeguard | S-01..S-05 | 17 `[l12]` cases incl. warm-up, normal, deviation, missing, uncertainty, consent/capture gating, repetition, L-11 preservation, persistence/privacy; every shipped target rebuilt before recording results | DONE |


## Phase T — H-01s synthetic critical impact + severity prioritisation (added 2026-10-13)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| H-01s | SIMULATED critical impact scenario | S-01..S-06 | `synthetic-critical-impact`: normal activity, then a high-intensity transient, then a completing stillness sample. The critical alert exists only because the evidence qualified | DONE |
| T-01 | `SyntheticImpactClassifier` (core) | Observation | Deterministic transient-then-stillness pattern with documented, tested thresholds (transient >= 0.80, stillness <= 0.25, window 4 samples). A lone transient is `inconclusive` — never a confirmation | DONE |
| T-02 | `SyntheticImpactRule` (correlation) | T-01, AlertPolicy | Fires ONLY on a confirmed pattern with >= 2 supporting impact observations; maps a confirmed pattern to `Severity::critical`; message + `details` state that this is not fall detection and no emergency service is contacted | DONE |
| T-03 | Severity prioritisation in `Pipeline` | T-02 | Every registered rule is evaluated and the HIGHEST severity is returned (info < warning < critical), so a critical impact alert is never lost behind an unrelated info/warning alert; single-rule behaviour is unchanged | DONE |
| T-04 | Lossy round-trip fix (`Observation::from_event`) | R-02 | The reconstructed Observation carries the bounded feature payload. Previously only activity `energy`/`detect_threshold` survived, so a detector-specific feature (the impact `phase`) was invisible to every rule | DONE |
| T-05 | Scenario + API + dashboard | T-01..T-04 | Id in the strict allowlist and the selector; the API returns the classifier's real result plus the alert's non-sensitive evidence; the dashboard labels it SIMULATED and never claims a real fall or an emergency response | DONE |
| T-06 | Tests + regression + stale-binary safeguard | T-01..T-05 | 18 `[h01s]` cases: normal, lone signal, confirmed pattern, evidence derivation, prioritisation, duplicate suppression, rule independence, re-arm, capture-off, denied/expired/ambiguous consent, inconclusive, repetition, persistence + restart, allowlist, L-11/L-12 preservation, no sensing/emergency contact | DONE |

### Boundary — kept explicit and unchanged

| Item | Status |
|------|--------|
| **H-01s** synthetic impact-prioritisation demonstration | **IMPLEMENTED (simulation only)** |
| **H-01** real pose-based fall detection | **NOT implemented** |
| **H-02** emergency workflow / escalation | **NOT implemented** |
| **G-02** external notifications | **NOT implemented** |


## Phase U — v21 final integration and stability verification (added 2026-10-13)

Release-candidate pass: verify the existing product end to end and correct
verified defects only. No new features, no architectural change, no merge.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| U-01 | Fresh out-of-source Release build of every shipped target | — | `homeguardian`, `homeguardian_tests`, `hg_alexa_bridge` build clean (0 errors) from scratch, outside the source tree; every runtime figure taken from those binaries | DONE |
| U-02 | Full scenario-catalogue verification | U-01 | All 10 registered scenarios exercised with asserted expectations (consent, capture, observations, events, detection, anomaly, fusion, alerts gen/delivered/suppressed, severity, persistence, audit) | DONE |
| U-03 | HTTP contract: unsupported scenario is 400 | U-01 | Handler owns the business status; unknown scenario answers 400 with the existing generic JSON error body; transport validation unchanged; regression asserts status **and** body | DONE |
| U-04 | Logger must never abort the process | U-01 | An unwritable working directory degrades to console-only logging instead of SIGABRT; regression test | DONE |
| U-05 | Bounded retention for the bridge's alert store | U-01 | The configured `alert_retention_days` is applied to the bridge's own durable store (startup + bounded periodic, only on write); `scripts/verify_retention.sh` regression | DONE |
| U-06 | End-to-end, privacy, persistence, lifecycle and stress verification | U-01..U-05 | Browser smoke test, adversarial matrix, alternating/repeated runs, corrupted-DB and read-only-dir handling, restart recovery, graceful SIGTERM, resource stability | DONE |
| U-08 | Test fixtures safe under concurrent runs | U-01 | Eleven fixtures no longer use fixed CWD-relative database names; two suite instances in one working directory both pass with zero `disk I/O error` occurrences (`scripts/verify_suite_concurrency.sh`) | DONE |
| U-07 | Release-candidate report | U-01..U-06 | `docs/RELEASE_CANDIDATE_V21.md` with tested functionality, limitations, reproduction commands and a GO/CONDITIONAL GO/NO-GO decision | DONE |


## Phase D — Distributed architecture (owner-directed, added 2026-10-13)

The project owner changed the target architecture: the simulator-only web demo is
no longer the intended final product. The target is a central C++20 hub in Docker
plus real old-Android-phone acquisition nodes, with genuine Alexa integration only
if an officially supported runtime exists.

**v21 remains the tested fallback for the hackathon.** These phases are additive:
they reuse the consent gate, observation/event model, detection + fusion, alert
policy, SQLite persistence and dashboard, and introduce **no second event engine**.
Work happens on the dedicated branch `feature/distributed-homeguardian`.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| D-01 | Dockerize the existing C++ hub | v21 | Multi-stage build; no Node/Python in the image; non-root; no privileged container; no device passthrough; no published port (loopback-only server); health check; persistent SQLite volume; explicit config/secret mounts; clean SIGTERM; resource limits; documented restart behaviour; one container starts the real core, keeps state across container replacement, and passes the existing suite inside the container toolchain | DONE |
| D-02 | Android acquisition node | D-01 | On a test phone: authorized real frames + PCM samples, correctly timestamped, bounded, accounted for; permission denial/withdrawal stops delivery; else BLOCKED and reported as such | **SOFTWARE DONE / PHYSICAL BLOCKED** |
| D-03 | Secure device transport | D-02 | Registration/pairing, per-device identity + auth, encrypted LAN transport, per-device consent, versioning, sequence numbers/timestamps, heartbeat, bounded queues, backpressure/drop, replay + malformed protection, reconnect policy, no unauthorized forwarding | **DONE (end-to-end with a mock device; physical device BLOCKED)** |
| D-04 | Multiple nodes | D-03 | Device registry, per-device media/consent, independent streams, device-scoped source ids, clock-skew handling, bounded buffering, reconnect isolation, revocation, multi-source correlation; losing one device does not affect another | TODO |
| D-05 | Real media-derived observations | D-02, D-04 | Modest explainable features (frame-change/motion energy, audio RMS, availability, connect/disconnect, bounded sensors) computed from actually acquired data, flowing through the existing pipeline; capture-off/permission/consent/disconnect all fail closed | TODO |
| D-06 | Genuine Alexa integration | D-01 (external gate) | Official runtime or official skill route, verified against primary Amazon sources. **Official Docker runtime: BLOCKED** (no official Alexa/Alexa+ container image; AVS tools withdrawn; AVS Device SDK archived 2024-01-16; Alexa+ partner-gated). Skill route requires owner approval | BLOCKED (external) |

## Phase V — D-01 container verification tooling (2026-10-13)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| V-02 | `scripts/verify_android_node.sh` | D-02 | 18 host-side checks: native library builds for armeabi-v7a and exports all JNI entry points; APK builds/aligned/verifies; manifest declares exactly CAMERA + RECORD_AUDIO, both features optional, no FOREGROUND_SERVICE; dex has no invokedynamic (API 27 verifier); `[d02]` contracts and the full host suite pass; reports APK/library sizes and the attached-device count | DONE |
| V-01 | `scripts/verify_docker_hub.sh` | D-01 | Starts the real container and asserts health, real-core responses, persisted alerts, survival across container replacement, clean SIGTERM exit, no published ports / non-root / caps dropped / no docker.sock / loopback-only listener, WAL header in the volume, and reports image size + RSS + CPU + threads | DONE |


## Phase W — Skills validation and detection integration (added 2026-10-14, D-02x)

Owner-directed: inspect three installed skills (`animal-calamity-precursor-detection`,
`home-security-sensor-nodes`, and a requested `Home Assistant hub`), validate what they claim,
and implement only the feasible, testable, honest parts.

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| W-01 | Skill discovery + compatibility matrix | - | Every skill located by exact path or reported missing with its exact identifier; matrix covers provenance, functions, detection methods, overlap, dependencies, privacy, reliability and an adopt/adapt/defer/reject decision | **DONE** (`docs/SKILLS_INTEGRATION_VALIDATION.md` §1-2; the third skill does not exist and is reported as such) |
| W-02 | Bounded precursor detection layer | W-01 | Activity baseline, temporal deviation (REUSING the existing `AnomalyMonitor`), audio activity, motion change, multi-source correlation and an environmental correlation adapter - all pure C++20, bounded, deterministic, host-tested | **DONE** (`src/core/{BoundedFeatures,ActivityDetectors,DempsterShafer,PrecursorDetection,EnvironmentalCorrelation}.*`) |
| W-03 | Evidence grading (A/B/C) and per-method records | W-01 | Every candidate method classified by evidence quality with hardware floor, window, confounders, FP risks, dataset availability, resource cost and privacy implications recorded; folklore kept as hypothesis only | **DONE** (`SKILLS_INTEGRATION_VALIDATION.md` §3) |
| W-04 | Security sensor observation model + adapters | W-01 | Device id, source type, acquisition + ingestion timestamps, real/synthetic provenance, confidence/quality, consent/authorization context and a bounded validated payload for every observation; no fabricated telemetry | **DONE** (`src/core/SecurityObservation.*` incl. availability tracking and de-duplication) |
| W-05 | Home Assistant integration adapter | W-01 | All ten required behaviours proven: address config, secure credential storage, entity discovery, explicit allowlist, authorized conversion to validated observations, liveness tracking, reconnect without fabricated events, duplicate suppression, provenance preservation, disabled by default | **DONE** (`src/core/HomeAssistantAdapter.*` + `src/core/HttpClient.*`; read-only, plaintext-gated, no actuation) |
| W-06 | Pipeline integration without a second alert engine | W-04, W-05 | New observations convert to the standard `Event`, flow through the existing `Pipeline`/rules/`AlertPolicy`; new detector ids ADVISORY by default so historical scenarios are byte-identical; opt-in via the existing explicit allowlist | **DONE** (`tests/core/test_observation_integration.cpp`) |
| W-07 | Conservative severity + uncertainty model | W-06 | info = verified event; warning = validated abnormality (tamper, loss of a sensor); **critical unreachable** from any new path; `synthetic-critical-impact` stays synthetic; no species/intruder/fall/calamity claim anywhere | **DONE** (`SecurityObservation::suggested_severity` matrix test + fusion mapping + `[integration][skills]`) |
| W-08 | Verification | W-02..W-07 | New suites pass; full host suite passes; ctest 100%; fresh out-of-source Release builds all three targets; ASan/UBSan clean; existing scenario behaviour unchanged | **DONE** (413 cases / 3,984 assertions; see STATUS) |
| W-09 | Deferred work recorded with unblock conditions | W-01 | YAMNet/embeddings, YOLO, optical flow, MediaPipe, Frigate, MQTT, WebSocket events and Alexa each recorded with the exact condition that would unblock them | **DONE** (`SKILL_VALIDATION_REPORT.md` section 11) |
| W-10 | Per-skill documentation set | W-02..W-07 | One document per skill carrying adopted/adapted/rejected + why, implementation files, tests, measured performance, missing hardware and remaining validation requirements | **DONE** (`docs/SKILL_VALIDATION_REPORT.md`, `ANIMAL_ACTIVITY_DETECTION.md`, `HOME_SECURITY_SENSOR_INTEGRATION.md`, `HOME_ASSISTANT_INTEGRATION.md`) |
| W-11 | Dashboard status surface | W-04, W-05 | Read-only `/api/integrations` route whose every value is derived from real state (adapter validation, real empty registry, closed label vocabulary), rendered with the three provenance labels and a per-label legend; empty surfaces state why they are empty; no calamity/identity claim in the payload | **DONE** (`src/bridge/NativeHttp.h`, `src/bridge/hg_alexa_bridge.cpp`, `src/web/index.html`, `scripts/verify_integration_dashboard.sh` 31+5 checks) |
| W-12 | Section-J gap coverage | W-02..W-07 | Multi-device simultaneity, missing observation, unauthorized/withdrawn source, upstream authentication failure, resource bounds under sustained input | **DONE** (`tests/core/test_skill_gaps.cpp`, `[gaps]` 4 cases / 84 assertions) |
| W-13 | Missing-observation guard | W-02 | A stale finding degrades to `insufficient_evidence` instead of being repeated as current | **DONE** (`PrecursorSuite::fusion(now)` with `stale_after`, default 300 s) |
| W-14 | Fail-closed hardening found by W-11 | W-11 | A malformed credential table must refuse cleanly (non-zero exit, bounded message, no core dump, no serving) | **DONE** (bridge credential load guarded; adversarial case in `verify_integration_dashboard.sh`) |

### Boundary — kept explicit

No model weights were downloaded, no ML runtime was added, no species classifier exists, and no
life-safety prediction is produced. Species, seismic, infrasound and ultrasonic channels are
recorded as hardware- or data-blocked rather than approximated.


## Phase X — D-03 secure device transport (added 2026-10-14, branch `feature/secure-device-transport`)

| ID | Task | Dependencies | Acceptance Criteria | Status |
|----|------|-------------|---------------------|--------|
| X-01 | Protocol + credential model | D-02 | Versioned framed compact-observation protocol; private CA with one certificate per device; identity derived from the certificate, never the payload; provenance derived from (authenticated peer) x (paired) x (consent) x (capture) x (live), with an unsubstantiated `real` claim recorded synthetic and flagged | **DONE** (`src/net/DeviceProtocol.*`, `tools/provision_ca.sh`) |
| X-02 | Mutual TLS transport | X-01 | OpenSSL, TLS 1.2 minimum, client-certificate verification against the private CA, hard frame cap validated before allocation, socket timeouts, no custom cryptography, verification never disabled | **DONE** (`src/net/TlsTransport.*`) |
| X-03 | Bounded device registry | X-01 | Per device: id, display name, paired, credential binding, revoked, type, capabilities, camera/mic, last heartbeat, last accepted sequence, last authorized observation timestamp, online state, per-device consent; bounded; strict persistence that never restores capture or liveness | **DONE** (`src/net/DeviceRegistry.*`) |
| X-04 | Server-side accept path | X-03 | Authenticated before storage; unpaired/revoked/identity-mismatch/consent/capture/allowlist/timestamp/sequence/duplicate/queue refusals each with a stable reason; the sequence watermark commits only after every check passes | **DONE** (`src/net/ObservationIngest.*`) |
| X-05 | Separate network-facing ingress | X-02, X-04 | One protocol only, no dashboard/commands/SQLite on the device port; bounded connection queue, worker pool and idle timeouts; clean SIGTERM; immediate revocation via a file re-read per connection | **DONE** (`src/net/DeviceIngressServer.*`, `src/main_ingress.cpp`) |
| X-06 | D-02x detectors in the shipped hub runtime | X-05 | Per-device activity/anomaly/liveness detection fed from accepted observations, into the SAME pipeline; scalar-appropriate components only; advisory by default; no path to critical | **DONE** (`src/net/IngestDetectors.*`) - the D-02x linking limitation is closed |
| X-07 | Docker deployment of the boundary | X-05 | Second service publishing exactly one port; hub dashboard stays unpublished; non-root, caps dropped, no-new-privileges, no docker.sock, read-only certificate mounts, own volume, resource ceilings | **DONE** (`deploy/docker/`) |
| X-08 | Acceptance tooling | X-05 | Real-TLS end-to-end with a mock device client, including adversarial modes; runs inside the container where OpenSSL exists | **DONE** (`src/main_device_client.cpp`, `scripts/verify_device_transport.sh`, 21/21) |
| X-10 | Footprint measurement | X-05 | Idle/peak RAM, CPU, threads, latency, throughput, queue bounds measured rather than estimated; hardware-dependent numbers explicitly not claimed | **DONE** (`scripts/measure_device_transport.sh`, results in `docs/DISTRIBUTED_DEPLOYMENT.md`) |
| X-09 | Android transport | X-01 | Bounded queue + framed mutual TLS in the APK, compact observations only, no credential in a payload or log | **PARTIAL** - implemented and compile-verified; no phone attached, so physical delivery is BLOCKED |

## Phase X — X-11 D-03.1 security and reliability hardening (complete)

- X-11.1 Server identity verification (Android SAN + endpoint identification; OpenSSL
  hostname/IP verification; SAN-aware provisioning). Done.
- X-11.2 Credential isolation: offline CA tree, runtime tree, device-key tree. Done.
- X-11.3 Delivery-time authorization refresh with fail-closed malformed state. Done.
- X-11.4 Durable replay protection across restarts, plus the resume point that keeps a
  legitimate application restart working. Done.
- X-11.5 Android acknowledgment matching, refusal classification, queue flush and
  lifecycle/generation handling. Done (compile-verified; device acceptance blocked).
- X-11.6 Explicit `processing_failed` outcome and pre-delivery persistence ordering. Done.
- X-11.7 Test hygiene found by the battery: per-process fixtures (concurrency), guarded
  traps, container-model restart/revocation checks. Done.

Next: X-12 (physical device acceptance, requires a connected phone and explicit operator
permission). D-04 multi-phone expansion and ML remain out of scope until D-03.1 passes
review.

## Phase X — X-12 D-03.2 replay and processing consistency (complete)

- X-12.1 Two-phase durable record (begin/complete/abandon) so the watermark cannot advance
  ahead of the work. Done.
- X-12.2 Idempotent answer for an unacknowledged repeat of the last completed observation.
  Done.
- X-12.3 Real fsync-before-rename plus directory fsync, with every failure reported and
  in-memory rollback. Done.
- X-12.4 Mandatory replay protection; loopback-only isolated-test opt-out; TLS-free policy
  unit so the host suite tests it. Done.
- X-12.5 Fault injection at every boundary (validation, begin, sink, complete, repeat,
  concurrency, unwritable store, dead-process marker). Done.
- X-12.6 Android: `duplicate_observation` reclassified as retryable, matching the hub's
  in-flight-timeout semantics. Done (compile-verified).

Next: X-13 (physical device acceptance, requires a connected phone and explicit operator
permission). D-04 remains out of scope until D-03.2 passes review.
