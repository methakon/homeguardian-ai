# HomeGuardian AI — v21 Release Candidate Verification Report

Date: 2026-10-13
Branch: `h04/native-http-low-footprint`
Scope: **final integration and stability verification** of the existing native
C++ application. No new sensing backend, no real fall detection, no emergency
notification, no Ring/live-Alexa+ integration, no MCP, no dashboard redesign.

> **Simulation only.** HomeGuardian AI is a simulator-first demonstration. It
> uses no physical camera or microphone, performs no medical inference, detects
> no real fall, and contacts no emergency service. H-01 (real pose-based fall
> detection), H-02 (emergency workflow) and G-02 (external notifications) are
> **NOT implemented**.

---

## 1. Verified repository state

| Item | Value |
|------|-------|
| Branch | `h04/native-http-low-footprint` |
| Starting HEAD | `ce368fbae52e34203536c63aedbc730fa6b46e88` (= origin; 0 unpushed) |
| `main` | `7c3c32307092f61d80a6048d58442d994dbec32c` — untouched, never merged, never force-pushed |
| Working tree | clean apart from the preserved untracked `example/` |
| Starting commit recorded | yes (above) |

## 2. Fresh build of every shipped target (mandatory safeguard)

A fresh out-of-source Release build was produced in a directory that is **not** a
child of the source tree, so relative-path bugs cannot hide:

```bash
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=<repo>/hg_build/_deps/spdlog-src \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<repo>/hg_build/_deps/nlohmann_json-src <repo>
make -j4 homeguardian homeguardian_tests hg_alexa_bridge
```

Result: configure 0, build 0, **0 errors**, for all three shipped executables
(`homeguardian`, `homeguardian_tests`, `hg_alexa_bridge`). Every runtime figure in
this report comes from those binaries, executed after the last source change.

## 3. Scenario catalogue — all 10 scenarios verified

Every scenario was exercised against the running bridge and its **full**
integration fields recorded (`/home/swarna-sekhar-dhar/.hermes/cache/scratch/hg_v21/scenarios.json`).
Expectations are asserted programmatically: `10/10 pass`.

| # | Identifier | Consent/capture prerequisites | Auth obs | Events | Detection | Anomaly | Alerts gen/deliv/supp | Severity (delivered) | Persisted | Audit |
|---|-----------|-------------------------------|---------|--------|-----------|---------|----------------------|---------------------|-----------|-------|
| 1 | `default` | grants assistant+sensor; capture on | 10 | 15 | detected (0.92) | — | 10/2/8 | warning + info | 2 | enforcement_ok |
| 2 | `normal` | grants; capture on | 4 | 6 | not_detected | — | 0/0/0 | — | 0 | enforcement_ok |
| 3 | `unusual` | grants; capture on | 10 | 15 | detected | — | 10/1/9 | warning | 1 | enforcement_ok |
| 4 | `inconclusive` | grants; capture on | 3 | 6 | unavailable/inconclusive | — | 0/0/0 | — | 0 | enforcement_ok |
| 5 | `consent-denied` | delivery explicitly denied | 1 denied | 1 | not run | — | 0/0/0 | — | 0 | enforcement_ok |
| 6 | `anomaly-warmup` | grants; capture on | 6 | 12 | detected | warm-up (2/4), 0 anomalies | 6/1/5 | info | 1 | enforcement_ok |
| 7 | `anomaly-normal` | grants; capture on | 12 | 24 | detected | normal (mean 0.710), 0 anomalies | 18/1/17 | info | 1 | enforcement_ok |
| 8 | `anomaly-deviation` | grants; capture on | 16 | 32 | detected | high, 2 anomalies (z 2.44) | 26/1/25 | info | 1 | enforcement_ok |
| 9 | `anomaly-missing` | grants; capture on; deliberate gap | 6 | 13 | detected | missing (no obs for 4s) | 7/1/6 | info | 1 | enforcement_ok |
| 10 | `synthetic-critical-impact` | grants; capture on | 12 | 30 | detected (0.53) | low, 2 anomalies, **advisory** | 23/2/21 | **critical** + info | 2 | enforcement_ok |

Exactly one scenario can produce a critical alert, and it does so only from a
**confirmed** synthetic impact pattern (transient `0.95` → stillness `0.10`
within 1 sample, 2 supporting observations). The remaining nine keep their
pre-v21 outcomes — the L-11 and L-12 behaviour is unchanged.

## 4. End-to-end pipeline verification

```text
authorized synthetic observation → ConsentGate → Observation validation
  → activity detector / anomaly monitor / impact classifier
  → Pipeline (time-window correlation) → highest severity wins
  → AlertPolicy (dedup + cooldown) → SQLite (WAL, synchronous=FULL)
  → native HTTP API → dashboard + simulated assistant
```

Verified in a **real browser** (CDP) and over the API:

- a delivered alert carries backend evidence (fusion verdict/confidence for
  activity; transient/stillness/pattern for the impact rule) — no UI-invented text;
- critical severity appears only for the confirmed impact pattern;
- `normal`/`inconclusive` produce no danger claim; `consent-denied` produces none;
- anomaly evidence is reported, audited and persisted but **advisory**: it does
  not vote, and the dashboard states that from live configuration;
- independent rules do not suppress each other (the critical scenario delivers
  `critical` **and** `info` — different policy keys);
- critical outranks info/warning when both qualify for the same window;
- 5 consecutive critical runs are byte-identical (deterministic), and an
  alternating normal → anomaly → critical → … sequence shows no state leak;
- 0 JavaScript errors during the browser session; no page text claims a real
  fall, an ambulance, a dispatch or live Alexa+/Ring.

## 5. Native HTTP API

| Route | Method | Result |
|-------|--------|--------|
| `/`, `/index.html` | GET | 200, embedded dashboard (CSP, nosniff, DENY, no-referrer) |
| `/api/health` | GET | 200 `{"ok":true,"status":"up","simulated":true}` |
| `/api/status` | GET | 200, or `{"error":"unavailable","ok":false}` when the audit DB is unusable (degraded) |
| `/api/alerts` | GET | 200, per-alert non-sensitive detail from the durable store |
| `/api/run-scenario?scenario=<id>[&cycles=N]` | POST | 200 on success (requires valid session & CSRF token); **400** for invalid scenario; **405** on GET |

Adversarial matrix: **51/51 pass** — bad/missing/duplicate/non-loopback `Host`
(403/400), cross-site `Origin` (403), non-GET methods (405), `Authorization` /
`Content-Length` / `Transfer-Encoding` / `Expect` / obs-fold / body-on-GET (400),
unknown or duplicate query keys and malformed `scenario`/`cycles` (400), oversized
header block (413), traversal and unknown paths (404), truncated and partial
requests (no crash, no answer), an oversized flood followed by a valid request
(200). The interface remains **loopback-only and unauthenticated by design**
(this hand-written demo server is not claimed to be production-ready).

### Defect fixed — unknown scenario returned 200

*Expected:* an invalid value for a supported parameter is `400 Bad Request`.
*Observed:* `200 OK` with `{"ok":false,"error":"unknown_scenario"}`.
*Root cause:* the transport hard-coded `response(200, …)` around the handler, so
the handler had no way to report a business status.
*Fix:* the handler now owns the status (`Handler(req, int& status)`); the
transport still owns framing/header/query validation and still answers 413/403/405/404/400
on its own. The JSON body shape is unchanged (`ok:false`, `error`, `scenario_id`),
so existing clients — the dashboard keys on `scenario.ok` — keep working.
*Regression:* `[v21]` transport tests assert both the status line and the JSON,
plus that a handler status can never override transport validation.
This is the only client-visible contract change in v21; it is intentional.

## 6. Dashboard verification

All 10 scenarios are listed and correctly named; the selector is labelled
(`aria-label="Scenario"`) and every control is a native `select`/`button`, so the
page is keyboard-operable. Severity is shown as **text and colour** (badges
`critical`/`warning`/`info`; a critical card also gets a red left border). The
result panel renders only backend-derived values; synthetic provenance, consent
status and persistence counts come from the API. Rendered content uses
`textContent`/`createElement` — the served dashboard contains **zero** `innerHTML`
uses. The simulated assistant is a fixed keyword matcher (documented as not a
language model): known questions answer from live backend state, and an unmatched
question gets a grounded fallback ("I can help with the home status, alerts,
capture state, and the last scenario. Right now HomeGuardian is Running with N
alerts on record.").

### Defect fixed — impact panel could be misread

*Observed:* with a confirmed pattern, the panel printed the **last sample's**
reason ("no impact transient") directly under "Confirmed pattern … 1
confirmation", which reads as if nothing had been confirmed.
*Root cause:* the snapshot exposed only the classifier's current state; the
confirming sample's reason was discarded.
*Fix:* the run now records `confirmed_reason` (the reason of the sample that
confirmed the pattern) and the panel shows "Confirmed because: …" when a pattern
was confirmed, labelling the final line as "Last sample: …" otherwise.
*Regression:* `[h01s]` asserts `confirmed_reason` is populated and names the
transient/stillness values.

## 7. Privacy and security regression

| Check | Result |
|-------|--------|
| Capture disabled | 0 authorized observations, 0 events, 0 alerts, no detection/anomaly/impact block, `enforcement_ok` true |
| Consent denied / withdrawn | delivery denied, no alert, audit intact |
| Consent expired / ambiguous | denied (`expired`; conflicting grants rejected) — `[h01s]` tests |
| No unauthorized observation in the pipeline | confirmed (capture-off scenario yields nothing) |
| Privacy audit `enforcement_ok` | true for all 10 scenarios |
| Raw media / biometrics in SQLite or API | none: `alerts` has no media column; a raw byte scan of the DB finds no media-like tokens |
| Access-control / input-validation boundaries | unchanged; 51/51 adversarial checks pass; loopback-only bind |

No authentication or consent path was weakened anywhere in this milestone.

## 8. Persistence, recovery and lifecycle

- `PRAGMA journal_mode` = **wal**; `PRAGMA synchronous` = **2 (FULL)** — read back
  from the live database; no code path lowers it and no build flag overrides it.
- Alert rows carry the expected metadata (`severity`, `rule_name`, `details` with
  the fusion or impact evidence, `detected_at` in ms) and no media.
- Restart recovery: alerts survive a SIGTERM restart and are re-read through the
  API with their evidence intact.
- No duplicate persistence: the store grows by exactly the number of delivered
  alerts per run (measured 42 → 44 for a run delivering 2).
- Alert-policy dedup/cooldown state is per process (documented); durable history
  is independent of it.
- Resource stability over 15 mixed runs: RSS 7,440 → 7,440 kB, fds 8 → 8,
  threads 1 → 1; 12 s idle: unchanged.
- Graceful shutdown on SIGTERM (bridge and standalone); no orphaned sockets or
  processes.

### Defect fixed — the bridge never applied retention to its own alert store

*Expected:* the configured `alert_retention_days` bounds the durable store.
*Observed:* an alert seeded 40 days old survived a restart; the store could grow
without bound across runs.
*Root cause:* only the application's own DB (`homeguardian_audit.db`) was pruned;
the bridge writes to its own `homeguardian_bridge_audit.db` and never called the
retention helpers with the config it was handed.
*Fix:* the bridge now prunes its store once at startup and, at most once per
10 minutes, after a scenario write — the same bounded policy the application uses.
`alert_retention_days = 0` still means keep-forever. Durability is untouched.
*Regression:* `scripts/verify_retention.sh` (seed an out-of-window row, restart,
assert it is pruned, the in-window row survives, the API total follows, and
scenarios still run). 8/8 checks pass.

### Defect fixed — an unwritable working directory aborted the binaries

*Expected:* if the working directory is not writable, start degraded and keep the
fail-closed guarantees.
*Observed:* the process died with **SIGABRT (exit 134)** — `terminate called after
throwing an instance of 'spdlog::spdlog_ex': Failed opening file
homeguardian.log for writing: Permission denied`.
*Root cause:* `Logger::initialize` created the file sink unguarded; the exception
escaped `main()`.
*Fix:* the file sink is now best-effort — on failure the logger logs to the
console only and reports the reason; logging is never fatal. Registration is also
guarded.
*Regression:* `[logger]` test that runs `Logger::initialize` with an
unwritable target (deterministically, by placing a directory where the log file
must be created) and asserts no throw plus a working logger.
*Live:* with a read-only data directory the bridge now stays up and reports
`{"error":"unavailable","ok":false}`; the scenario still produces real detection
evidence but **no** durable persistence is claimed.

A corrupted audit DB is likewise handled: the bridge keeps serving, the read and
scenario routes fail closed, and the process stays alive.

### Defect fixed — test fixtures were not safe under concurrent runs

*Expected:* the verification suite can be run twice in one working directory
(which happens whenever a build, a repeated-run loop and a sanitizer suite
overlap).
*Observed:* two instances running together failed with
`unexpected exception ... SQL execute failed: disk I/O error | SQL: PRAGMA
journal_mode = WAL;`. A direct two-instance reproduction gave **31 and 27 failed
cases with 49 occurrences** of that message.
*Root cause:* eleven fixtures across ten test files used **fixed, CWD-relative
database filenames** (`test_persistence.db`, `test_consent_gate.db`,
`test_privacy_<tag>.db`, `test_audit_<tag>.db`, …) and unlinked them in their
constructors. The second instance deleted the first instance's open database and
its `-wal`/`-shm` files; SQLite reports a removed/replaced database file in WAL
mode as a `disk I/O error`, which names neither the cause nor the culprit. It is a
test-fixture defect, not a product defect: a single instance always passes, and
the product opens one database per process.
*Fix:* every fixture path is unique per process (`<name>.db.<pid>`).
*Regression:* `scripts/verify_suite_concurrency.sh` — two instances in one
working directory must both pass, with zero `disk I/O error` occurrences and no
leftover fixture databases. Before: FAIL (31/27 failed). After: both instances
pass (369 cases / 2841 assertions each), for the Release and the ASan binaries.

**This explains every red run observed during v21.** The first ASan suite
(9 failures) and one direct Release run (1 failure) were executed while another
suite instance was running in the same working directory, so all of them are the
same fixture collision. Every one of those runs passes when re-executed alone,
and the fix removes the class of failure rather than the instances. A green run
over an unexplained red one is not evidence — hence this section.

## 9. Test results (actual, executed)

| Suite | Result | Baseline |
|-------|--------|----------|
| Full C++ suite | **369 cases / 2841 assertions** pass | 364 / 2811 (H-01s) |
| CTest | **100% (1/1)** | 100% |
| ASan/UBSan build + full suite | 369 cases / 2841 assertions, **0 sanitizer reports** | clean |
| `[v21]` (new) | 5 cases / 27 assertions | — |
| `[logger]` | 4 cases / 11 assertions | 3 cases |
| `[nativehttp]` | 25 cases / 143 assertions | 22 cases |
| `[h01s]` / `[l11]` / `[l12]` | 18 / 11 / 18 cases — pass | unchanged |
| Legacy Node (`alexa_skill`) | **76/76** pass | 76 |
| Scenario matrix | 10/10 | — |
| HTTP adversarial | 51/51 | — |
| E2E + stability | PASS | — |
| Retention regression | 8/8 | — |
| Fail-closed (capture off / corrupted DB / read-only dir) | 7/7 | — |
| Suite concurrency (two instances, one CWD) | both pass, 0 `disk I/O error` | 31/27 failed before the fixture fix |

Nothing in the matrix was skipped.

## 10. Footprint and performance (fresh Release build)

| Metric | v21 | Previous baseline |
|--------|-----|-------------------|
| `hg_alexa_bridge` (Release) | **3,342,280 B** | 3,320,856 B (+21,424 B, +0.65 %) |
| `homeguardian` (Release) | 3,210,648 B | 3,194,080 B |
| `homeguardian_tests` | 6,610,016 B | — |
| Processes / threads | 1 / 1 | 1 / 1 |
| Idle RSS (fresh process) | 6,392 kB | ~6.4 MB |
| Peak RSS after 5 critical runs | 7,224 kB (= RSS, no growth) | — |
| Idle CPU | 0.00 % of one core over 10 s | — |
| Startup → ready | 3–4 ms | — |
| Critical scenario latency | 72–111 ms | 147–169 ms |
| Read-only endpoints | ≤ 1 ms | — |
| Runtime dependencies | libstdc++ / libm / libgcc_s / libc | unchanged |
| SQLite durability | WAL + `synchronous=FULL` | unchanged |

Scenario latency is dominated by the accepted `synchronous=FULL` fsync; the range
moves with database size and host load. No durability or security setting was
weakened to improve any number.

## 11. Known limitations (unchanged by v21)

- Simulator-only: no camera, microphone, pose inference, medical inference or
  biometrics; H-01/H-02/G-02 not implemented.
- The impact classifier is a deterministic synthetic pattern, not a safety
  detector; it says so in every message, payload and panel.
- The native HTTP demo server is loopback-only, unauthenticated by design and is
  **not** production-ready (no TLS, no identity provider, no rate limiting).
- Alert dedup/cooldown state resets when the process restarts (history does not).
- In degraded mode (unusable audit DB) alerts are decided in memory but not
  persisted; the status endpoint reports `unavailable`.
- The suite is now concurrency-safe within one working directory, but two
  instances still share `homeguardian.log` (the logger's own file, append-only);
  that is expected and harmless, and fixture databases no longer collide.

## 12. Reproduction

```bash
# build + full verification
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=<repo>/hg_build/_deps/spdlog-src \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<repo>/hg_build/_deps/nlohmann_json-src <repo>
make -j4 homeguardian homeguardian_tests hg_alexa_bridge
ctest --test-dir .            # 100%
./homeguardian_tests "[v21]"  # 5 cases / 27 assertions

# regression scripts (need the built binaries)
HG_BRIDGE_PATH=<build>/hg_alexa_bridge bash scripts/verify_retention.sh
HG_TESTS_PATH=<build>/homeguardian_tests bash scripts/verify_suite_concurrency.sh

# live demo + matrices
<build>/hg_alexa_bridge --http config/homeguardian.demo.json <creds> --port 8497 --token demo-token
python3 scenario_matrix.py 8497 .        # 10/10
python3 http_adversarial.py 8497 out.json # 51/51
python3 e2e_stability.py 8497 <pid> .     # PASS
```

## 13. Release-candidate decision

**GO** for release-candidate status on `h04/native-http-low-footprint`.

Evidence: every shipped target builds clean from scratch in Release; the full C++
suite, CTest, ASan/UBSan, Node suite, scenario catalogue, HTTP adversarial matrix,
end-to-end/browser smoke test, privacy/consent checks, persistence/recovery and
retention regressions all pass; three verified defects (HTTP status for an unknown
scenario, an aborting logger, and unapplied retention on the bridge's store) are
fixed with regression coverage; the footprint delta is small and explained; and
`main` remains untouched.

This report does **not** authorize a merge or a release publication, and it is not
a substitute for the owner's manual-testing window.
