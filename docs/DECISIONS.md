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

## ADR-006: Deferred License Selection

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
