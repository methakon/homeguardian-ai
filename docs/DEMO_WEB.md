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
| 2 | Pick a scenario (e.g. **Unusual activity**) and click **▶ Run scenario** | Status updates: **15 events processed** (10 authorized synthetic observations + 5 detection observations), **10 alerts generated**, **1 delivered**, **9 suppressed** (dedup/cooldown). The **Scenario result** card shows the detector counts (5 detected), the fusion verdict (`detected`, confidence 1.00), the consent audit (`enforcement_ok: true`) and *why* the alert fired. The alert timeline shows the alert with rule `detection-fusion`, severity `warning`, its `synthetic` provenance and an explanation built from its evidence. |
| 3 | Ask **"Is everything okay?"** | "There is 1 alert on record, highest severity warning." (Counts come from the real backend.) |
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
hg_build/hg_alexa_bridge --http \
  config/homeguardian.json \
  <creds.json> --port 8199 --token demo-token
```

Running the scenario then reports `events_processed: 0` and explains that the
consent/capture gate blocked every observation. (`demo.sh` uses
`config/homeguardian.demo.json`, which enables capture so the scenario runs.)

> **Native, no Node.js.** The demo is a single C++ process
> (`hg_alexa_bridge --http`) that serves the embedded dashboard and the JSON API
> over loopback. The historical Node server (`src/bridge/hg_demo_server.js`) is
> kept only as a reference for footprint comparison and is not required
> (`HG_DEMO_LEGACY_NODE=1 bash demo.sh` to use it).

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
| 0:35–1:00 | Pick **Unusual activity** and click **▶ Run scenario** | "Synthetic observations are acquired under consent and a detector classifies them. The fusion result agrees, so the detection rule raises an alert — the alert exists because the evidence qualified, not because a counter ticked. Ten firings, but the cooldown policy delivers one and suppresses nine." |
| 1:00–1:20 | Alert timeline + ask "Is everything okay?" | "The alert is persisted with its id, rule, and severity. The answer comes from the real backend, not from the page." |
| 1:20–1:45 | Ask "Is capture enabled?"; run scenario again | "Consent-first: if capture or consent is withdrawn, the gate blocks everything and nothing reaches the pipeline." |
| 1:45–2:10 | Privacy card | "Local-first, no cloud, no raw media — only alert metadata is stored." |
| 2:10–2:40 | Terminal: `npm test` / test counts scrolling | "329 C++ test cases, 76 Node tests, ASan/UBSan clean, fresh-build reproducible." |
| 2:40–3:00 | Close | "This is a simulated Alexa+ experience built on a real, tested backend. Thank you." |

**Recording tips:** 1080p, show the terminal command and the browser side by
side or in sequence, keep the cursor visible, and end on the test suite running
so judges see the engineering depth.

## v17 — Scenario catalogue and explainable alerts (2026-10-13)

The dashboard now includes a **scenario selector** and a **Scenario result**
card. For each run the page shows, from backend data only:

1. **What happened** — the selected scenario and its description.
2. **What was detected** — detector counts (`detected` / `not detected` /
   `inconclusive`) for the synthetic evidence.
3. **Fusion** — verdict, confidence and contributing inputs. An ambiguous or
   insufficient result is reported as `inconclusive`/`unavailable`, never as a
   detection.
4. **Why an alert was, or was not, generated** — the detection/fusion rule outcome
   and the number of duplicate alerts suppressed by the dedup/cooldown policy.
5. **What was persisted** — the count written this run and the durable total.
6. **Consent** — authorized vs denied acquisitions and whether enforcement held.

The **Alert timeline** is rendered from `GET /api/home_alerts` (real rows from the
local SQLite audit store) using `textContent` — the previous hardcoded
"synthetic simulator presence notice" string is gone.

> **Superseded by L-11.** The table below records the v17 (pre-L-11) behaviour,
> when the alert came from a raw-observation correlation rule and the detection
> stage did not influence it. See the "L-11 — Alerts are now detection-driven"
> section at the end of this file for the current, re-measured results.

| Scenario | Intent | Observed result (capture on) |
|---|---|---|
| `default` | backwards-compatible original | 10 observations, 1 alert delivered, 13 suppressed, 1 persisted |
| `normal` | routine activity, no qualifying concern | 1 observation, **no alert**; activity detected (2/2) |
| `unusual` | repeated activity triggers the rule | 10 observations, 1 alert delivered, 13 suppressed, 1 persisted |
| `inconclusive` | ambiguous evidence | 1 observation, **no alert**; detector 3 inconclusive, fusion `unavailable` |
| `consent-denied` | denied acquisition | 1 authorized + 1 denied, **no alert**, `enforcement_ok` true |

Endpoints: `GET /api/scenarios`, `GET /api/alerts`,
`GET /api/run-scenario[?scenario=<id>][&cycles=N]`. All loopback-only.

The assistant is a **fixed keyword matcher** grounded in the real status and the
last scenario result. It is not a language model and is not real Alexa+
intelligence.

## L-11 — Alerts are now detection-driven (2026-10-13)

The scenario table below is RE-MEASURED after L-11; the previous table described
the (now removed) raw-observation rule.

| Scenario | Intent | Measured result (capture on) |
|---|---|---|
| `normal` | routine activity below the detection baseline | 4 deliveries, 0 alerts. Detector: 2 not_detected; fusion `not_detected` (0.80) |
| `unusual` | sustained activity above the baseline | 10 deliveries, **1 alert** (`warning`), 9 suppressed. Detector: 5 detected; fusion `detected` (1.00) |
| `inconclusive` | ambiguous evidence | 3 deliveries, 0 alerts. Detector: 3 inconclusive; fusion `unavailable` |
| `consent-denied` | withdrawn consent | 1 authorized + 1 denied, 0 alerts; detection not run (denied path produced no observation) |
| `default` | backwards-compatible original | 10 deliveries, **2 alerts** (`warning` 0.92 + `info` 0.61), 8 suppressed |

Every alert shown carries rule `detection-fusion`, a severity derived from the
fused confidence, and an explanation built from the alert's own evidence
(contributing detector observations, fusion verdict, confidence). The timeline
also shows the fusion verdict and `synthetic` provenance per alert.

## L-12 — Statistical anomaly scenarios (2026-10-13)

Four catalogue entries exercise the statistical anomaly stage. All figures below
were measured from the freshly built bridge (capture on).

| Scenario | Intent | Measured result |
|---|---|---|
| `anomaly-warmup` | too few observations to trust a baseline | baseline **not ready** (2/4 samples), status `warm-up`, **0 anomalies** — warm-up never produces a false anomaly |
| `anomaly-normal` | routine activity at a stable level | baseline ready (mean 0.710, sd 0.00894), status `normal`, **0 anomalies** |
| `anomaly-deviation` | sustained unusually high activity | baseline ready, status `high`, **2 anomalies**, last z = 2.442 (threshold 2.0), with the detector's own reason ("value 0.99 is 2.44 sigma above baseline mean 0.75 …") |
| `anomaly-missing` | a deliberate gap past the expected interval | gap probe reports **`missing`** — "no observation for 4s (max_gap 3s)" |

The **Scenario result** card shows, for these scenarios: whether the baseline is
ready, the phase (warm-up/ready), the anomaly status (warm-up/normal/high/low/
missing), baseline mean and standard deviation, the last value and z-score, the
missing-observation check, and the detector's own explanation — plus an explicit
**"Anomaly vs alert"** row stating whether anomaly evidence is counted as alert
evidence for that run. Under the default configuration it is **advisory**.

Plainly: a statistical deviation is not a fall, an emergency or a danger claim.
Under the default configuration it does not raise an alert; the alert in these
scenarios comes from the activity detection path, and the relationship row says so.


## H-01s — Simulated critical impact scenario (2026-10-13)

`synthetic-critical-impact` is the tenth catalogue entry and the only one that
can produce a **critical** alert. Figures below are measured from the rebuilt
`hg_alexa_bridge` (Release), run 3× with identical results.

| Field | Measured value |
|-------|----------------|
| Synthetic media authorized | 12 |
| Events processed | 30 |
| Alerts generated | 23 |
| Alerts delivered | **2** (1 CRITICAL `synthetic-impact` + 1 INFO `detection-fusion`) |
| Duplicates suppressed | 21 |
| Alerts persisted (this run / total) | 2 / 2 |
| Impact classifier | 6 samples, **1 transient**, **1 confirmation** |
| Confirmed pattern | transient **0.95** → stillness **0.10** within **1 sample** |
| Anomaly layer | status `low`, 2 anomalies, `fused_into_alert` **false** (advisory) |
| Consent enforcement | `enforcement_ok` true |
| Scenario latency | ~0.15 s per run |

The classifier sees `0.10, 0.12, 0.11, 0.95, 0.10, 0.08`: sample 4 is a lone
transient (**inconclusive**, no alert), sample 5 supplies the stillness that
confirms the pattern, and sample 6 is inert. That is why the alert is delivered
once and then suppressed as a duplicate.

The dashboard shows the scenario result plus a red **SIMULATED critical impact**
panel containing the classifier, sample/transient/confirmation counts, the
confirmed pattern, the delivered-critical count, the classifier's own reason and
the backend disclaimer. Critical alerts render with a red severity badge; the
severity is also spelled out as text so colour is never the only signal. The
wording is fixed to "Simulated critical impact pattern. This demonstration does
not confirm a real fall or contact emergency services."

**Never** shown: a claim that a real person fell, that an ambulance was
contacted, or that emergency services were notified.


## v21 — API contract note and impact-panel wording (2026-10-13)

**Unknown scenario ids now answer `400 Bad Request`.** Selecting or requesting a
scenario that is not in the catalogue returns the same generic JSON error body as
before (`{"ok":false,"error":"unknown_scenario","scenario_id":"…"}`), but with a
`400` status instead of `200`. The dashboard already keys on `scenario.ok`, so it
behaves exactly as before and shows "The backend rejected the scenario: …".
Malformed ids are still rejected by the transport with `400` before the handler
runs. Existing scenario identifiers and every successful response are unchanged.

**Impact panel wording.** When the synthetic impact pattern is confirmed the
"SIMULATED critical impact pattern" panel now reads "Confirmed because: …" using
the reason from the sample that completed the pattern (for example
`simulated impact signature: transient 0.95 followed by stillness 0.1 within 1
sample(s)`), and the classifier's final-sample reason is labelled "Last sample:".
Previously the last-sample reason appeared directly under "Confirmed pattern",
which could read as if nothing had been confirmed.

**Degraded mode is honest.** If the bridge cannot open its audit database (for
example an unwritable directory) the dashboard's status card shows the backend as
unavailable; a scenario still runs and its detection/impact evidence is real, but
the page shows no persisted-alert counts rather than inventing them.
