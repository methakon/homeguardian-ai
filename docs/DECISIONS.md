# HomeGuardian AI — Architecture Decisions

## ADR-001: C++20 as the Implementation Language

**Status:** Accepted
**Date:** 2026-10-09

### Context
The project requires a native, resource-conscious application for home
intelligence. The machine has 40 logical CPUs and 15 GB RAM.

### Decision
Use C++20 as the baseline language standard.

### Rationale
- Native performance for real-time sensor processing
- Fine-grained control over memory and resource usage
- Mature ecosystem for multimedia, networking, and ML inference
- No runtime overhead compared to managed languages
- RAII and modern C++ features enable safe, deterministic resource management

### Consequences
- Steeper learning curve for contributors unfamiliar with modern C++
- Build times longer than interpreted languages
- Memory safety requires discipline (mitigated by RAII and smart pointers)

## ADR-002: Single Native Core Process

**Status:** Accepted
**Date:** 2026-10-09

### Context
The system could be designed as a monolith or as multiple microservices.

### Decision
Start with a single native C++ process. Add services only when a
demonstrated requirement justifies the cost.

### Rationale
- Simpler deployment and debugging
- Lower resource overhead on the development machine
- Easier to maintain data consistency
- The current scope does not require independent scaling

### Consequences
- Single point of failure (mitigated by process supervision)
- Must be careful with resource isolation between components
- Adding services later requires refactoring interfaces

## ADR-003: Local-First Processing

**Status:** Accepted
**Date:** 2026-10-09

### Context
Home monitoring involves sensitive video, audio, and behavioral data.

### Decision
All data processing happens on the local network. No cloud dependencies
for core operation.

### Rationale
- Privacy — sensitive data never leaves the home
- Latency — no network round-trips for real-time processing
- Reliability — works without internet connectivity
- Cost — no ongoing cloud service fees

### Consequences
- Limited compute compared to cloud (mitigated by efficient C++ and
  optional ONNX Runtime local inference)
- User is responsible for backups and maintenance

## ADR-004: Interface-Based Architecture

**Status:** Accepted
**Date:** 2026-10-09

### Context
The system integrates multiple sensors, AI providers, and output channels.

### Decision
All external dependencies sit behind well-defined interfaces.

### Rationale
- Enables swapping implementations (e.g., different camera sources)
- Simplifies testing via mock implementations
- Supports future expansion without core changes
- Enforces dependency inversion principle

### Consequences
- Initial development slightly slower due to interface design
- Must resist over-abstraction for simple cases

## ADR-005: CMake as Build System

**Status:** Accepted
**Date:** 2026-10-09

### Context
Need a build system that supports C++20, is cross-platform, and integrates
with common C++ libraries.

### Decision
Use CMake 3.28.3 with Unix Makefiles generator.

### Rationale
- Industry standard for C++ projects
- Excellent IDE and tooling support
- FetchContent for dependency management
- No Ninja dependency (use `make`)

### Consequences
- CMake syntax can be verbose
- Build times longer than Ninja (mitigated by `-j4` flag)

## ADR-006: Implementation History — File-Scoped Integration

**Status:** Accepted
**Date:** 2026-10-09 (updated 2026-10-10)

### Context
The project used different implementation approaches across phases. Phase E
included authorized parallel, independent work on core model headers.

### Decision
- Phase B code was reviewed and cleaned up (commit `e9ebfa3`).
- Phase E tasks were file-scoped (core model headers), followed by integration,
  builds, tests, and review. Parallel work used disjoint files to avoid conflicts.
- Documentation must accurately distinguish implementation history and verification evidence.

### Rationale
- Parallel work requires explicit authorization and file ownership.
- The recorded integration process included compilation, tests, and review before commit.

### Consequences
- Documentation must not invent authorship or verification claims.
- Future phases follow the implementation process authorized by the developer.

## ADR-007: Deferred License Selection

**Status:** Pending
**Date:** 2026-10-09

### Context
The project needs a license before public release.

### Decision
License selection deferred until the project approaches release.

### Rationale
- License choice depends on intended use (personal, open source, commercial)
- No code is public yet, so no immediate need
- Changing license later is easier than before release

### Consequences
- Cannot accept external contributions until license is selected
- Repository should not be published without a license

## D-native-http: one native C++ process for the demo (2026-10-13, v14)

**Context.** The demo ran as two processes: a Node.js HTTP server
(`src/bridge/hg_demo_server.js`) that proxied to the C++ bridge over NDJSON,
spawning a fresh bridge child per request. Goal: eliminate Node from the final
demo/runtime, keep one process, minimise RAM/CPU/footprint.

**Decision.** Add a native HTTP mode to the existing bridge binary
(`hg_alexa_bridge --http …`) that serves the dashboard and the JSON API over
loopback, reusing the SAME `Application`, `AlexaContract`, verifier, and
`PersistenceAudit` as every other path. The dashboard HTML is **embedded** at
build time (CMake `configure_file`) so the binary is self-contained.

**Rationale.**
- No duplicated backend logic (the scenario/status code is shared verbatim).
- One process, no Node runtime dependency, no per-request child spawn.
- The existing NDJSON stdio mode is preserved unchanged for the Node tests and
  process-boundary proofs.
- Embedding the dashboard removes a runtime web-server file dependency and any
  request-time filesystem path (no traversal surface).

**Consequences / semantics preserved.**
- `--http` mode is loopback-only; static assets and status queries are GET-only; `run-scenario` is strictly POST requiring session authentication and single-use CSRF tokens (updated in HG-SEC-01); capture/consent/config stay read-only.
- **Per-run reset**: because one process now serves repeated scenario runs, the
  scenario resets per-run state (counters, alert policy, pipeline history,
  runtime/simulator latches) so each run behaves like the legacy fresh-process
  run. This is a deliberate demo-control-plane decision (simulator-only).
- The legacy Node demo server is **retained as a historical reference**
  (`HG_DEMO_LEGACY_NODE=1` in `demo.sh`) for A/B footprint comparison only; it is
  no longer the default and is not required to run the demo.

**Not adopted:** an external HTTP framework/library. The surface is a bounded
loopback demo subset; a reviewed server stack + TLS is required only for
non-local/production use (see `docs/SECURITY_READINESS_V8.md`).

## D-license-apache: Apache License 2.0 (2026-10-13, v16)

**Decision.** License HomeGuardian AI's original source under the **Apache
License 2.0** (owner-approved), replacing the previous MIT `LICENSE`.

**Rationale.** Apache-2.0 is permissive like MIT but adds an express patent grant
and a NOTICE/attribution convention — preferable for a public hackathon project
that may see reuse or corporate evaluation.

**Implementation.** Root `LICENSE` = the unmodified official Apache-2.0 text
(sha256 verified against apache.org). Root `NOTICE` = copyright + third-party
attribution. README updated. Bundled/fetched third-party components (SQLite —
public domain; Catch2 — BSL-1.0; spdlog, nlohmann/json — MIT; ask-sdk — Apache
2.0) are **not** relicensed and keep their own notices.

**Ownership.** `Copyright 2026 Swarna Sekhar Dhar` (repo owner; 45/46 commits;
prior MIT line). One externally-authored commit is flagged for owner confirmation.

## D-sqlite-synchronous-full-retained: durability over latency (2026-10-13, v16)

**Decision.** KEEP `PRAGMA synchronous=FULL` on the audit SQLite connection. The
~93 ms/commit fsync cost measured in v15 (`docs/PERF_SCENARIO_LATENCY_V15.md`) is
accepted as the price of durable alert persistence on this host.

**Rationale.** Weakening synchronization (`NORMAL`) or batching away required
audit commits would trade durability for benchmark numbers — explicitly out of
scope. Correctness and durability take priority over demo latency.

**Consequence.** The native `/api/run-scenario` latency on this host remains
~50–180 ms (dominated by one durable commit). No code change; the v15
investigation and the default-off `HG_PROFILE` instrumentation remain as
documentation/tooling.

## D-demo-scenarios: deterministic scenario catalogue + real-alert persistence (2026-10-13, v17)

**Decision.** The demo is a small, deterministic SCENARIO CATALOGUE
(`src/core/DemoScenarios.{h,cpp}`) executed by a core runner
(`src/core/DemoScenarioRunner.{h,cpp}`) that drives the REAL Application
pipeline. Scenario definitions are data (cycles, synthetic consent, detection
energies); they contain NO pre-baked alert outcomes.

**Why.** The previous demo produced one hardcoded alert
(`"synthetic simulator presence notice"` written directly to SQLite whenever an
alert had been delivered) and the dashboard hardcoded the same text. That
undermined the project's central claim (detection -> alert -> explanation) and
made the "why" unverifiable.

**Consequences.**
- `Application` exposes a passive `set_alert_sink()` observer; the runner captures
  the alerts the pipeline actually DELIVERED and `persist_run_alerts()` writes
  exactly those. No fabricated alert is ever inserted.
- `SimulatorRuntime::reset_consent_state()` clears the synthetic consent repo at
  the start of each scenario, so scenarios are deterministic and independent of
  execution order in the long-lived demo process.
- The Alexa contract stays AGGREGATE-ONLY (its documented privacy property). The
  dashboard gets per-alert detail from a new loopback-only `/api/alerts` served
  from the durable store, not from the contract.
- `/api/run-scenario` gained a strictly allowlisted `scenario=` token; `/api/scenarios`
  lists the catalogue. Unknown scenario ids are rejected.

**Alternatives rejected.** (a) Extending the Alexa contract to return per-alert
detail — rejected: it would weaken that boundary's documented aggregate-only
guarantee. (b) Keeping the hardcoded alert — rejected: unverifiable and
misleading. (c) A separate scenario engine — rejected: the mission forbids a
second event engine.

## D-anomaly-advisory: statistical anomaly evidence is advisory by default (2026-10-13, L-12)

**Decision.** `AnomalyMonitor` feeds its results through the existing pipeline,
but `DetectionFusionRule` accepts only allowlisted detector ids as fusion
evidence; the default allowlist is the activity detector (`activity-energy`), so
statistical anomaly evidence (`activity-anomaly`) is reported, audited and
persisted **without** voting on an alert. Adding `activity-anomaly` to the
allowlist is the explicit, tested way to let it influence alerts.

**Why.** (a) The framework's votes are about *presence*; an anomaly detector's
verdict is about *deviation from a learned baseline* — different targets, so
wholesale fusion conflates them. (b) Measured while writing the causal test: the
detector's own within-baseline samples emit `not_detected`, which outnumber the
flagged samples on a realistic run, so fusing all of them returns `inconclusive`
and a vote-shaped fusion of an anomaly detector systematically under-reports.
Making the influence an explicit configuration keeps the L-11 alert semantics and
measured results exactly intact while still providing a real, tested path for
anomaly-driven alerts.

**Consequences.** The dashboard shows the actual relationship
(`fused_into_alert` / `relationship`) rather than implying the anomaly caused the
alert. A deviation never becomes a fall/emergency/danger claim, and it never
raises an alert under the default configuration.


## D-impact-pattern: a deterministic synthetic impact classifier, not fall detection (2026-10-13, H-01s)

### Context

H-01s must demonstrate severity prioritisation with a plausible high-priority
household event. Nothing in the build can honestly classify an impact: there is
no camera, no pose estimation, no model and no biometric data, and H-01 (real
pose-based fall detection) is explicitly out of scope. Roadmap guidance for this
milestone forbids presenting an ordinary activity-energy reading as "proof of an
impact" without a meaningful mapping.

### Decision

Introduce `SyntheticImpactClassifier` (`src/core/ImpactClassifier.{h,cpp}`), a
small deterministic classifier over a bounded synthetic impact-intensity feature
in [0,1], with documented two-phase semantics: a transient (>= 0.80) is NOT a
confirmation on its own (`inconclusive`, "awaiting supporting stillness"); the
pattern is confirmed only when a following sample <= 0.25 arrives within 4
samples. Anything else is `not_detected`; an expired transient reports why.

`SyntheticImpactRule` reads only those observations and fires a critical alert
only for a confirmed pattern with >= 2 supporting observations.

### Rationale

- The mapping from feature to classification is explicit, deterministic and
  testable (both thresholds and the window are asserted), so no ordinary activity
  reading is mislabelled as an impact.
- Ordering is honest evidence: a single ambiguous signal cannot become a
  definitive critical classification.
- It reuses the established `Observation`/`Event`/rule/policy/persistence path.

### Consequences

- Every positive result carries its own evidence (transient value, stillness
  value, sample distance, supporting count) and every note states plainly that
  this is a synthetic demonstration, not fall detection, and that no emergency
  service is contacted.
- H-01 (real pose-based fall detection) remains NOT implemented; H-01s says so in
  the alert message, the API payload and the dashboard.

## D-severity-prioritisation: highest severity wins in the pipeline (2026-10-13, H-01s)

### Context

Before H-01s `Pipeline` returned the FIRST rule's alert and stopped. With more
than one rule registered, a critical condition could be hidden behind an
unrelated info/warning alert that happened to match first.

### Decision

`Pipeline` evaluates EVERY registered rule over the same window and returns the
alert with the highest severity (`info < warning < critical`); ties keep the
earliest rule. `Application` registers `DetectionFusionRule` (activity) and
`SyntheticImpactRule` (impact); the impact rule stays silent unless impact
evidence exists, so other scenarios are unaffected.

### Rationale

- Severity prioritisation must be genuine: the critical classification comes from
  the backend rules and their evidence, not from presentation.
- Independent rules must not suppress each other; they keep their own policy keys
  (rule + severity), so a critical alert and an unrelated info alert are both
  delivered.

### Consequences

- Single-rule behaviour is bit-identical to the previous implementation.
- Duplicate suppression, cooldown and re-arm semantics are unchanged (they are
  keyed on rule + severity and remain per process).
- Covered by `[h01s]` tests: prioritisation, duplicate suppression, rule
  independence and re-arm.


## D-handler-owned-status: the handler reports the HTTP status (2026-10-13, v21)

### Context

The native HTTP transport hard-coded `response(200, ...)` around every handler
result, so a well-formed request naming an unsupported value (an unknown scenario
id) answered `200 OK` with `{"ok":false,...}`. That misleads any client that reads
the status code rather than the body, and it contradicts the transport's own
strict validation (which answers 400/403/404/405/413 correctly).

### Decision

`Handler` is `std::function<std::string(const HandlerRequest&, int& status_out)>`.
The handler returns the JSON body and reports the status for the outcome it
decided; the transport continues to own framing, header and query validation. An
unknown scenario answers `400 Bad Request` with the unchanged JSON error body. A
handler cannot override a transport-level rejection (the handler never runs).

### Rationale

- The layer that knows the business outcome should own the status; a hard-coded
  200 makes every failure look like success.
- The body shape is unchanged, so the dashboard (which keys on `scenario.ok`) and
  any existing client keep working — no unnecessary breakage.

### Consequences

- New `[v21]` tests assert the status line, the JSON body, and the
  non-override property.
- Documented behaviour change for direct API users; recorded in DEMO_WEB,
  ARCHITECTURE and SECURITY_READINESS_V8.

## D-degraded-startup: diagnostics never abort, persistence degrades (2026-10-13, v21)

### Context

With an unwritable working directory the shipped binaries died with SIGABRT
(exit 134) because the logger's file sink threw out of `main()`. Separately, the
bridge's own durable alert store never applied the configured retention window.

### Decision

- Logging is best-effort: if the log file cannot be created, log to the console
  only, report the reason once, and continue. Registration failures are ignored.
- The bridge applies `alert_retention_days` to its own audit DB (startup plus a
  bounded periodic prune, only after a scenario write); `0` keeps records forever.
- An unusable audit database keeps the process serving and reports
  `{"error":"unavailable","ok":false}`; no persistence is claimed.

### Rationale

Consent enforcement, capture gating and the privacy audit must keep running even
on a read-only or full filesystem; a diagnostic facility must never be able to
take down the safety path. Retention is already a configured, tested policy — it
must be applied where the data is written.

### Consequences

- New `[logger]` regression plus `scripts/verify_retention.sh`.
- Durability (WAL + `synchronous=FULL`) and the alert record shape are unchanged.


## D-distributed-target: hub in Docker + Android nodes, v21 preserved (2026-10-13, D-01)

### Context

The project owner changed the target architecture: the simulator-only web demo is
no longer the intended final product. The target is a central C++20 hub in Docker
plus real Android phones as camera/microphone/speaker/sensor nodes, with genuine
Alexa integration if an officially supported runtime exists. The hackathon
deadline (feature freeze 19 Oct 2026) does not fit the whole distributed
architecture.

### Decision

Work proceeds on a dedicated branch, `feature/distributed-homeguardian`, created
from the verified v21 commit. The v21 simulator product stays untouched and
remains the tested submission fallback (it is *not* deleted or destabilised).
The D-series reuses the existing consent gate, observation/event model,
detection + fusion, alert policy, SQLite persistence and dashboard; **no second
event engine** is introduced and no node performs detection or alerting.

### Rationale

- The owner's architecture decision is explicit, so it is implemented — but the
  deadline makes a verified fallback essential.
- Reusing the pipeline keeps one set of consent/privacy/durability guarantees to
  audit rather than two.

### Consequences

- Documentation states an honest operating mode per deployment
  (`SIMULATION MODE`, `REAL LOCAL CAPTURE ACTIVE`, `AMAZON ALEXA — Not Connected`)
  and never claims real capture or an Alexa connection that is not verified.
- Final submission media stays pending until the owner picks which verified
  implementation to submit.

## D-hub-container-base: debian:trixie-slim in both stages (2026-10-13, D-01)

### Context

The first container build failed at configure:
`CMake 3.28.3 or higher is required. You are running version 3.25.1` — Debian
bookworm ships CMake 3.25.1, while the project's `CMakeLists.txt` requires
3.28.3 (the host has 3.28.3, which is why local builds work).

### Decision

Use `debian:trixie-slim` (CMake 3.31.6, g++ 14, glibc 2.41) for **both** the
builder and the runtime stage. Do not relax the project's CMake requirement, and
do not build on a newer glibc than the runtime provides.

### Rationale

- Relaxing `cmake_minimum_required` would change a working project contract to
  suit the container, which the mission forbids.
- A binary linked against a newer glibc than the runtime image has cannot start
  (`GLIBC_2.3x not found`), so builder and runtime must be the same release.

### Consequences

- The image base is documented in the Dockerfile with the reason, so a future
  change of base is a conscious decision rather than a silent regression.
- The `test` stage proves the suite passes with the container toolchain.

## D-hub-no-published-ports: the loopback-only server is not exposed (2026-10-13, D-01)

### Context

The hub's handwritten HTTP server was designed and security-reviewed for
loopback use: reads are unauthenticated by design for local diagnostics while state changes require authenticated session and CSRF token, relying on `Host`/`Origin`
validation, and has no TLS. The container could publish a port in one line.

### Decision

The hub container publishes **no** ports. Its listener stays `127.0.0.1` inside
the container. An opt-in override
(`docker-compose.loopback.yml`) may bind the **host** side to `127.0.0.1` for
local viewing only. A genuinely network-facing boundary is milestone D-03's
authenticated, encrypted transport with pairing and revocation, reviewed on its
own.

### Rationale

Changing `127.0.0.1` to `0.0.0.0` (or publishing the port) would expose an
unauthenticated, unencrypted surface to the LAN — a security downgrade, not a
deployment step.

### Consequences

- `scripts/verify_docker_hub.sh` asserts no published ports and that the
  listening socket is `0100007F:1F90` (127.0.0.1) and never `00000000`.
- Anyone needing LAN access must wait for D-03.

## D-alexa-official-runtime-blocked: no genuine Alexa/Alexa+ Docker runtime (2026-10-13, D-06)

### Context

The mission asks for genuine Alexa/Alexa+ running inside Docker, but explicitly
makes official support a feasibility gate rather than permission to substitute a
simulator or an unofficial image.

### Decision

Report `OFFICIAL ALEXA DOCKER RUNTIME: BLOCKED`, with primary-source evidence, and
implement nothing Alexa-branded in the meantime. Evaluate the official skill
route as an alternative **for owner approval** without silently switching the
requested architecture.

### Rationale

Evidence (all from Amazon's own pages; see `docs/DISTRIBUTED_DEPLOYMENT.md` §6):
AVS developer tools are "no longer generally available for Alexa Built-in"; the
AVS Device SDK repository was archived 2024-01-16; the AVS documentation set and
console no longer resolve; Alexa+ Category SDK / MCP Toolkit are "available to
select partners only"; there is no official Amazon Alexa image on Docker Hub.
Building an imitation and calling it genuine would be a false claim.

### Consequences

- The dashboard must show `AMAZON ALEXA — Not Connected` unless a verified
  authenticated integration exists.
- Unblocking requires Amazon partner access (Alexa+ for Builders, Category SDK /
  MCP Toolkit, a supported marketplace, AWS hosting) — an owner-level action.
- The existing `AlexaContract` + skill-handler prototypes are the reuse target if
  the owner approves the official skill route.


## ADR-008: Adopt the skill's bounded detection methods; reject species and calamity claims

**Status:** Accepted
**Date:** 2026-10-14

### Context

Three skills were supplied for inspection. `animal-calamity-precursor-detection` offers a full
pipeline (log-mel + YAMNet embeddings, YOLOv8, Farneback optical flow, cosine vector drift,
Dempster-Shafer fusion, MQTT/Alexa delivery) and its own correction ledger already marks parts of
its source document as debunked or overstated. `home-security-sensor-nodes` offers phones as
sensor nodes with hub-side inference (Frigate/YOLO/MediaPipe). The project is C++20 with no ML
runtime, no OpenCV, no model weights, and a standing rule against downloading weights without a
licensing and resource review.

### Decision

Adopt the methods that are pure, bounded, deterministic and testable now: bounded audio/motion
features, an activity baseline, cosine vector drift, Dempster-Shafer fusion with conflict mass K,
an environmental correlation adapter, and the skills' evidence discipline. Defer every
model-dependent method. Reject species identification and every calamity-prediction channel as an
alerting path, and make the rejection structural by removing the calamity hypothesis from the DST
frame rather than merely documenting it.

### Rationale

- The adopted methods need no dataset, no weights and no runtime beyond the standard library, so
  they are implementable and verifiable today; the deferred ones are blocked on reviews the
  project forbids skipping.
- The skill's own ledger documents that ant seismology is debunked (ants react to shaking above
  M2.0), that the Haicheng case is overstated, and that gecko-call meaning is superstition. A
  system that emits a life-safety claim from those channels would be asserting something its own
  source material retracts.
- Rejected: a model-dependent path (no review), a species classifier (no dataset), and any
  disaster prediction (unrepresentable in the frame).

### Consequences

- HomeGuardian gains real, tested detection components without a single new dependency.
- The strongest faunal output is a labelled hypothesis (`possible_animal_activity`) at
  informational severity; anything more would require a dataset and a review that do not exist.
- Species, ultrasonic (rodent USV), infrasound and optical-flow channels are recorded as
  hardware- or data-blocked, so a later reader does not mistake them for oversights.

## ADR-009: Home Assistant integration is native, read-only, default-off and plaintext-gated

**Status:** Accepted
**Date:** 2026-10-14

### Context

A `Home Assistant hub` skill was requested but does not exist in this environment. The nearest
artefact is the installed Hermes plugin `homeassistant` (v2.0.1, MIT): a Python gateway for the
agent with REST tools, a WebSocket subscription and `notify.notify` delivery. HomeGuardian is a
C++20 hub in a container with no Python at runtime, and its loopback-only listener was never
designed for external traffic.

### Decision

Implement a small native C++ adapter that reads entity state over the REST API
(`GET /api/states/<entity_id>`) with an explicit entity allowlist, a token read from a mounted
0600 secret file, availability tracking, de-duplication and real-vs-synthetic provenance. It is
disabled by default, requires an explicit authorization decision before any request, is
read-only (GET-only, no service calls), and refuses an `https://` base URL because no TLS is
linked, with an additional explicit `allow_plaintext` opt-in for an `http://` endpoint.

### Rationale

- The plugin is the wrong layer (agent gateway, not a C++ hub component) and the wrong runtime
  (Python), so it is not a dependency.
- REST polling is the smallest surface that satisfies the milestone's ten required behaviours;
  WebSocket events, MQTT and any actuation are deferred or refused.
- Refusing `https://` is deliberate: silently downgrading to plaintext while sending a bearer
  token would be worse than refusing to run. An explicit opt-in keeps the operator aware of the
  exposure.
- Rejected: linking the Python plugin (runtime cost, wrong layer), actuator control (out of scope
  until separately approved), and baked-in credentials (tokens stay in a mounted secret).

### Consequences

- The integration is usable on a trusted LAN today and cannot actuate anything.
- TLS, WebSocket events and MQTT remain explicit future work; the adapter's transport seam
  (`IHomeAssistantTransport`) is where they plug in without touching the mapping, de-duplication
  or liveness logic.

## ADR-010: New detector ids are advisory by default, and critical stays unreachable

**Status:** Accepted
**Date:** 2026-10-14

### Context

The precursor and sensor observations introduce new detector ids (`motion-activity`,
`audio-activity`, `audio-activity-deviation`, `vector-drift`, `precursor-fusion`,
`environmental-correlation`, `sensor.*`). `DetectionFusionRule` votes only on an explicit
evidence allowlist, and `Severity::critical` is currently reachable only through the explicitly
synthetic `synthetic-critical-impact` scenario. The ten historical scenarios and the H-01s
scenario must keep behaving identically.

### Decision

Add no new detector id to the default evidence allowlist. Keep the new evidence advisory:
validated, counted, auditable and visible, but not a fusion vote unless an operator opts in
explicitly via `set_evidence_detector_ids`. Map every new observation to informational severity,
with tamper and loss-of-sensor as the only warnings, and give the security severity function no
branch that returns `Severity::critical`.

### Rationale

- The L-12 lesson already established that a differently-targeted detector must not be fused
  wholesale; a behaviour change hidden inside a new observation type is exactly that failure with
  extra steps.
- A single motion, contact or availability report is not sufficient evidence for a high-priority
  claim; escalation belongs to correlation, which the existing `AlertPolicy` already owns.
- Making critical unreachable in code (rather than by convention) is what keeps "camera motion is
  not a fall" true after the next change.

### Consequences

- The historical scenarios are byte-identical (asserted in `[integration][skills]`), and the
  opt-in path is covered by a test that shows allowlisted precursor evidence can alert and never
  exceeds `warning`.
- Anyone wanting the new detectors to raise alerts must do so explicitly and can see exactly
  which ids they enabled.


## ADR-011: The dashboard reports only derived state, and an empty surface must say why

**Status:** Accepted
**Date:** 2026-10-14

### Context

The brief asks the dashboard to show connected sensor devices, the Home Assistant connection,
animal-activity observations, security observations, statistical anomalies, environmental
correlations, provenance, confidence/evidence and consent state - while clearly separating live
data, synthetic test data and unvalidated hypotheses. At the time of writing, nothing is
connected: no phone (D-02 physical acceptance is blocked) and no Home Assistant instance exists.

### Decision

Add a read-only `/api/integrations` route whose every value is derived from real state rather than
written down: the Home Assistant block comes from `HomeAssistantConfig::problems()`, the device
count from a real `DeviceAvailabilityTracker` instance, and the label set from the detection
layer's closed vocabulary. Every surface that is empty carries a reason string. The three
provenance labels are rendered verbatim, and the payload contains no calamity, identity or fall
claim at all.

### Rationale

- A hardcoded status panel would drift from the implementation within one milestone; deriving it
  means the panel cannot claim what the code does not do. The reason string for Home Assistant
  being off is the adapter's own validation message, so the UI and the adapter cannot disagree.
- An unexplained zero is the most dangerous display in a monitoring product: it is
  indistinguishable from a working sensor reporting calm. "0 devices connected - no node transport
  yet (D-03)" is honest; "0" alone is not.
- Keeping the forbidden vocabulary out of the payload (rather than listing it as "never produced")
  means a client cannot render a disaster prediction even by mistake, and the verification script
  can assert its absence.

### Consequences

- The dashboard tells the truth about a system with no hardware attached, which is the current
  reality; when D-03 lands, the same route starts reporting non-zero counts with no UI change.
- The verification script (`scripts/verify_integration_dashboard.sh`) becomes the guard: it fails
  if a count becomes non-zero without a connected source, if a surface loses its reason, if the
  vocabulary drifts, or if a forbidden token appears.
- The new route also exposed a real fail-closed defect (a malformed credential table aborted the
  bridge), now fixed and asserted adversarially.


## ADR-012 — Device authentication is a separate process, and identity comes from the certificate

**Date:** 2026-10-14
**Status:** Accepted (D-03)
**Context:** D-03 required network communication between Android nodes and the hub, while D-01's
server was explicitly designed, reviewed and documented for loopback only. The obvious shortcut --
change the bind address -- would have moved the dashboard, the command channel and SQLite-backed
reads onto the Wi-Fi and called it "networking".

**Decision:**

1. The network-facing surface is a **separate executable and a separate container**, speaking exactly
   one protocol: hello/welcome, compact observation frames, heartbeat, goodbye. It serves no
   dashboard, no command channel and no SQLite. The loopback demo server keeps its bind address and
   stays unpublished in the default compose file.
2. Trust is established by a **private CA with one certificate per device**, and authorization
   requires **two independent bindings**: the chain must verify against the CA *and* the certificate
   fingerprint must equal the `credential_id` recorded for that device id. Either alone is
   insufficient, because a valid CA signature proves membership, not identity, and a recorded
   fingerprint proves nothing about who is holding the key today.
3. The device id used for authorization is taken from the **certificate CN**, never from a message
   field. The message field exists so a client can be honest about itself and the hub can *detect*
   disagreement.
4. Provenance is **derived** from authenticated peer × paired × consent × capture-enabled × live
   acquisition. A `real` claim the hub cannot substantiate is recorded synthetic and flagged.
5. The TLS implementation is **OpenSSL**, not hand-rolled: the brief asked for an established
   implementation, and a bespoke cipher/KDF is not a security boundary, it is a liability. The
   transport has no insecure mode, and tests use a real CA.
6. Revocation is a **file re-read on every connection**, so it takes effect on the next message. A
   revocation that needs a restart is still authorizing the device in the meantime.
7. Certificate material is **operator-provisioned, mounted read-only, and gitignored**. Nothing
   sensitive is baked into an image layer.

**Consequences:**

- Two containers, one image, two trust levels. The ingress adds no open port beyond 8443/tcp.
- A device cannot read household data: its port has no read surface beyond its own acknowledgements.
- The D-01 dashboard path is untouched and its 26 container checks still pass.
- OpenSSL becomes a build and runtime dependency **for the network binaries only**; the protocol,
  registry, ingest and detector layers stay OpenSSL-free and are tested everywhere.
- A phone that steals another phone's certificate still fails, and a phone whose certificate is
  revoked is refused immediately.
- Cost: certificate provisioning is an operator step with no UI yet; the Android client stores its
  PKCS#12 in app-private storage rather than in the Android Keystore (recorded as the next hardening
  step, not as done).


## ADR-013 — Capture authorization is written by the operator; revocation is reversible

**Date:** 2026-10-14
**Status:** Accepted (D-03, supersedes the stricter reading in ADR-012 on one point)

**Context:** D-03's first implementation cleared `capture_enabled` on every registry load, on the
principle that a restart must not resurrect an authorization nobody re-stated. Combined with the
accept path's rule that capture-disabled devices are refused, this made the boundary unusable: no
device could ever be accepted, and the milestone's own objective was unreachable. The same review
found that file-based revocation could only set the revoked flag, so reinstating a device required a
restart.

**Decision:**

1. **Capture authorization is honoured from the registry file**, but only when the operator wrote it
   AND the device is paired AND consent is recorded AND the device is not revoked. An absent key is
   always false. There is no inference: nothing but the operator's own written entry can set it.
2. **Runtime state is still never restored.** Online status, the sequence watermark and the
   acquisition-timestamp high-water mark are re-learned from the wire after every restart. They are
   observations about the world, not authorizations, and restoring them would assert something the
   hub cannot know.
3. **File revocation is authoritative for the ids it lists, and removing an id lifts the ban
   immediately.** Un-revoking deliberately does not restore pairing or consent: the operator must
   state the permissions again. The ban and the permissions are different decisions.
4. **Consent withdrawal requires a registry reload** (or the equivalent operator action), because the
   registry is read at startup while the revocation list is re-read per connection. The acceptance
   suite exercises withdrawal exactly that way, and the deployment documents it.

**Consequences:**

- The system is usable: an operator can authorize a device in writing, and the hub accepts it.
- Queries about "did a restart silently re-authorize a device?" answer no: only written
  authorization is honored, and every runtime observation is re-learned.
- Reinstating a revoked device is two explicit acts (un-revoke, then re-grant), which is the correct
  amount of friction for a security control.
- The registry file becomes an authorization record the operator must protect - which it already was,
  since it carries credential fingerprints.


## ADR-014 — The ingestion boundary writes the hub's alert history, and fusion evidence is opt-in

**Date:** 2026-10-14
**Status:** Accepted (D-03 finalization)

**Context:** Two gaps were found only when the ingress was run as a deployment rather than as a
binary:

1. The ingress kept its OWN SQLite audit store. Its alerts were therefore invisible to the dashboard,
   which reads `homeguardian_bridge_audit.db`. The D-03 objective says an authorized observation must
   "become available in the dashboard and persisted alert history" — with two stores, it never could.
2. Ingested observations carry `detector_id = "sensor.<source_type>"`, and the fusion rule's evidence
   allowlist defaults to `{"activity-energy"}`. So ingested evidence never voted, and no ingested
   observation could ever produce an alert. That default is a deliberate safety property (L-12), not
   an oversight — but it also meant the milestone's end-to-end path terminated at "recorded".

**Decision:**

1. **One alert history.** The ingress and the hub share the persistent volume and the same SQLite
   file. Both processes use WAL with a 5 s busy timeout (already configured in `SQLiteDatabase::open`),
   so writers are serialized and readers see a consistent snapshot. The constraint is a LOCAL
   filesystem (named volume or same-host bind mount): WAL over NFS is a corruption risk and is not
   supported. `synchronous=FULL` is unchanged in both.
2. **Fusion evidence is an explicit operator opt-in.** `evidence_detector_ids` in the ingress config
   lets the operator admit specific ingested detector ids (e.g. `sensor.motion`) as fusion evidence.
   Absent or empty keeps the conservative default. An empty list is never passed through to the rule,
   because an empty allowlist means "accept every detector" — absence of a decision must not become
   the widest possible permission.
3. **Nothing here can produce `Severity::critical`.** The severity mapping derives from fused
   confidence (info < 0.8, warning >= 0.8); critical remains reserved for the synthetic impact demo.
   Animal-activity and sensor-derived conclusions stay advisory.

**Consequences:**

- An accepted observation's alert is visible through the hub's own API and dashboard, which is what
  "end to end" has to mean.
- Two writers on one SQLite file is a supported pattern on a local filesystem, but it is also a
  deployment constraint worth stating in the docs: do not put `/data` on network storage.
- Conservative behavior is preserved by default: an operator who wants ingested motion to alert must
  say so, and the hub prints the active allowlist at startup so the decision is never invisible.

## ADR-015 — Split credential storage: offline CA, runtime tree, device keys

**Context.** The D-03 provisioning tree held the CA private signing key beside the
material the ingress container mounts, and the deployment documentation suggested
`chmod 644 server.key` as the way to let uid 10001 read it. Either a container compromise
or a world-readable key would reach material that can mint new devices.

**Decision.** Three trees with different trust levels: the CA signing key stays in an
offline directory that no container mounts; the runtime directory holds only what the
ingress needs; device private keys live with the device, outside both. Ownership (not
world-readability) is the mechanism for uid 10001.

**Consequences.** A compromise of the ingress cannot mint devices. Provisioning gains two
path options and must be idempotent; deployment documentation must describe signing,
deployment, rotation and revocation separately.

## ADR-016 — Durable sequence watermark, and the resume point that goes with it

**Context.** D-03 loaded the sequence watermark from the registry at startup, so
restarting the hub reopened the replay window. A purely durable counter, however, would
refuse every observation from a legitimately restarted application that begins again at
sequence 1.

**Decision.** Persist a per-device counter (never a timestamp) atomically, bounded, with
fail-closed loading, written BEFORE delivery. The welcome carries the device's own
watermark as `last_sequence`, and a client resumes above it. Identity remains the
certificate-derived device id.

**Consequences.** Restart is not a replay primitive and an app restart still works.
Reconnecting cannot reset protection (the watermark is durable and the resume point is
the hub's own state). Sequence rollover is handled by the counter type; clock changes are
irrelevant because no timestamp is involved.

## ADR-017 — Authorization at delivery time; `processing_failed` as a distinct outcome

**Context.** Authorization was evaluated at connection setup and `accepted` was returned
before downstream processing finished, so a refusal could be reported as an acceptance
and a stalled connection could keep a revoked device feeding detectors.

**Decision.** Re-check authorization before every accepted observation (rate-limited,
forced on a deadline, failing closed on malformed state). Define `accepted` as
authenticated + authorized + bounded + validated + handed to the sink, and introduce
`RejectReason::processing_failed` for a downstream failure. On failure the observation is
dropped and the watermark does not advance.

**Consequences.** A refusal is never reported as an acceptance; a device that is revoked,
unpaired or denied consent stops contributing on its next observation without a restart;
devices are unaffected by each other's authorization changes.

## ADR-018 — Two-phase durable replay record, with an at-least-once contract

**Context.** D-03.1 persisted the sequence watermark before delivering to the sink. A sink
failure therefore answered `processing_failed` while the watermark had already advanced, so
the device's retry was refused as a replay and the observation was silently lost. The
alternative - simply recording after delivery - reopens the window the watermark exists to
close: a crash between processing and recording lets the same observation be processed
again, and an attacker who can crash the hub gets a replay primitive.

**Decision.** Split the record in two. `begin(device, sequence, observation_id)` marks an
attempt in flight IN MEMORY before processing (deliberately not durable - it only guards
this process, and persisting it doubled the fsync cost of every observation);
`complete(...)` advances the watermark durably, and only after processing succeeded;
`abandon(...)` clears the marker when it did not. A repeat of the
last completed observation is answered from the record instead of being reprocessed. The
contract is explicitly at-least-once with idempotent acknowledgment of an unacknowledged
repeat - NOT exactly-once, and the remaining crash window is documented rather than hidden.
Concurrent attempts at the same sequence are refused while the in-flight marker is fresh; a
marker older than `in_flight_timeout_ms` is a dead attempt and the retry proceeds.

**Consequences.** A processing failure never consumes a sequence, an acceptance is never
reported for work that failed, and an unacknowledged observation is answered rather than
duplicated. The cost is one durable write per observation attempt (two on the happy path),
bounded to the device count.

## ADR-019 — Durable replay protection is mandatory; the opt-out is loopback-only

**Context.** `setup_replay_protection()` accepted an empty `replay_state_path` and printed a
warning. A live boundary could therefore run with replay protection disabled by omission,
and a warning in a container log is not a control.

**Decision.** An unset path is refused. The only way to run without durable replay state is
`allow_ephemeral_replay_protection: true`, which is itself refused unless the bind address
is loopback. The policy is a pure function in `src/net/IngressConfigPolicy.{h,cpp}` - free
of TLS and sockets - so the host suite tests it rather than requiring a container.

**Consequences.** A live deployment cannot lose replay protection silently. Isolated tests
keep a documented path, and the ingress announces when it is used.

## ADR-020 — Video event detection is trigger-driven, not scheduled

**Context.** Any fixed inference interval fails. Two minutes is functionally blind (~2 minutes:
a subject crosses in seconds). One to two seconds sustains CPU load to the point of thermal
throttling. Five seconds is thermally survivable but still misses a subject crossing in 3-4 s.
A fast subject is missed twice over: the sample may not land during the crossing, and the
subject has left the frame by the time analysis runs. Raising the sample rate does not fix it,
because cost grows linearly with the rate while the device throttles.

**Decision.** Video event analysis is event-gated, never scheduled. A cheap always-on change
detector (Tier-0) runs at 10-15 checks/s on a reduced luma image (80x60 or 160x90); an
asymmetric gate opens only on validated motion (one strong hit fires immediately, weak motion
requires 2 of the last 3 observations, and the gate latches 0.8-1.5 s past the last hit); the
expensive analyser (Tier-1) sleeps until the gate fires and then analyses 3-8 selected frames
from **before** the trigger. The event timestamp is recorded on the first weak hit, not at
confirmation, because that timestamp anchors the look-back window. Thermal pressure is relieved
by shrinking Tier-1 work, never by reducing the Tier-0 rate.

**Rationale.** The requirement is continuous responsiveness without invoking throttling.
Decoupling cheap continuous observation from expensive gated analysis is the only shape that
delivers both. Rejected: a longer interval (misses fast subjects), a shorter interval (throttles),
and requiring several consecutive motion frames before triggering — at 10 Hz that spans
200-300 ms, so a short crossing may never accumulate enough hits, which misses precisely the
events the system exists to catch.

**Consequences.** Fast subjects are caught because the pre-trigger buffer already holds them.
Cost becomes a function of event rate rather than sample rate, so idle load is near zero and
scales with activity instead of time. It costs architectural complexity: a pre-event ring, a
bounded event queue, a latched gate and a thermal controller with explicit operating states.
Consultation order for anything in this area: `docs/EDGE_VIDEO_EVENT_DETECTION.md`.

## ADR-021 — The edge node stays a native NDK/Bionic runtime

**Context.** The analysis needed a runtime for the device. Candidate routes were a native
NDK/Bionic binary, a Termux userland, or a Debian/Ubuntu rootfs under PRoot. The PRoot route
intercepts every syscall through `ptrace` and costs 300 MB-1 GB on disk; Termux costs ~200 MB
and a second app install. The device has no root, SELinux is enforcing and verified boot is
green, so there is no privileged shortcut available regardless.

**Decision.** The device runs the real binaries natively under Android's Bionic libc,
cross-compiled with the NDK for `arm64-v8a` / API 30. No Termux, no PRoot/glibc userland, no new
package to host the runtime.

**Rationale.** Native Bionic carries no syscall interception, needs no extra app install, and
matches the libc the platform itself uses. Verified end to end: the binaries build for, start
on, and serve from the device with real persistence. `ninja` is deliberately not a dependency
(`docs/DECISIONS.md`, no-Ninja decision); use `make -j4`. Rejected the alternatives on
disk cost, syscall overhead, and an extra unmanaged app in the trust path.

**Consequences.** The device remains a single controlled runtime. Cross-compilation is a build
step the project already scripts, and new native code follows the same route. The cost is that
anything requiring a glibc-only dependency is unavailable without porting it to Bionic — visible
already in the OpenSSL gap (ADR-022 consequences).

## ADR-022 — Camera ingestion is network-side; the edge service holds no camera permission

**Context.** Tier-0 needs frames. Two sources were available: the device's own camera, or a LAN
camera reachable from the device. The device camera requires CAMERA permission on the service
and constitutes activating a sensor on the handset. The network path requires only the INTERNET
permission the service already holds.

**Decision.** Frame acquisition is over the network from the LAN camera. The production edge
service requests and holds no CAMERA or RECORD_AUDIO permission. The separate harness package is
the only component that requests them, and it is neither running nor granted.

**Rationale.** Acquisition is verified working from the device (HTTP 200, valid JPEG payload,
~28 ms round trip), so the network path is proven rather than assumed. Keeping the service a
network client means continuous monitoring never activates an on-device sensor as a side effect,
and it keeps the trust surface small: no sensor permission on the always-running component.
Rejected the device camera as the default source.

**Consequences.** The service can monitor continuously without any sensor-activation question
arising, and the same code path works against any reachable camera. The cost is that the node
depends on network reachability and on the camera's own availability — the recovery behaviour
for address changes and restarts is therefore load-bearing (see ADR-023 note in
`docs/EDGE_VIDEO_EVENT_DETECTION.md`), and mutual-TLS ingestion remains unavailable on this node
until OpenSSL is cross-compiled for Android.

## ADR-023 — The pre-event ring retains compressed packets, not decoded frames

**Context.** Retrospective analysis requires retaining 1-3 s of history so Tier-1 can look back
to where the event began. The originating design retained decoded high-resolution frames. The
representation determines both memory and the achievable window: for a 2-second payload,
80x60 grayscale is 141 KiB, 320x240 BGR is 6.6 MiB, and 1920x1080 BGR is 356 MiB, while the same
window as compressed H.264 is ~1 MB at 4 Mbit/s.

**Decision.** Retain compressed packets (reference-counted, later fed to the decoder) for an
encoded camera, raw JPEG bytes for an MJPEG webcam, and a reduced frame size for a raw webcam.
Never retain decoded full-resolution frames. Retain **more than the desired pre-roll**: a clip
cannot begin decoding from an arbitrary P/B frame, so the ring must extend past the preceding
keyframe — with a 2 s pre-roll and a 1 s GOP, retain 3-4 s.

**Rationale.** ~350x reduction in memory for the same window, which is what makes a longer
retention window affordable. A longer window is what guarantees a keyframe is inside it.
Rejected the decoded-frame ring: it is the single largest memory mistake available in this
design, and it does not improve what Tier-1 can see.

**Consequences.** Memory stays bounded and small, and the retention window can be generous. The
cost is decode complexity: the decode worker must seek back to the preceding keyframe, feed
packets, discard output before the desired start, and only then analyse. Retention must never
fall below the GOP length or the clip becomes undecodable.

## ADR-024 — Where design references conflict, the later and stricter one governs

**Context.** Three design references were reviewed. They are not independent: the first is the
origin, the second reviews the first and states corrections, the third reviews both. They
contradict each other in places — the ring representation (ADR-023), trigger strictness
(ADR-020), whether sustained inference risks physical damage, and the latch duration (the
earliest says 750 ms; the later two say 0.8-1.5 s).

**Decision.** Where the references conflict, the later and more rigorous document governs, and
its correction is applied explicitly rather than by averaging. Numeric parameters that vary
across all three are recorded as ranges (latch 0.8-1.5 s, weak 0.4-0.6%, strong 2-2.5%) rather
than as one author's figure. Guidance on which all three agree is treated as settled: the
asymmetric trigger, the latched gate, 3-8 frame sampling, the 1-2 job bound, and disabling
runtime thread spinning.

**Rationale.** The references disagree because they were written in sequence with review, so
recency correlates with correctness — but only where the later document says it is correcting
the earlier one. Averaging would silently discard the correction. Recording ranges where the
spread is genuine tuning latitude keeps the numbers honest.

**Consequences.** The design has a defensible provenance and its known corrections are visible
rather than buried. Two corrections were applied: the ring stays compressed, and "CPU burn-out"
is too strong (the device throttles). The cost is that the documented parameter set is
deliberately a range, so the first real device measurement — not the references — settles the
values actually used. None of the references was validated on the target hardware; see
`docs/EDGE_VIDEO_EVENT_DETECTION.md` §7.
