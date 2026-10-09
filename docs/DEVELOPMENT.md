# HomeGuardian AI — Development Guide

## Toolchain

| Tool | Version | Purpose |
|------|---------|---------|
| g++ | 13.3.0 | C++20 compiler |
| CMake | 3.28.3 | Build system |
| Git | 2.x | Version control |
| agy | 1.3.2 | Antigravity CLI for delegated implementation |

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
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

The `-j4` flag limits parallelism to 4 jobs, respecting the 30% CPU budget.

## Testing

```bash
cd build
ctest --output-on-failure
```

Tests use Catch2. Each module has its own test executable.

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

## Antigravity Workflow

See [docs/ORCHESTRATION.md](docs/ORCHESTRATION.md) for the full delegation workflow.

Key rules:
- `agy` writes code; Hermes runs every command
- `agy` must never run `git` — all git operations stay with Hermes
- `agy` uses `--mode plan` for investigation, `--mode accept-edits` for edits
- Hermes verifies every result before committing

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
