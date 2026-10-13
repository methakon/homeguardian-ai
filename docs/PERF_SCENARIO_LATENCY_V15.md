# Native scenario latency — investigation and findings (v15, 2026-10-13)

**Question (from v14):** `/api/run-scenario` measured *slower* on the native
single-process server (~50–180 ms) than on the legacy per-request Node→C++ path
(~7–16 ms). Why? Is it an artifact of the native design, and is there a safe
optimization?

**Answer:** the cost is **SQLite's durable commit (`synchronous=FULL`, the
default) paying one `fsync` per commit, and this host's filesystem charges
~50–410 ms per `fsync`.** It is **not** a flaw in the native architecture. No
optimization is available that removes it *without weakening durability*, which
is out of scope — so this is reported as an investigated finding with options for
approval, not a code change.

## 1. Reproduction (Release build, before any change)

`python3 /tmp/bench_lat.py` (40 trials, warm-up, curl `time_total`):

| endpoint | n | median | mean | p95 | min | max | sd |
|---|---|---|---|---|---|---|---|
| native `/api/run-scenario` | 40 | **46.5 ms** | 87.4 ms | **437 ms** | 43.0 ms | 602 ms | 120 ms |
| native `/api/status` | 30 | 0.61 ms | 0.62 ms | 0.70 ms | 0.58 ms | 0.73 ms | 0.04 ms |
| native `/api/health` | 30 | 0.25 ms | 0.26 ms | 0.27 ms | 0.24 ms | 0.55 ms | 0.05 ms |
| legacy `/api/run-scenario` | 40 | 6.1 ms | 6.2 ms | 7.3 ms | 5.4 ms | 8.0 ms | 0.53 ms |
| legacy `/api/status` | 30 | 4.81 ms | 4.88 ms | 5.4 ms | 4.3 ms | 5.8 ms | 0.33 ms |

Native read endpoints are **faster** than legacy (0.6 ms vs 4.8 ms status); only
the *scenario* route is slow, and it is **wildly variable** (43 ms → 602 ms).

## 2. Phase breakdown (steady-clock, `HG_PROFILE=1`)

Bounded instrumentation splits `run_demo_scenario` into phases (see §6).
40 trials, phase medians:

| phase | median | p95 | max |
|---|---|---|---|
| reset | 0.03 ms | 0.04 | 0.07 |
| capture_enable (`set_media_capture_enabled`) | 0.01 ms | 0.01 | 0.04 |
| consent (grants) | 0.01 ms | 0.01 | 0.01 |
| sim_ticks (start+5 ticks+shutdown) | 0.76 ms | 0.80 | 1.40 |
| detection + fusion | 0.70 ms | 0.75 | 0.76 |
| **persist (SQLite alert write)** | **91.8 ms** | **519.8** | **854.7** |
| status_call (contract) | 0.19 ms | 0.21 | 0.23 |
| summary_call (contract) | 0.11 ms | 0.14 | 0.15 |

**99 % of the latency is the `persist` phase — one SQLite alert commit.** Runtime
preparation, the shared audio broker, the simulator, and detection/fusion are all
sub-millisecond. HTTP parsing/dispatch is bounded by the 0.25 ms `/api/health`
round-trip.

## 3. Is it the native design or the storage? (decisive control)

Same DB file, same time window, identical code path — differing only in process/
connection lifetime:

| writer | persist median | p95 | max |
|---|---|---|---|
| fresh process + fresh connection (legacy child model) | 0.11 ms | 0.13 | 88 ms |
| long-lived connection (native server) | 30.6 ms | 455 | 518 ms |

Then `strace -f -T -e trace=fsync,fdatasync`:

- **both** paths issue `fsync` and both pay dearly — the legacy child showed 11
  fsyncs at **28–220 ms each**; the native showed 11 fsyncs at **50–410 ms each**.
  The difference is *where* the sync lands (in the measured request for the
  long-lived connection; partly at connection close for the child) and normal
  filesystem variance — **not** whether fsync happens.

Isolated confirmation with the stock `sqlite3` CLI/module (no HomeGuardian code),
20 commits each on this filesystem (`/dev/sdc3`, ext4):

| `PRAGMA synchronous` | median | p95 | max |
|---|---|---|---|
| `FULL` (SQLite default) | **92.7 ms** | 222 ms | 229 ms |
| `NORMAL` | 0.03 ms | 0.07 ms | 0.08 ms |

**Conclusion:** `synchronous=FULL` costs ~93 ms per commit here; the native
scenario does exactly one commit per run; therefore ~93 ms median with a
long latency tail. The legacy model looked fast only because its per-request
child paid the fsync partly at teardown and the end-to-end timing masked it.

## 4. Is there a safe optimization?

| Option | Effect | Durability / security impact | Verdict |
|---|---|---|---|
| `PRAGMA synchronous=NORMAL` (WAL) | ~0.03 ms/commit → scenario ~1–2 ms | **Reduces durability**: crash-safe (no corruption) but the most recent committed transactions can be lost on OS crash / power loss | **Requires explicit approval** (mission forbids weakening durability for speed) |
| Batch/async the alert write | fewer fsyncs | **Reduces durability** (same class of risk) | Not allowed without approval |
| Open/close the DB per scenario run | unknown (child model) | none (same durability) | An **architectural** change; must be measured first; may merely move the fsync to close |
| Different storage / disable the slow flush | faster fsync | environmental | Out of scope; a host, not app, property |
| Reduce work before the write | — | none | **Measured: nothing to reduce** (all other phases < 1 ms) |

**No safe, in-scope optimization removes the cost.** The remaining work is
sub-millisecond; the single durable commit is the whole latency.

## 5. Recommendation

1. **Keep durability as-is** for now. The native scenario's latency on *this*
   host is a *storage* characteristic (fsync ≈ 93 ms), not a design defect; on a
   host with normal fsync (~1–10 ms) it would be ~1–10 ms, i.e. comparable to the
   legacy path — while keeping native's far lower RSS/CPU/process/dependency
   footprint.
2. **If the demo's scenario latency must be reduced**, the single lever is
   `PRAGMA synchronous=NORMAL` on the audit DB connection (scenario → ~1–2 ms).
   This is a **durability trade-off** and is **not applied**: the mission requires
   stopping for approval. Recommendation: accept the trade-off **only if** the
   audit DB is treated as a demo convenience rather than the durable record of
   record; otherwise keep `FULL`.
3. Do **not** remove the alert persistence or bypass SQLite (explicitly out of
   scope).

**Decision required from the user:** (a) keep `synchronous=FULL` (durable,
~93 ms/run on this host), or (b) approve `synchronous=NORMAL` for the demo audit
connection (~1–2 ms/run, crash-safe but may lose the most recent commits on power
loss). No license/code change is made for this without approval.

## 6. Instrumentation added (bounded, default-off)

`HG_PROFILE=1` (env) enables phase timing in `run_demo_scenario` and the HTTP
handler; it adds only numeric `timing_ms` / `handler_timing_ms` fields to the
JSON response and is **inert when unset** (verified: the default response has no
such fields and all counts are unchanged). It leaks no credentials, paths, or
media. Reproduce with:

```
HG_PROFILE=1 hg_build/hg_alexa_bridge --http config/homeguardian.demo.json <creds> --port 0 --token demo-token
curl -s 'http://127.0.0.1:<port>/api/run-scenario?cycles=5' | python3 -m json.tool | grep -A12 timing_ms
```

## 7. Limitations

- The absolute numbers are host-specific (this volume's fsync is unusually slow);
  the *ratio* between `FULL` and `NORMAL` and the phase attribution are the
  portable findings.
- The exact fsync count per commit (SQLite may sync the WAL and, on checkpoint,
  the DB file) was not decomposed further; the isolated `sqlite3` test bounds the
  cost per commit directly.
- The child-vs-native accounting is affected by where the sync lands (request vs
  teardown); the strace evidence documents that both sync.
