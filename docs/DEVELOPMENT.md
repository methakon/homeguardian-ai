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
