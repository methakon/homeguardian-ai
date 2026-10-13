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

## ADR-006: Implementation History — Mixed Tool Usage

**Status:** Accepted
**Date:** 2026-10-09 (updated 2026-10-10)

### Context
The project used different implementation approaches across phases. Phase B was
implemented using the Antigravity CLI (an AI coding agent). Phase C was
implemented directly by the developer without AI tools. Phase D was implemented
directly. For Phase E the developer authorized the use of AI coding agents
(OpenCode and Antigravity) for parallel, independent work.

### Decision
- Phase B code was reviewed and cleaned of AI fingerprints (commit `e9ebfa3`).
- Phase C and Phase D were implemented directly.
- Phase E used OpenCode and Antigravity agents for independent, file-scoped
  tasks (core model headers), with Hermes integrating, building, testing, and
  reviewing. Parallel agents were assigned disjoint files to avoid conflicts.
- Documentation must accurately reflect this history without concealing tool
  usage.

### Rationale
- Honesty about tool usage is required.
- The developer explicitly authorized parallel AI agents for Phase E, which
  supersedes the earlier "direct implementation only" stance for that phase.
- All agent output was compiled, tested, and reviewed before commit.

### Consequences
- Documentation must not claim "no AI tools used" when they were.
- Future phases follow whatever tooling the developer authorizes at the time.

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
- `--http` mode is GET-only and loopback-only; `run-scenario` is the only
  state-advancing route (bounded ≤100 cycles); capture/consent/config stay
  read-only.
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
loopback use: it is unauthenticated by design, relies on `Host`/`Origin`
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
