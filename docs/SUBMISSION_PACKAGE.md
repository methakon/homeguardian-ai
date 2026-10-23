# HomeGuardian AI — Hackathon Submission Package

**Event:** Build, Ship, Shape: Amazon Developer Hackathon (AmazonAppDev 2026)
**Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server & C++ Home Safety Engine with an Alexa-Style Demonstration Client** (AmazonAppDev 2026)
**Deadline:** October 23, 2026, 12:00 PM PDT = **October 24, 2026, 12:30 AM IST**
**Repository:** https://github.com/methakon/homeguardian-ai (public)
**License:** Apache License 2.0 (see [LICENSE](../LICENSE) and [NOTICE](../NOTICE))

> **Honesty statement for judges.** The **Alexa+ track self-hosted MCP route**
> uses native C++ MCP 2025-11-25 over Streamable HTTP with two read-only tools.
> Separately, a functional **classic ASK hosted skill → AWS SQS → Oracle private
> worker → Android bridge → native C++ tools** lane exists. Voice replies are
> owner-attested; [independent mailbox/native verification](ALEXA_LIVE_PATH_VERIFICATION.md)
> records the proof and remaining reliability/deployed-code limitations.
> This is not production Alexa+ integration or certified monitoring. Detector,
> demo and regression inputs are synthetic. Video/publication and final
> submission remain unverified.

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
user explicitly consents, and no raw sensor media traverses AWS.** Aggregate
read-only status/alerts do cross AWS in the ASK lane. The system
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
  *metadata* is stored (id, severity, rule, timestamp), not raw sensor media.
  Separate explicit operator uploads may store media locally for simulated
  playback; this is not live camera capture or detection.
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
| Consent gate, guarded delivery, event pipeline, correlation, alert dedup, SQLite retention, native C++ read-only MCP tools, device registry, and functional classic ASK/SQS/private-worker/Android/native transport (voice owner-attested; mailbox/native independently verified) | Detector/demo/regression inputs, synthetic viewer and local Alexa-style presentation; explicit upload playback is simulated, not live capture/detection | Production Alexa+, LWA linking, real local ML inference, Ring, physical sensor-to-alert acceptance; optional private owner-controlled device-list live feed pending verification |

**Testing.** Historical candidate counts are not current HEAD acceptance. See
[live-path verification](ALEXA_LIVE_PATH_VERIFICATION.md),
[Android physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md), and
[current status](STATUS.md) for scoped evidence and known limitations. No new
runtime test results are asserted by this documentation update.

---

## 2. Three-minute demo script

For the primary MCP recording run `bash scripts/verify_synthetic_mcp_demo.sh --serve`
(see [demonstration guide](DEVPOST_DEMONSTRATION_GUIDE.md)). The following is the
alternative standalone synthetic `bash demo.sh` storyboard. Keep the
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
| **2:05–2:25** | Privacy card and revision-scoped acceptance records | "This detector demo is synthetic. The separate working ASK lane sends read-only aggregate status and alerts through AWS, not raw sensor media. Sensor retention stores metadata; explicit uploaded media is stored locally for simulated playback." |
| **2:25–2:50** | Back to the UI, ask **"What can you do?"** | "It reports home status, alerts, and consent state, and runs synthetic scenarios — read-only, so it can never enable a sensor or change configuration." |
| **2:50–3:00** | Close on the repo README | "A simulated Alexa+ experience built on a real, tested, privacy-first backend. Thank you." |

**Do not say:** production Alexa+, certified monitoring, physical sensor-to-alert
acceptance, or real-world detection accuracy. The self-hosted MCP server is real;
the classic ASK hosted skill is also real but separate. Distinguish owner-attested
voice replies from independent mailbox/native evidence. Never show private camera
streams publicly; the optional private live feed remains pending verification.

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
- *Purpose:* the C++ unit/integration suite (counts vary by revision).
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
  **not** claimed is production Alexa+ or LWA account linking. The separate classic
  ASK hosted skill uses AWS SQS and the existing private worker/native path.

**Used:** classic ASK hosted skill/Lambda and AWS SQS for aggregate read-only
queries, with an Oracle private worker and Android/native tools. **Not used:**
Amazon partner-only Alexa+ MCP Toolkit, Category SDK, `@alexa-ai/cli`, Alexa+ Web
Simulator, or Login with Amazon. AWS Builder and Open Source mini-challenges
remain **not claimed**; using AWS alone is not a mini-challenge acceptance verdict.

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
| Current HEAD release-readiness | Review scoped acceptance records and limitations; historical passes are not a current all-green verdict | user |
| Primary track: **Alexa+ (self-hosted MCP route)** | ⬜ **USER** — select on Devpost | user |
| Project description (use §1 above) | ⬜ **USER** — paste into Devpost | user |
| Screenshots | ⬜ **USER** — capture the running dashboard | user |
| Demo video <3 min, public on YouTube/Vimeo | ⬜ **USER** — record with §2, check with §3 | user |
| Product feedback (use §4 above) | ⬜ **USER** — paste into Devpost | user |
| Explain prior work / hackathon-window updates | ⬜ **USER** — verify dates and scope from repository history before publishing | user |
| Final Devpost submission + verify the rendered page | ⬜ **USER** — submit and confirm it renders | user |

**Not claimed:** AWS Builder mini-challenge, Open Source mini-challenge,
production Alexa+, certified monitoring, Ring integration, physical sensor-to-alert
acceptance, or real-world detection accuracy. The native MCP server and classic
ASK lane are real; neither establishes production Alexa+ integration.
