# HomeGuardian AI — Hackathon Submission Package

**Event:** Build, Ship, Shape: Amazon Developer Hackathon (AmazonAppDev 2026)
**Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server & C++ Home Safety Engine with an Alexa-Style Demonstration Client** (AmazonAppDev 2026)
**Deadline:** October 23, 2026, 12:00 PM PDT = **October 24, 2026, 12:30 AM IST**
**Repository:** https://github.com/methakon/homeguardian-ai (public)
**License:** Apache License 2.0 (see [LICENSE](../LICENSE) and [NOTICE](../NOTICE))

> **Honesty statement for judges.** This is a *project-built demonstration* of an
> Alexa-style conversational experience. It is **not** a real Alexa+ device
> integration and there is **no live Amazon identity**. An **authoritative native C++ MCP server is
> implemented and delivered** (spec 2025-11-25, Streamable HTTP, two read-only tools) and runs
> locally on loopback; it is **not** an Amazon cloud runtime and is not connected to
> Amazon cloud infrastructure. All household observations are **synthetic** — no physical camera
> or microphone is ever activated. Every number and alert shown in the demo
> comes from the real C++ backend running locally.

---

## 1. Project description (judge-facing)

**The problem.** Families want to know their home is safe — that a fall was
noticed, that an elderly relative is moving around normally, that something
unusual happened while they were out — without handing a continuous audio and
video feed of their private life to an opaque cloud service. Existing products
force a bad trade: dumb cameras that record everything and understand nothing,
or "smart" systems that stream a household's most sensitive data to someone
else's servers with no transparency and no real consent.

**What HomeGuardian AI is.** A privacy-first, local-first home intelligence
platform built around one non-negotiable rule: **nothing is sensed unless the
user explicitly consents, and nothing raw ever leaves the machine.** The system
ingests household observations, validates them, correlates them across time,
and raises explainable alerts — with the consent gate as a hard delivery
boundary rather than a settings checkbox.

**How it is built.** A C++20 core with a deterministic, in-process sensor
simulator that exercises the real consent, lifecycle, delivery, and bounded-
buffer machinery on the shared code paths:

- **Consent gate + guarded delivery boundary.** Media is delivered only while
  capture is enabled *and* consent is granted. Withdraw either and the gate
  blocks all delivery — nothing reaches the event pipeline. This is
  demonstrated live in the demo.
- **Event pipeline + time-window correlation.** Observations are validated
  (schema, timestamps, confidence) and correlated — a rule fires when a
  meaningful pattern appears within a window, not on every single sample.
- **Alert deduplication / cooldown policy.** A repeated firing becomes one
  notification per active condition, with re-arm when the condition clears.
  Unrelated alerts are never suppressed against each other.
- **Durable, privacy-safe persistence.** SQLite with schema migration,
  transactional writes, age-based retention, and recovery. Only alert
  *metadata* is stored (id, severity, rule, timestamp) — never raw audio,
  video, or biometric data.
- **Bounded everything.** Queues, event history, dedup state, and log volume
  are all bounded; the runtime cannot grow without limit.

**The demo.** A simulated Alexa+ conversational web experience on top of that
backend. You ask it household questions in plain language, run a synthetic
observation scenario, and watch real events flow through validation,
correlation, alert generation, deduplication, and durable persistence — then
see the consent gate refuse to deliver anything once capture is turned off.

**Implemented vs simulated vs planned** — stated plainly:

| Implemented and tested | Simulated (this demo) | Planned (not built) |
|---|---|---|
| Consent gate, guarded delivery, event pipeline, correlation, alert dedup, SQLite persistence + migration/retention/recovery, hardened credential store, read-only integration contract, loopback bridge/HTTP adapter, synthetic detection + fusion stage (activity-energy detector, explainable anomaly detection, confidence-aware fusion — all synthetic), and a **detection-driven alert rule** (an alert is raised only when the fused detector evidence qualifies; raw observations never alert) | The Alexa+ conversational web experience; all observations are synthetic | Real camera/mic capture (V4L2/ALSA/PipeWire), real local ML inference (face/pose/lip/voice), an **Alexa+/Amazon** MCP integration (account linking, live Amazon identity — the local MCP server delivered in HG-MCP-01 is not one), Ring integration |

**Testing.** 527 C++ test cases (38,926 assertions), 161 UI/dashboard tests, 73 MCP protocol tests, 21 Java E2E bridge tests, 46 outbound bridge tests, ASan/UBSan clean, and a reproducible out-of-source build — all green at submission.

---

## 2. Three-minute demo script

Run `bash demo.sh` first (see [docs/DEMO_WEB.md](DEMO_WEB.md)). Keep the
terminal and browser side by side; have `docs/DEMO_WEB.md` open for the
expected outputs.

| Time | Screen / action | Narration |
|---|---|---|
| **0:00–0:15** | Terminal: `bash demo.sh`; browser opens to the URL | "HomeGuardian AI is a privacy-first home safety system. This is a simulated Alexa+ experience that runs entirely on my machine." |
| **0:15–0:35** | The web UI: the yellow banner, then the status card | "Note the banner — this is a simulation. No physical camera or microphone is ever activated. Status is Running, capture is enabled for a synthetic sensor." |
| **0:35–1:00** | Pick **Unusual activity** in the scenario selector, click **▶ Run scenario** | "Synthetic observations are acquired under consent and a detector classifies them — all detected here. The fusion result agrees with confidence 1.0, and the detection rule raises the alert: the alert exists because the evidence qualified, not because a counter ticked. Ten firings, but the cooldown policy delivers one and suppresses nine duplicates." |
| **1:00–1:20** | Alert timeline, then ask **"Is everything okay?"** | "The alert is persisted with its id, rule, and severity. And the answer comes from the real backend, not from the web page." |
| **1:20–1:45** | Ask **"Is capture enabled?"**; click **Run synthetic scenario** again | "Consent-first. If capture or consent is withdrawn, the gate blocks everything and nothing reaches the pipeline — I can demonstrate that with the capture-off config." |
| **1:45–2:05** | Run the scenario against `config/homeguardian.json` (capture off) — show `events_processed: 0` | "Zero events. The consent gate refused every observation. That fail-closed behaviour is the core privacy guarantee." |
| **2:05–2:25** | The privacy card; then the terminal running `npm test` / the C++ suite | "Local-first, no cloud, no raw media — only alert metadata is stored. And it is tested: 527 C++ cases (38,926 assertions), full regression suites, sanitizers clean, reproducible build." |
| **2:25–2:50** | Back to the UI, ask **"What can you do?"** | "It reports home status, alerts, and consent state, and runs synthetic scenarios — read-only, so it can never enable a sensor or change configuration." |
| **2:50–3:00** | Close on the repo README | "A simulated Alexa+ experience built on a real, tested, privacy-first backend. Thank you." |

**Do not say:** that this is real Alexa+, that the local MCP server is an Alexa+
runtime or a deployed Amazon service, that any physical sensor was used, or that
detection accuracy was measured on real data. (An MCP server does exist and may be
demonstrated — describe it exactly as it is: local, loopback, read-only.)

---

## 3. Recording checklist

- **Resolution / format:** 1920×1080, 16:9, H.264 MP4.
- **Length:** under 3:00 — target 2:45. Time each section against the script.
- **Before recording:**
  - [ ] `bash demo.sh` runs cleanly and the URL loads.
  - [ ] Start from a **clean demo database** (stop the demo, re-run `demo.sh` —
    it uses a fresh temp dir, so the timeline starts empty).
  - [ ] Terminal: clear scrollback, no secrets/credentials/tokens visible,
    no unrelated windows or notifications. Set a slightly larger font.
  - [ ] Browser: zoom ~110%, hide bookmarks/extensions, close other tabs.
  - [ ] Disable desktop notifications; mute non-essential apps.
- **During:** narrate from the script; pause briefly after each state change so
  the numbers are readable; keep the cursor movements deliberate.
- **If the demo fails mid-recording:** stop, fix offline, re-record the affected
  section (do not narrate over an error). Quick recovery:
  - *"Backend unavailable" / bridge not starting* → the bridge binary is stale;
    rebuild it: `cmake --build hg_build --target hg_alexa_bridge`, then re-run
    `bash demo.sh`.
  - *Port in use* → set a free port: `HG_DEMO_PORT=8199 bash demo.sh`.
  - *Scenario shows 0 events* → you are on the capture-off config; use
    `config/homeguardian.demo.json` (the `demo.sh` default).
  - Full troubleshooting table: `docs/DEMO_WEB.md` §4.
- **After:**
  - [ ] Watch the full playback — confirm audio is clear and every number shown
    matches the expected output in `docs/DEMO_WEB.md` (scenario: **15 events
    processed, 14 alerts generated, 1 delivered, 13 suppressed, 1 persisted**;
    detection stage: 4 detected / 1 inconclusive, fusion verdict `detected`).
  - [ ] Confirm the video is **publicly accessible** on YouTube or Vimeo and is
    under three minutes.
  - [ ] Copy the final video URL into the Devpost submission.

---

## 4. Product feedback drafts

Truthful answers based on how this project was actually built. Adjust tone as
desired; the substance reflects real experience.

**C++20 / g++ / CMake**
- *Purpose:* the core runtime — event model, consent gate, pipeline,
  persistence.
- *What worked:* expressive and fast; CMake's FetchContent made dependency
  setup reproducible.
- *What needs improvement:* nothing specific for this project.
- *Onboarding:* good.
- *Would use again:* yes.

**spdlog (logging)**
- *Purpose:* structured logging across the runtime.
- *What worked:* simple API, multiple sinks (console + file) with per-sink
  levels.
- *What needs improvement:* the console sink writes to stdout, which
  interleaves with a machine-readable protocol on the same stream — I had to
  set the level to `critical` in the bridge so stdout stayed clean NDJSON. A
  stderr-default console sink (or an explicit stream choice) would avoid that.
- *Onboarding:* good.
- *Would use again:* yes.

**nlohmann/json**
- *Purpose:* JSON parsing/serialization for the contract, credentials, and
  event payloads.
- *What worked:* ergonomic API, header-only, good error reporting.
- *What needs improvement:* nothing specific.
- *Onboarding:* excellent.
- *Would use again:* yes.

**SQLite (amalgamation)**
- *Purpose:* durable alert-metadata persistence with migration and retention.
- *What worked:* embedded, zero-config, WAL mode and busy timeouts handled
  concurrent access cleanly; transactional migrations rolled back correctly on
  failure in testing.
- *What needs improvement:* nothing specific.
- *Onboarding:* good.
- *Would use again:* yes.

**Catch2**
- *Purpose:* the C++ unit/integration suite (329 cases).
- *What worked:* tag filters made focused runs easy; SECTIONs kept fixtures
  readable.
- *What needs improvement:* nothing specific.
- *Onboarding:* good.
- *Would use again:* yes.

**Node.js (standard library `http`, `child_process`, `fs`)**
- *Purpose:* the simulated Alexa+ web demo and the Alexa skill prototype.
- *What worked:* the stdlib alone was enough — no framework needed for the
  loopback demo server and the child-process bridge.
- *What needs improvement:* the HTTP server de-duplicates the `Authorization`
  header at parse time (keeping the first), so duplicate-header rejection has
  to be handled by strict single-token parsing rather than header inspection.
- *Onboarding:* excellent.
- *Would use again:* yes.

**Model Context Protocol specification (2025-11-25)**
- *Purpose:* studied to decide the Alexa+ track route.
- *What worked:* the specification is clear and well-organised; the Streamable
  HTTP transport requirements are unambiguous.
- *What needs improvement:* nothing specific.
- *Note:* the specification was used for real — a local **MCP server** was built
  against it (spec 2025-11-25, Streamable HTTP, read-only tools; HG-MCP-01). What is
  **not** implemented is the Amazon side: no Alexa+ runtime, no account linking, no
  live Amazon identity.

**Not used (stated for honesty):** no AWS services, no Amazon partner-only
tools (Alexa+ MCP Toolkit, Category SDK, `@alexa-ai/cli`, Alexa+ Web
Simulator), no Login with Amazon, and no other Amazon APIs were used. The AWS
Builder and Open Source mini-challenges are therefore **not claimed**.

---

## 5. Submission checklist

**Deadline:** October 23, 2026, 12:00 PM PDT = **October 24, 2026, 00:30 IST**.

| Item | Status | Owner |
|---|---|---|
| Repository public and accessible | ✅ DONE — https://github.com/methakon/homeguardian-ai | — |
| **License: Apache-2.0** `LICENSE` (sha256-verified vs apache.org) + `NOTICE` | ✅ DONE | — |
| README accurate (no Amazon/Alexa+/Ring/sensor overclaims; Native, no Node) | ✅ DONE | — |
| Local MCP server (spec 2025-11-25, Streamable HTTP, 2 read-only tools) | ✅ DONE — HG-MCP-01 | — |
| Runnable demo from a clean clone (`bash demo.sh`, Node-free) | ✅ DONE + verified | — |
| Demo guide (`docs/DEMO_WEB.md`) | ✅ DONE | — |
| Native low-footprint C++ implementation (1 process; ~6.4 MB idle RSS) | ✅ DONE | — |
| Consent-first privacy architecture + privacy audit (`enforcement_ok`) | ✅ DONE | — |
| Release-readiness verified: Release build, C++ suite, CTest, ASan/UBSan, `[nativehttp]` adversarial, loopback-only, persistence/restart, capture-off fail-closed, graceful shutdown | ✅ DONE (evidence in this repo's docs) | — |
| Primary track: **Alexa+ (Route B — simulated web experience)** | ⬜ **USER** — select on Devpost | user |
| Project description (use §1 above) | ⬜ **USER** — paste into Devpost | user |
| Screenshots | ⬜ **USER** — capture the running dashboard | user |
| Demo video <3 min, public on YouTube/Vimeo | ⬜ **USER** — record with §2, check with §3 | user |
| Product feedback (use §4 above) | ⬜ **USER** — paste into Devpost | user |
| Explain prior work / hackathon-window updates | ⬜ **USER** — 46 commits Oct 9–13 2026, all in-window | user |
| Final Devpost submission + verify the rendered page | ⬜ **USER** — submit and confirm it renders | user |

**Not claimed:** AWS Builder mini-challenge, Open Source mini-challenge, live
Alexa+ integration, MCP server, Ring integration, physical sensor capture, and
real-world detection accuracy. The simulated Alexa+ experience is **not**
equivalent to production Alexa+ integration.
