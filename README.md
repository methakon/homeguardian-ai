# HomeGuardian AI

Whole-home intelligence for safety, wellbeing, and convenience — privacy-first,
consent-gated, and local-first.

> **Hackathon demo note.** This repository is a submission for the *Build, Ship,
> Shape: Amazon Developer Hackathon*, **Alexa+ track**, taking the officially
> permitted **simulated Alexa+ web experience** route. The conversational web
> demo is a **project-built simulation** — it is **not** a real Alexa+ device
> integration, there is **no MCP server**, and there is **no live Amazon
> identity**. All household observations are **synthetic**; no physical camera
> or microphone is ever activated. See [docs/AMAZONAPPDEV2026_FINAL_AUDIT.md](docs/AMAZONAPPDEV2026_FINAL_AUDIT.md).

## Vision

HomeGuardian AI is a privacy-first, multimodal home intelligence platform that
provides:

- **Consent-based wellbeing observations** — opt-in monitoring of activity
  patterns, with explicit user control.
- **Fall and safety-event detection** — alerts for potential falls or unusual
  inactivity, with configurable emergency workflows.
- **Contextual event correlation** — connecting events across time for
  meaningful insights.
- **Explainable alerts** — every alert includes the evidence that triggered it.
- **Simulated Alexa+ conversational experience** — a voice-first-style web
  interface for household status, built on the project's own agentic tooling.
- **Durable, privacy-safe audit** — alert metadata only, never raw media.

## Problem Statement

Families want to monitor their homes for safety and convenience without
sacrificing privacy or relying on cloud-dependent, opaque systems. Current
solutions either lack intelligence (dumb cameras) or send all data to the
cloud with no transparency. HomeGuardian AI aims to provide intelligent,
privacy-respecting home monitoring with explicit consent and local-first
processing.

## What is implemented vs simulated vs planned

**Implemented and tested (C++ core):**
- Event model, schema validation, and a fail-closed **consent gate**.
- **Consent-guarded delivery boundary** — media is delivered only while capture
  is enabled *and* consent is granted; withdrawal or capture-off blocks all
  delivery.
- **Event pipeline** with time-window **correlation rules**.
- **Alert deduplication / cooldown policy** with re-arm on condition clear.
- **SQLite persistence** with schema migration, transactional writes,
  age-based retention, and recovery.
- A deterministic in-process **sensor simulator** that exercises the full
  consent/lifecycle/delivery machinery on the shared code paths.
- A read-only **integration contract** (`AlexaContract`) exposing only
  `status` and `alert_summary`.
- A hardened **credential store** (bearer tokens, 0600 secure file, fail-closed).
- A **local NDJSON stdio bridge** and a **loopback-only HTTP adapter**.

**Simulated (this is a simulation, not real hardware or real Alexa):**
- The **Alexa+ conversational web experience** (`src/web/` +
  `src/bridge/hg_demo_server.js`) — a project-built simulation for
  demonstration. No Alexa device, no MCP, no live Amazon identity.
- All household observations are **synthetic**; no physical sensor is activated.

**Planned (not implemented):**
- OpenCV / ONNX Runtime for real local ML inference.
- Real camera/microphone capture backends (V4L2/ALSA/PipeWire) — **blocked
  pending explicit approval**.
- A real Alexa+ **MCP server** (spec 2025-11-25+, Streamable HTTP) — not built.
- Amazon **Login with Amazon** account linking — not built.
- Ring integration — **not implemented**.

## Technology Stack

- C++20 (g++ 13.3.0)
- CMake 3.28.3 (build system)
- spdlog v1.13.0 (structured logging) — via CMake FetchContent
- nlohmann/json v3.11.3 (JSON) — via CMake FetchContent
- SQLite amalgamation (persistence) — vendored in `third_party/sqlite`
- Catch2 v2.13.10 (testing) — vendored in `third_party/catch2`
- Node.js (standard library only) — **optional**, for the legacy demo server and
  the Alexa skill prototype tests only; not required by the default demo

No unreviewed third-party dependency is used by the demo or bridge; the native
demo and bridge use only the C++ standard library. The optional legacy demo
server and the Alexa skill prototype use only the Node.js standard library.

## Quick start — run the demo

The fastest way to see the product is the simulated Alexa+ web experience:

```bash
bash demo.sh
```

This builds the C++ bridge if needed, starts a loopback-only web server, and
prints a `http://127.0.0.1:<port>` URL. Open it, click **Run synthetic
scenario**, and ask questions like *"Is everything okay?"*. See
[docs/DEMO_WEB.md](docs/DEMO_WEB.md) for the full walkthrough, expected output,
and troubleshooting.

> Prerequisites: a C++20 compiler, CMake ≥ 3.20, and `make`. **Node.js is NOT
> required** for the demo — the native C++ server serves the dashboard and API in
> one process. (Node.js ≥ 18 is needed only for the optional legacy demo server
> and the Alexa-skill prototype tests.)
> The first configure fetches spdlog and nlohmann/json (network needed once).

## Building the C++ core

The project builds out-of-source. On the first configure, CMake fetches spdlog
and nlohmann/json from GitHub, so **network access is required once** to
populate the dependency cache.

```bash
mkdir -p hg_build && cd hg_build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make -j4 homeguardian homeguardian_tests hg_alexa_bridge
```

This builds the `homeguardian` application, the `homeguardian_tests` target,
and the `hg_alexa_bridge` used by the web demo. To build only the tests:
`make -j4 homeguardian_tests`.

## Testing

```bash
cd hg_build
ctest --output-on-failure
```

Run the test binary directly with a tag filter:

```bash
./homeguardian_tests "[sim]"
./homeguardian_tests "[e2e]"
./homeguardian_tests "[alexa]"
./homeguardian_tests "[creds]"
```

Node tests (skill handlers, contract, bridge integration, HTTP adapter, web
demo):

```bash
cd alexa_skill
npm test
```

## Project Structure

```
homeguardian-ai/
├── CMakeLists.txt          # Root build configuration
├── demo.sh                 # One-command reproducible web demo
├── src/
│   ├── main.cpp            # Application entry point
│   ├── core/               # Event model, consent gate, pipeline, rules, contract
│   ├── persistence/        # SQLite repositories + schema/migration
│   ├── backend/
│   │   ├── android/        # Android NDK camera/audio backends
│   │   ├── linux/          # Linux sensor discovery (metadata)
│   │   └── simulator/      # Deterministic in-process sensor simulator
│   ├── bridge/             # stdio bridge, loopback HTTP adapter, web demo server
│   └── web/                # Simulated Alexa+ web experience (UI)
├── apk/                    # Android harness (Java + JNI + build script)
├── alexa_skill/            # Alexa custom skill prototype (ASK SDK v2, Node.js)
├── tests/                  # Unit, integration, and end-to-end simulator tests
├── docs/                   # Project documentation
├── config/                 # Configuration files
└── third_party/            # Vendored SQLite amalgamation + Catch2
```

## Current Status

**Simulator-first development with consent-gated acquisition.** The C++ core
(event model, consent gate, consent-guarded device delivery boundary, event
pipeline + time-window correlation rules, alert dedup policy, and SQLite
persistence) is implemented and covered by the host test suite. A deterministic
in-process sensor simulator exercises the full consent/lifecycle/delivery/
bounded-buffer machinery on the shared code paths, and an end-to-end simulator
workflow is tested. The simulated Alexa+ web experience drives that same
backend, so the demo shows real event processing, correlation, alerts, and
durable persistence.

**No real camera or microphone capture is implemented or authorized.** Capture
is disabled by default (`media_capture_enabled: false`) and every frame/sample
must pass the consent delivery boundary before processing. Physical-hardware
capture remains blocked pending explicit approval. See
[docs/ROADMAP.md](docs/ROADMAP.md) for per-layer verification status and
[docs/STATUS.md](docs/STATUS.md) for the latest status.

## Privacy and Consent

HomeGuardian AI is designed with privacy as a core principle:

- **Explicit consent** for all monitoring activities.
- **Local-first processing** — data stays on the local machine.
- **Configurable retention** — automatic alert-metadata expiration.
- **No covert monitoring** — all sensing is user-configurable and auditable.
- **Synthetic fixtures** for development — no real household data.
- **No raw media persisted** — only alert metadata (id, severity, rule,
  timestamp) is stored.

## License

HomeGuardian AI's original source code is released under the **Apache License,
Version 2.0**. See [LICENSE](LICENSE) for the full text and
[NOTICE](NOTICE) for copyright and third-party attribution.

Bundled and fetched third-party components retain their own licenses and are
**not** relicensed under Apache-2.0 (SQLite — public domain; Catch2 — Boost
Software License 1.0; spdlog and nlohmann/json — MIT; ask-sdk-core and
ask-sdk-model — Apache License 2.0). See [NOTICE](NOTICE).

## Contributing

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for development guidelines.
