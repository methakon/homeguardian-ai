# HomeGuardian AI

Whole-home intelligence for safety, wellbeing, and convenience.

## Vision

HomeGuardian AI is a privacy-first, multimodal home intelligence platform that uses
cameras, microphones, and sensors to provide:

- **AI Front Door Receptionist** — identify visitors, announce deliveries, and
  interact with people at the door.
- **AI Family Concierge** — voice-activated assistance for household tasks,
  schedules, and information.
- **Consent-based wellbeing observations** — opt-in monitoring of activity
  patterns, with explicit user control.
- **Fall and safety-event detection** — alerts for potential falls or unusual
  inactivity, with configurable emergency workflows.
- **Contextual event correlation** — connecting events across time and sensors
  for meaningful insights.
- **Explainable alerts** — every alert includes the evidence that triggered it.
- **Alexa+ conversational experience** — voice-first interaction via Alexa+
  with MCP integration.
- **Ring integration** — doorbell and security camera event ingestion.

## Problem Statement

Families want to monitor their homes for safety and convenience without
sacrificing privacy or relying on cloud-dependent, opaque systems. Current
solutions either lack intelligence (dumb cameras) or send all data to the
cloud with no transparency. HomeGuardian AI aims to provide intelligent,
privacy-respecting home monitoring with explicit consent and local-first
processing.

## Technology Stack

### Implemented
- C++20 (g++ 13.3.0)
- CMake 3.28.3 (build system)
- spdlog v1.13.0 (structured logging) — fetched via CMake FetchContent
- nlohmann/json v3.11.3 (JSON serialization) — fetched via CMake FetchContent
- SQLite amalgamation (local persistence) — vendored in `third_party/sqlite`
- Catch2 v2.13.10 (unit testing) — vendored in `third_party/catch2`

### Proposed (not yet implemented)
- OpenCV (computer vision)
- ONNX Runtime (local ML inference)
- Boost.Beast or Drogon (HTTP server)
- FFmpeg (media processing)

> Note: the Alexa+ / MCP integration is an **optional** workstream (see
> `alexa_skill/`); it is not a dependency of the C++ core.

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full architecture.

## Project Structure

```
homeguardian-ai/
├── CMakeLists.txt          # Root build configuration
├── src/
│   ├── main.cpp            # Application entry point
│   ├── core/               # Event model, consent gate, pipeline, rules
│   ├── persistence/        # SQLite repositories + schema
│   └── backend/
│       ├── android/        # Android NDK camera/audio backends (F22)
│       ├── linux/          # Linux sensor discovery (V4L2/PipeWire metadata)
│       └── simulator/      # Deterministic in-process sensor simulator
├── apk/                    # Android harness (Java + JNI + build script)
├── alexa_skill/            # Optional Alexa custom skill (ASK SDK v2, Node.js)
├── tests/                  # Unit, integration, and end-to-end simulator tests
├── docs/                   # Project documentation
├── config/                 # Configuration files
└── third_party/            # Vendored SQLite amalgamation + Catch2
```

## Current Status

**Simulator-first development with consent-gated acquisition.** The C++ core
(event model, consent gate, consent-guarded device delivery boundary, event
pipeline + time-window correlation rules, and SQLite persistence) is implemented
and covered by the host test suite. A deterministic in-process sensor simulator
(`src/backend/simulator`) exercises the full consent/lifecycle/delivery/
bounded-buffer machinery on the shared code paths, and an end-to-end simulator
workflow is tested.

**No real camera or microphone capture is implemented or authorized.** Capture is
disabled by default (`media_capture_enabled: false`) and every frame/sample must
pass the consent delivery boundary before processing. Physical-hardware capture
remains blocked pending explicit approval. See [docs/ROADMAP.md](docs/ROADMAP.md)
for per-layer verification status and [docs/STATUS.md](docs/STATUS.md) for the
latest status.

## Building

The project builds out-of-source. Use a build directory (do not reuse a stale
one). On the first configure, CMake fetches spdlog and nlohmann/json from GitHub,
so **network access is required once** to populate the dependency cache.

```bash
mkdir build && cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

This builds the `homeguardian` application and the `homeguardian_tests` target.
To build only the tests: `make -j4 homeguardian_tests`.

## Testing

```bash
cd build
ctest --output-on-failure
```

To run the test binary directly with a tag filter (e.g. simulator or end-to-end):

```bash
./homeguardian_tests "[sim]"
./homeguardian_tests "[e2e]"
```

## Privacy and Consent

HomeGuardian AI is designed with privacy as a core principle:

- **Explicit consent** for all monitoring activities.
- **Local-first processing** — data stays on the local network.
- **Configurable retention** — automatic data expiration.
- **No covert monitoring** — all sensing is user-configurable and auditable.
- **Synthetic fixtures** for development — no real household data.

## License

License to be selected. See [docs/DECISIONS.md](docs/DECISIONS.md).

## Contributing

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for development guidelines.
