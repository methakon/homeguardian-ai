# Demo Guide — Simulated Alexa+ Web Experience

**What this is:** a one-command, reproducible, local demo of HomeGuardian AI's
household-safety workflow, presented as a simulated Alexa+ conversational
experience. Everything runs on your machine over loopback; no cloud, no
network, no physical sensor.

> This is a **project-built simulation**, not a real Alexa+ integration. There
> is no MCP server and no live Amazon identity. All observations are synthetic.

---

## 1. Start the demo

```bash
bash demo.sh
```

The script will:

1. Build the C++ bridge (`hg_alexa_bridge`) if it is not already built,
   reusing an existing build directory and never deleting it.
2. Create synthetic, isolated test credentials in a temp directory.
3. Start the web server bound to **127.0.0.1 only**.
4. Print a URL like `http://127.0.0.1:<port>`.

Open that URL in a browser. Press **Ctrl+C** to stop.

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| C++20 compiler | g++ ≥ 11 | The first CMake configure fetches spdlog + nlohmann/json (**network needed once**). |
| CMake | ≥ 3.20 | |
| make | any | |
| Node.js | ≥ 18 | Standard library only; no `npm install` required for the demo. |

### Options

| Variable | Default | Meaning |
|---|---|---|
| `HG_DEMO_BUILD_DIR` | `hg_build` | Build directory to use/reuse for the bridge. |
| `HG_DEMO_PORT` | `0` | `0` picks a free port; set a fixed port if you prefer. |

---

## 2. The scenario (what to click)

The page has four areas:

1. **System status** — application state, capture/consent state, and live
   counters (events processed, alerts delivered, duplicates suppressed).
2. **Ask HomeGuardian** — a conversational panel with suggested questions.
3. **Alert timeline** — alerts persisted by the real backend, each with its
   stable id, rule name, and severity.
4. **How this works & your privacy** — the consent/privacy explanation.

### Walkthrough

| Step | Action | Expected result |
|---|---|---|
| 1 | Open the URL | Status shows **Running**, capture **Enabled (synthetic)**, mode **Simulated**. A banner explains this is a simulation. |
| 2 | Click **▶ Run synthetic scenario** | Status updates: **10 events processed**, **9 alerts generated**, **1 delivered**, **8 suppressed** (dedup/cooldown). The alert timeline shows one alert (`sim-alert-N`, rule `sim-presence`, severity `info`). |
| 3 | Ask **"Is everything okay?"** | "There is 1 alert on record, highest severity info." (Counts come from the real backend.) |
| 4 | Ask **"Is capture enabled?"** | "Capture is enabled for the synthetic sensor, and consent is granted — but this is simulated, and no physical camera or microphone is active." |
| 5 | Click **▶ Run synthetic scenario** again | A second alert is appended; the earlier alert is still listed (durable history). |
| 6 | Click **↻ Refresh status** | Counters and timeline reflect the current backend state. |
| 7 | Ask **"What can you do?"** | Describes the available read-only queries and notes the simulation. |

Every number and alert shown is read from the running C++ backend
(consent gate → event pipeline → correlation → dedup → SQLite). Nothing on the
page is hardcoded.

### Demonstrating fail-closed behaviour

To show that a disabled capture switch blocks all observations, restart the
demo against the default (capture-off) config:

```bash
HG_DEMO_PORT=8199 node src/bridge/hg_demo_server.js \
  --bridge hg_build/hg_alexa_bridge \
  --config config/homeguardian.json \
  --creds <creds.json> --port 8199 --token demo-token
```

Running the scenario then reports `events_processed: 0` and explains that the
consent/capture gate blocked every observation. (`demo.sh` uses
`config/homeguardian.demo.json`, which enables capture so the scenario runs.)

---

## 3. Reset / restart

The demo's SQLite database (`homeguardian_bridge_audit.db`) lives in the
server's working directory, which `demo.sh` sets to a temp directory — so it is
**never inside the repository** and is removed when the demo stops. To start
completely fresh, just stop (Ctrl+C) and run `bash demo.sh` again. The in-page
**Reset demo data** button explains this and makes no destructive changes to the
repo.

---

## 4. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `bridge binary not found` | The bridge was not built. | Run `cmake --build hg_build --target hg_alexa_bridge`, or let `demo.sh` build it. |
| `demo server exited unexpectedly` | Port in use, or Node < 18. | Set `HG_DEMO_PORT` to a free port; check `node --version`. |
| "Backend unavailable" in the UI | The bridge process failed to start. | Confirm `hg_build/hg_alexa_bridge` exists and is executable. |
| Scenario shows 0 events | Capture is disabled in the config. | Use `config/homeguardian.demo.json` (the `demo.sh` default). |
| First configure fails | No network for FetchContent. | Connect once to fetch spdlog/nlohmann/json, or reuse an existing `hg_build`. |

---

## 5. Suggested demo video walkthrough (< 3 minutes)

A tight script that fits the time limit and shows the product working:

| Time | Shown | Say |
|---|---|---|
| 0:00–0:15 | Terminal: `bash demo.sh`, browser opens | "HomeGuardian AI — a privacy-first home safety system. This is a simulated Alexa+ experience running entirely locally." |
| 0:15–0:35 | The UI: banner, status card | "Status is Running, capture is enabled for a synthetic sensor. No physical camera or microphone is ever activated." |
| 0:35–1:00 | Click **Run synthetic scenario** | "Two synthetic observations arrive, the correlation rule fires, and an alert is generated. Note 9 generated, 1 delivered, 8 suppressed by the cooldown policy." |
| 1:00–1:20 | Alert timeline + ask "Is everything okay?" | "The alert is persisted with its id, rule, and severity. The answer comes from the real backend, not from the page." |
| 1:20–1:45 | Ask "Is capture enabled?"; run scenario again | "Consent-first: if capture or consent is withdrawn, the gate blocks everything and nothing reaches the pipeline." |
| 1:45–2:10 | Privacy card | "Local-first, no cloud, no raw media — only alert metadata is stored." |
| 2:10–2:40 | Terminal: `npm test` / test counts scrolling | "255 C++ test cases, 68 Node tests, ASan/UBSan clean, fresh-build reproducible." |
| 2:40–3:00 | Close | "This is a simulated Alexa+ experience built on a real, tested backend. Thank you." |

**Recording tips:** 1080p, show the terminal command and the browser side by
side or in sequence, keep the cursor visible, and end on the test suite running
so judges see the engineering depth.
