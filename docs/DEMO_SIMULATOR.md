# HomeGuardian AI — Simulator Demonstration (Runtime Hardening v1)

A reproducible, simulator-only demonstration of the running application. It
requires **no physical sensors, no elevated privileges, no network access, no
credentials, and no AWS resources**. All media is deterministic synthetic PCM;
no camera, microphone, or speaker is opened.

## Prerequisites

- A built `homeguardian` executable (see `docs/DEVELOPMENT.md`):
  ```bash
  mkdir -p build && cd build
  cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
  make -j4 homeguardian
  ```
  (On first configure, CMake fetches spdlog + nlohmann/json from GitHub once.)
- The shipped config `config/homeguardian.json` (capture **disabled** by default).

All commands below run from the repository root. The app runs a bounded
simulator demonstration and then idles in its main loop; send `SIGTERM`
(Ctrl-C) or let the `timeout` wrapper stop it. Output is logged to stdout.

---

## 1. Default capture-disabled startup — no media, no alerts

```bash
timeout 3 ./build/homeguardian config/homeguardian.json
```

Expected output pattern:
```
[info] Simulator mode: capture_enabled=false consumers=2 state=Starting
[info] Simulator idle: capture disabled; no media delivered.
[info] Entering main loop...
[info] Main loop exited cleanly. final_state=Stopped
```

Observe: **no** `Alert generated`, **no** media delivery, clean `final_state=Stopped`.

## 2–4. Explicitly enabled synthetic mode — observations, alert, dedup

Create a temporary config with capture enabled (do **not** edit the tracked
config):

```bash
sed 's/"media_capture_enabled": false/"media_capture_enabled": true/' \
    config/homeguardian.json > /tmp/hg_on.json
timeout 4 ./build/homeguardian /tmp/hg_on.json
rm -f /tmp/hg_on.json
```

Expected output pattern:
```
[info]  Simulator mode: capture_enabled=true consumers=2 state=Starting
[info]  Simulator cycle 0: delivered=2 blocked=0 speaker_played=2 pipeline_history=2 alerts_delivered=1 alerts_suppressed=0
...
[warn] Alert generated: synthetic simulator presence notice
[info] Alert suppressed (dedup/cooldown): synthetic simulator presence notice
...
[info] Simulator cycles complete. speaker_played=10 queue=0 pipeline_history=10 alerts_delivered=1 alerts_suppressed=8
[info] Main loop exited cleanly. final_state=Stopped
```

Observe:
- **Authorized observations enter the pipeline** (`pipeline_history` grows
  2 → 10 across 5 cycles).
- **A controlled correlation scenario generates the expected synthetic alert**
  (`Alert generated: synthetic simulator presence notice`) once ≥2 observations
  are in the 60s window.
- **Repeated matching observations follow the dedup/cooldown policy**: the
  first firing is delivered; subsequent firings of the same active condition
  within the 60s cooldown are suppressed (`alerts_delivered=1`,
  `alerts_suppressed=8` over 10 observations). Unrelated alerts (different
  rule/severity) would be delivered independently — see `AlertPolicy`.

## 5. Revoking consent blocks subsequent observations

This is exercised by the automated tests (`[lifecycle]`, `[event2alert]`): when
a consumer's consent is withdrawn, subsequent `tick()` cycles deliver nothing to
that consumer and no observation enters the pipeline. The running-app demo
above grants consent for the whole bounded run; consent withdrawal mid-run is
covered deterministically by:

```bash
timeout 120 ./build/homeguardian_tests "[event2alert],[lifecycle]"
```

## 6. Shutdown — bounded queues, consistent final status

Every run above ends with `final_state=Stopped`, `queue=0` (speaker queue
drained/cleared), and bounded `pipeline_history`. The shutdown path is
idempotent and covered by `[lifecycle]` and `[hardening]` tests.

## 7. Durable audit + observability

The running app logs a privacy-safe status summary when entering the main loop
and at clean shutdown, e.g.:

```
state=Running capture=on persist=ok media_auth=10 media_denied=0 events_proc=10
events_rej=0 events_fail=0 alerts_gen=9 alerts_deliv=1 alerts_supp=8
persist_fail=0 spk_depth=0 spk_overflow=0 spk_played=10 startups=1
clean_shutdowns=1 recoveries=0
```

Delivered alerts are persisted (metadata only) to `homeguardian_audit.db` via
the existing SQLite layer. **Restart distinguishability:** run the capture-on
demo twice against the same database — the second run logs
`Persistence ready. historical_alerts=1`, showing prior-process alerts are
distinguishable from current-process ones.

```bash
# First run
timeout 4 ./build/homeguardian /tmp/hg_on.json   # -> historical_alerts=0
# Second run (same DB)
timeout 4 ./build/homeguardian /tmp/hg_on.json   # -> historical_alerts=1
```

To start fresh, remove the audit DB (`rm -f homeguardian_audit.db*`). No raw
media is persisted; only alert metadata.

**Retention:** with the default `alert_retention_days=30`, the app prunes alerts
older than 30 days once at startup and at most once per 10 minutes while
running (bounded — never per tick). Set `alert_retention_days: 0` to keep
forever. Pruning removes only expired rows; newer records are preserved.

## 8. Alexa integration contract (local, not deployed)

The application exposes a versioned, authenticated, READ-ONLY contract for a
future Alexa adapter (`src/core/AlexaContract.*`). It is exercised entirely
locally with mock clients — no endpoint, no AWS, no credentials:

```bash
# C++ contract tests (fail-closed auth, read-only ops, no mutation)
./homeguardian_tests "[alexa]"

# Node adapter boundary tests (mirror the same semantics)
cd alexa_skill && npm test
```

The contract accepts only `status` and `alert_summary` reads. Missing/invalid/
expired credentials fail closed (`unauthorized`); an authenticated principal
without permission for the operation gets `forbidden`; a degraded app returns
`unavailable`. It cannot enable capture, grant consent, change config, or
activate a sensor.

**This is NOT a live Alexa integration.** A real end-to-end test requires a
deployed, authenticated endpoint and is documented as pending.

---

## Alert policy (summary)

`AlertPolicy` turns a repeated rule firing into a single notification per active
condition:

- **Identity** = `rule_name` + `severity`. Different keys are independent.
- **First occurrence** is always delivered.
- **Cooldown** (default 60s, configurable): repeat firings for the same active
  condition within cooldown are suppressed and counted.
- **Re-arm**: after cooldown expires, the next firing re-notifies; if the
  condition clears (a cycle produces no alert for that key), the next firing is
  a fresh condition and is delivered immediately.
- **Bounded**: retained dedup state and history are capped (oldest evicted).
- **Clock**: injectable; a backward-moving clock cannot extend a cooldown.

This is a **notification** policy, not a safety detector. A suppressed duplicate
is still an active condition. The alert is a **synthetic presence notice**, never
a real-world danger detection.

## Limitations

- Simulator-only: does not open V4L2/ALSA/PipeWire/Android hardware and does not
  prove real-device behavior or real OS permission semantics.
- The demonstration runs a fixed 5-cycle bounded scenario, not a persistent
  service.
