# HomeGuardian AI

Whole-home intelligence for safety, wellbeing, and convenience — privacy-first,
consent-gated, and local-first.

> **Hackathon disclosure.** This submission uses the **Alexa+ track's self-hosted
> MCP route**: native C++ MCP 2025-11-25 over Streamable HTTP, with two read-only
> tools ([MCP server](docs/MCP_SERVER.md)). A separate functional **classic ASK
> hosted skill → AWS SQS → Oracle private worker → Android bridge → native C++
> tools** path exists. Alexa voice replies are **owner-attested**; independent
> mailbox/native verification and remaining reliability/deployed-code limitations
> are recorded in [live-path verification](docs/ALEXA_LIVE_PATH_VERIFICATION.md).
> This is **not production Alexa+ integration or certified monitoring**. Detector
> demos and regression inputs remain explicitly synthetic. Video recording,
> publication and final submission are not verified.

**Published portal:** https://homeguardian.berhampore.in/ — the native
`hg_alexa_bridge` HTTP portal behind a Cloudflare tunnel, bound to loopback on the
deployment host. It reports live hub state and alerts. No camera is configured, no
observations feed that instance, and capture is disabled, so its camera panel answers
"not configured" and its device/fusion counters are zero. Deployment notes:
[portal deployment](docs/PORTAL_DEPLOYMENT.md); honest scope:
[real-event pipeline acceptance](docs/REAL_EVENT_PIPELINE_ACCEPTANCE.md).

## Vision

HomeGuardian AI is a privacy-first, multimodal home intelligence platform that
provides:

- **Consent-based wellbeing observations** — opt-in monitoring of activity
  patterns, with explicit user control.
- **Fall and safety-event detection** *(planned — see the implemented/planned
  breakdown below; not implemented)* — alerts for potential falls or unusual
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

**Implemented and tested (C++ core, Native MCP & Network Bridge):**
- Event model, schema validation, and a fail-closed **consent gate**.
- **Consent-guarded delivery boundary** — media is delivered only while capture
  is enabled *and* consent is granted; withdrawal or capture-off blocks all
  delivery.
- **Event pipeline** with a **detection/fusion-driven alert rule** — an alert is
  raised only when the fused detector evidence qualifies (raw observations alone
  never alert; they are the detector input).
- **Statistical anomaly detection** — a bounded numeric feature per authorized
  observation is compared with a learned rolling baseline (mean/stddev, warm-up,
  high/low/missing). A deviation is reported and persisted as a *statistical*
  finding; it is **advisory by default** and is not a fall, emergency or danger
  claim (an explicit, tested evidence allowlist controls whether it may vote).
- **Alert deduplication / cooldown policy** with re-arm on condition clear.
- **SQLite persistence** with schema migration, transactional writes,
  age-based retention, and recovery.
- A deterministic in-process **sensor simulator** that exercises the full
  consent/lifecycle/delivery machinery on the shared code paths.
- A read-only **integration contract** (`AlexaContract`) exposing only
  `status` and `alert_summary`.
- A hardened **credential store** (bearer tokens, 0600 secure file, fail-closed).
- A **local NDJSON stdio bridge** and a **loopback-only HTTP adapter**.
- **Native C++ MCP 2025-11-25 Server (`hg_mcp_server`)** — implements JSON-RPC 2.0
  over Streamable HTTP, session lifecycle, and strictly two read-only tools:
  `get_homeguardian_status` and `get_alert_summary`. Zero mutating or camera tools.
- **Outbound Authenticated MCP Coordinator (`deploy/coordinator/service.js`)** —
  Streamable HTTP ingress combined with an authenticated RFC6455 WebSocket edge control bridge.
- **Hardened Security & Peripheral Lifecycle** — CSPRNG pairing, operator-authorized
  recovery, 0600 secret files, loopback Origin/Host enforcement, and CSRF token binding (HG-SEC-01 through HG-SEC-06D).
- **Android Edge Service Candidate** — v1.0.3 package (`apk_service/`), byte-reproducible
  normalization (`scripts/normalize_apk_zip.py`, whole-file SHA-256 `e3116ac18d70...`),
  VPN route validation and socket pinning (`RouteSecurityPolicy.java`), and off-device synthetic
  platform route test suite (**historical candidate: 44/44 PASS**, not a current HEAD verdict).

**Simulated (detector/demo/regression inputs, not the separate working ASK lane):**
- The **Alexa+ conversational web experience** (`src/web/` +
  `web_mcp/server.js`) — a project-built simulation for demonstration.
  Acts as an actual MCP client (sending initialize, tools/list, tools/call) and
  renders simulated sensor test patterns; it is not a production Alexa+ runtime.
- The reproducible detector demonstration uses **synthetic** observations, not
  physical sensor-to-alert acceptance. Uploaded-media simulated playback is not
  live camera capture or detection. An owner-controlled live feed in the private
  device listing is optional and **pending verification**, not claimed implemented.

**Physical Hardware & Sensor Status (Fail-Closed Boundary):**
- **V380 Wi-Fi IP Cameras:** Camera 1's unauthenticated RTSP/ONVIF port exposure was
  discovered and closed; Camera 2 has vendor-only proprietary ports and remains unintegrated.
  Hardware capture backend (`V4L2`/PipeWire) is disabled by default (`media_capture_enabled: false`)
  and blocked pending physical authorization.
- **Android node:** The deployed private ASK path reaches native C++ tools through
  the Android bridge; permanent stale-session and reboot/network recovery are not
  verified. Capture remained disabled; see [live-path evidence](docs/ALEXA_LIVE_PATH_VERIFICATION.md).
- **Panasonic Eluga (CCTV Handset):** Separate legacy Android 8.1 / API 28 device used for auxiliary RTSP/CCTV staging; unprovisioned with judge credentials and isolated from the release runtime.
- **Workstation-First Evaluation Boundary:** The judge release is 100% self-contained on workstation loopback (`127.0.0.1`) with zero physical Android or cloud dependencies required for evaluation.

**Planned (not implemented):**
- Real on-device ML inference (OpenCV / ONNX Runtime).
- Physical sensor-to-alert acceptance and verified private-dashboard live camera
  integration — not claimed by this submission.
- Amazon **Login with Amazon (LWA)** account linking — not built.
- Fall detection pose inference (H-01) and emergency calling workflow (H-02) —
  **not implemented**.
- Ring integration — **not implemented**.
- General public remote access — not claimed; the existing Oracle worker uses a
  private path. The reproducible synthetic demo needs no cloud or VPN connection.

## Technology Stack

- C++20 (g++ 13.3.0)
- CMake 3.28.3 (build system)
- spdlog v1.13.0 (structured logging) — via CMake FetchContent
- nlohmann/json v3.11.3 (JSON) — via CMake FetchContent
- SQLite amalgamation (persistence) — vendored in `third_party/sqlite`
- Catch2 v2.13.10 (testing) — vendored in `third_party/catch2`
- Node.js — **optional**; not required by the default demo or by `demo.sh`
  - the legacy demo server (`src/bridge/hg_demo_server.js`) and the Alexa skill
    prototype use the **Node.js standard library only**
  - the MCP web gateway (`web_mcp/server.js`), its dashboard and its test suites
    additionally need two npm packages, declared in the root `package.json`
    (`werift`, `ws`). Install them once from the repository root before using
    anything under `web_mcp/`:
    ```bash
    npm install          # one-time; 43 packages, ~5 s on a warm cache
    ```

No unreviewed third-party dependency is used by the native demo or bridge: both
use only the C++ standard library. The Node packages above are used **only** by
the optional `web_mcp/` gateway, dashboard and their tests, and by nothing in
`demo.sh`.

## Quick start — run the demo

The fastest way to see the product is the simulated Alexa+ web experience:

```bash
bash demo.sh
```

This builds the C++ bridge if needed, starts a loopback-only web server, and
prints a `http://127.0.0.1:<port>` URL. Open it, pick a scenario (**Run synthetic
scenario**) and ask questions like *"Is everything okay?"*. Ten scenarios are
shipped, including `synthetic-critical-impact`: a simulated impact-like signature
(a high-intensity transient followed by a completing stillness sample) that raises
one **critical** alert with its own evidence. It is a **simulation demonstration
of severity prioritisation, not fall detection** — the dashboard says so and no
emergency service is involved. See [docs/DEMO_WEB.md](docs/DEMO_WEB.md) for the
full walkthrough, expected output, and troubleshooting.

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
make -j4 homeguardian homeguardian_tests hg_alexa_bridge hg_mcp_server hg_device_registry_cli
```

This builds the `homeguardian` application, the `homeguardian_tests` target, the
`hg_alexa_bridge` used by the web demo, the **`hg_mcp_server`** (the MCP server
described above — MCP 2025-11-25 over Streamable HTTP) and the
`hg_device_registry_cli` used by the device dashboard. To build only the tests:
`make -j4 homeguardian_tests`. `hg_mcp_server` binds to loopback only and is not
required by `demo.sh`.

## Testing

Run the native C++ test suite (counts depend on the checked-out revision):

```bash
cd build
HG_SOURCE_DIR=.. ./homeguardian_tests
```

Run with CTest or with a tag filter:

```bash
ctest --output-on-failure
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

MCP server and MCP client verification (requires `hg_mcp_server` to be built and
`npm install` to have been run from the repository root — see above). These are
reproduction commands, not a claim that every suite is green at current HEAD:

```bash
bash scripts/verify_judge_release.sh    # Automated judge smoke test: Native C++, MCP 2025-11-25, Coordinator, Security
bash scripts/verify_mcp.sh              # native protocol suite
node web_mcp/test_client.js             # MCP client suite
node web_mcp/test_devices_dashboard.js   # dashboard/WebRTC/secrets
node tests/test_java_ws_bridge_cxx_e2e.js # Java bridge <-> Coordinator <-> C++ engine
node tests/test_outbound_mcp_bridge.js  # authenticated outbound MCP bridge
```

`scripts/verify_mcp.sh` provisions its own 0600 credential file in a temporary
directory at run time and drives the real binary over loopback; it commits
nothing and exposes no listener beyond `127.0.0.1`.

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
pipeline + a detection/fusion-driven alert rule, alert dedup policy, and SQLite
persistence) is implemented and covered by the host test suite. A deterministic
in-process sensor simulator exercises the full consent/lifecycle/delivery/
bounded-buffer machinery on the shared code paths, and an end-to-end simulator
workflow is tested. The simulated Alexa+ web experience drives that same
backend, so the demo shows real event processing, detection, alerts, and
durable persistence.

**Physical sensor-to-alert acceptance is not claimed.** Capture is disabled by
default (`media_capture_enabled: false`); authorized capture must pass the consent
delivery boundary. The requested private owner-controlled live feed remains
pending verification. The working ASK lane and its limitations are documented in
[live-path verification](docs/ALEXA_LIVE_PATH_VERIFICATION.md). See
[docs/INDEX.md](docs/INDEX.md) for the master component index and architecture guide,
[docs/ROADMAP.md](docs/ROADMAP.md) for per-layer verification status,
[docs/STATUS.md](docs/STATUS.md) for the latest status, and
[docs/RELEASE_CANDIDATE_V21.md](docs/RELEASE_CANDIDATE_V21.md) for the
release-candidate verification report (tested functionality, known limitations
and reproduction commands).

## Privacy and Consent

HomeGuardian AI is designed with privacy as a core principle:

- **Explicit consent** for all monitoring activities.
- **Local-first processing** — no raw sensor media over AWS; aggregate read-only
  status/alerts do cross AWS in the ASK mailbox path.
- **Configurable retention** — automatic alert-metadata expiration.
- **No covert monitoring** — all sensing is user-configurable and auditable.
- **Synthetic fixtures** for development — no real household data.
- **Sensor-pipeline retention** — alert metadata, not raw sensor media. Separate
  explicit operator uploads may store media locally for simulated playback;
  playback is neither live camera capture nor detection.

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
