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
- spdlog (structured logging)
- nlohmann/json (JSON serialization)
- SQLite (local persistence)
- libcurl (HTTP client)
- Catch2 (unit testing)

### Proposed (not yet implemented)
- OpenCV (computer vision)
- ONNX Runtime (local ML inference)
- Boost.Beast or Drogon (HTTP server)
- MCP Streamable HTTP (Alexa+ integration)
- FFmpeg (media processing)

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full architecture.

## Project Structure

```
homeguardian-ai/
├── CMakeLists.txt          # Root build configuration
├── src/                    # Application source code
│   ├── core/              # Core types, interfaces, event model
│   ├── sensors/           # Sensor abstractions and implementations
│   ├── inference/         # ML inference interfaces
│   ├── pipeline/          # Event processing pipeline
│   ├── persistence/       # SQLite storage
│   ├── server/            # HTTP/MCP server
│   └── main.cpp           # Application entry point
├── tests/                  # Unit and integration tests
├── docs/                   # Project documentation
├── config/                 # Configuration files
├── scripts/                # Build and utility scripts
└── web/                    # Lightweight dashboard (HTML/CSS/JS)
```

## Current Status

**Phase A — Orchestration and repository foundation.** The repository is
initialized with documentation. No application code has been implemented yet.

See [docs/STATUS.md](docs/STATUS.md) for the latest status and
[docs/ROADMAP.md](docs/ROADMAP.md) for the implementation roadmap.

## Building

```bash
mkdir build && cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

## Testing

```bash
cd build
ctest --output-on-failure
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
