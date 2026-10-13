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
