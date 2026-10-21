# Environmental context engine — design and handoff (HG-EDGE-05)

**Branch:** `experiment/mi-a3-edge-service` (isolated; no release or security worktree touched).
**Base:** `ba218514` (the completed wireless-development milestone).
**Status:** the module, its test suite and its benchmark are **implemented and passing**.
Nothing is wired into the running Android service yet — this phase is the interface and the
isolated prototype, as briefed.

```
src/core/EnvironmentalContext.{h,cpp}        the engine + Android-to-C++ interface
tests/core/test_environmental_context.cpp    12 test cases / 162 assertions, tag [envcontext]
tools/envcontext_bench.cpp                    cost benchmark (compiled on demand, not a test)
```

---

## 1. The problem this module exists to solve

The Mi A3 sensor inventory (`docs/EDGE_DEVICE_SENSORS_MI_A3.md`) established a fact that
shapes the whole design: of the four environmental metrics the engine already allowlists
in `EnvironmentalBounds::metric_supported` — `temperature | humidity | pressure |
illuminance` — **the phone can supply exactly one**, `illuminance`. Ambient temperature,
barometric pressure and humidity are absent hardware. Meanwhile the device exposes 60+
thermal zones named `battery`, `bms`, `skin-therm`, `xo-therm`, and friends.

That is a trap rather than an inconvenience. A battery temperature is a *plausible* number:
at 46.9 °C it sits comfortably inside the −90…70 °C air-temperature bounds, so it would pass
every range check in the existing code and flow into an evidence path looking perfectly
valid. The operator's instruction — never substitute battery temperature for atmospheric
temperature — is therefore enforced here by **structure**, not by convention or a comment.

---

## 2. The Android-to-C++ interface

### 2.1 Channel taxonomy

Every reading names a channel from a fixed registry. The channel's *class* is what carries
the atmospheric/device distinction, and the two namespaces are disjoint by construction:

| Channel | Class | Unit | Range | Max age | Auth |
|---|---|---|---|---|---|
| `atmospheric.temperature` | **atmospheric** | C | −90…70 | 15 min | **yes** |
| `atmospheric.humidity` | **atmospheric** | % | 0…100 | 15 min | **yes** |
| `atmospheric.pressure` | **atmospheric** | hPa | 850…1100 | 15 min | **yes** |
| `external.weather.temperature` | external | C | −90…70 | 2 h | no |
| `external.weather.humidity` | external | % | 0…100 | 2 h | no |
| `external.weather.pressure` | external | hPa | 850…1100 | 2 h | no |
| `external.terrain.elevation` | external | m | −500…9000 | 24 h | no |
| `illuminance` | illuminance | lux | 0…200000 | 5 min | no |
| `device.battery.temperature` | **device_thermal** | C | −20…80 | 2 min | no |
| `device.soc.temperature` | **device_thermal** | C | −20…120 | 2 min | no |
| `position.latitude` | positional | deg | −90…90 | 10 min | **yes + gated** |
| `position.longitude` | positional | deg | −180…180 | 10 min | **yes + gated** |
| `position.altitude` | positional | m | −500…9000 | 10 min | **yes + gated** |
| `motion.acceleration_magnitude` | kinematic | m/s² | 0…200 | 1 min | no |
| `motion.orientation_change` | kinematic | deg | 0…360 | 1 min | no |
| `motion.presence` | kinematic | bool | 0…1 | 5 min | no |

Three deliberate notes:

* **The four plausibility ranges are identical to `EnvironmentalBounds::plausible()`** so
  there is one definition of "physically possible" rather than two that drift apart.
* `battery.temperature` is deliberately **not** a valid channel name — the tempting wrong
  spelling is refused, and a test asserts that.
* The Mi A3 can populate exactly `illuminance`, the three `motion.*`, and the two
  `device_thermal` rows. The rest are for external sources.

### 2.2 Reading contract

```cpp
struct EnvironmentalReading {
    std::string channel;                      // must be in the registry
    std::string source;                       // "node:mi-a3-04de", "ha:weather.home"
    double value;
    std::string unit;
    system_clock::time_point observed_at;     // when the sensor saw it
    system_clock::time_point received_at;     // when HomeGuardian got it
    Provenance provenance;                    // reused: synthetic | real
    double quality;                           // [0,1]
    double uncertainty;                       // >= 0, in the channel's own unit
    bool uncertainty_declared;                // false => a placeholder, not a claim
    std::string authorization;                // required for auth-gated channels
    std::string note;                         // bounded, non-sensitive
};
```

Mapping the brief's five required properties onto concrete mechanisms:

| Required | Mechanism |
|---|---|
| **timestamps** | `observed_at` and `received_at` are separate, so sensor time and arrival time cannot be conflated. A missing timestamp is refused; a future one beyond `max_future_skew` is refused. |
| **provenance** | the existing `Provenance { synthetic, real }` enum — no new vocabulary, so a replay fixture can never present itself as a live measurement. |
| **freshness** | **computed**, never asserted: `age = now − observed_at` against the channel's own `max_age`. Channels expire on their own schedules (a 3-minute-old sample is stale for the battery at 2 min but still fresh for illuminance at 5 min — asserted). |
| **quality** | `[0,1]`, refused outside the range. |
| **uncertainty** | explicit, in the channel's unit, non-negative, and **propagated** through fusion rather than averaged away. |

### 2.3 Fusion

Multiple fresh readings of one channel fuse by **inverse-variance weighting** when every
contributor declares an uncertainty:

```
value  = Σ(vᵢ/σᵢ²) / Σ(1/σᵢ²)
σ      = sqrt( 1 / Σ(1/σᵢ²) )
```

So two readings of 10 ± 1 and 20 ± 2 fuse to **12 ± 0.894** — the estimate is pulled toward
the more precise input and the fused uncertainty is *smaller* than either, which is the
whole point of collecting uncertainty. When uncertainties are not declared the engine falls
back to a plain mean and reports `uncertainty_declared = false` with the worst declared
input's σ — it never invents a precision it was not given.

### 2.4 The accessors that make the trap impossible

```cpp
bool                 atmospheric_series_available() const;  // false on the Mi A3
std::optional<double> atmospheric_temperature() const;      // NEVER falls back to battery
std::optional<double> battery_temperature() const;          // separate by construction
```

`std::optional` is the right return type here: on this device "there is no air temperature"
is a **real answer**, not an error, and the API should be unable to express anything else.
`to_json()` renders the two as distinct keys so the separation survives serialisation.

### 2.5 Thresholds are injected, never invented

A daylight decision and a thermal-health band both need a number, and inventing one here
would be exactly the arbitrary-threshold failure this project forbids. So both are injected
via `Config`, each with a `*_source` string naming where the number came from:

* no `daylight_lux_threshold` (or no source string) → `daylight()` returns `nullopt`;
* no `thermal_warn_c`/`thermal_critical_c` (or no source string) → `thermal_health()`
  returns `unknown`, even for a battery sitting at 55 °C.

The thermal banding exists to protect the handset, so its limits should come from the
device's own thermal trip points — for example `quiet-therm-adc`'s passive trip at 56 °C
recorded in the HG-EDGE-02 work — not from a number chosen in this file. A test asserts
that a hot *air* temperature cannot produce a device-thermal health verdict.

### 2.6 Positional channels: off by default, twice gated, coarsened

Continuous GPS tracking was explicitly not authorized, so `position.*` is gated in two
independent places:

1. `Config::positional_enabled` — **default `false`**; the check runs *before*
   authorization, so a caller cannot enable position merely by supplying a grant.
2. A per-reading authorization context containing `granted` (the convention already used
   by `EnvironmentalAdapter`).

When enabled, positions are **coarsened to 2 decimal places (~1.1 km) on ingest**, so a
precise coordinate is never even retained in memory. There is no code path that turns any
of this on by itself.

---

## 3. Integration with external context and future sensors

The registry already reserves the channels for the sources the device cannot provide, so
adding them is configuration rather than new code:

| Future source | Channel(s) | Note |
|---|---|---|
| Home Assistant / weather service | `external.weather.{temperature,humidity,pressure}` | already the pattern used by `HomeAssistantAdapter`; 2 h max age because a forecast observation ages slowly |
| Terrain / elevation dataset | `external.terrain.elevation` | 24 h max age — it only changes if the device moves. Usable to sanity-check a GNSS altitude instead of trusting it blindly. |
| Optional BLE temperature/humidity/pressure puck | `atmospheric.temperature`, `atmospheric.humidity`, `atmospheric.pressure` | The Mi A3 **cannot** supply these; a real external sensor is the only honest route. Note from the inventory: a BLE scan on API 30 requires `ACCESS_FINE_LOCATION`, so this needs separate approval. |

Crucially, a genuine external sensor and a device thermal zone arrive on **different
channels**, so "we now have a real temperature" cannot accidentally be satisfied by the
phone warming up in a pocket.

---

## 4. Correlation with animal-behaviour detections

The output vocabulary is exactly two strings:

```
environmental_correlation
insufficient_evidence
```

A correlation is produced only when a channel's newest reading falls within `max_lag` of a
behavioural change; otherwise the honest answer is `insufficient_evidence`. The result
carries the channel, value, uncertainty and signed lag, and its reason string is worded as
association — *"environmental reading moved within the accepted window of the change"* —
not causation and not forecast.

A test enumerates the vocabulary and fails if any status contains `predict`, `forecast`,
`caus`, `earthquake`, `seismic`, `calamity`, `disaster`, `flood`, `species` or `animal`.
That is the same discipline `DempsterShafer` already applies by keeping `SeismicPrecursor`
out of its frame of discernment: the unsupported output is made **unrepresentable** rather
than merely discouraged. This is why `possible_animal_activity` remains the only
animal-related label in the repo, and it is reachable only through the existing precursor
path, never from here.

---

## 5. Verified results

### 5.1 Functional tests — PASS

```
$ ./build_envcontext/homeguardian_tests "[envcontext]"
All tests passed (162 assertions in 12 test cases)

$ ./build_envcontext/homeguardian_tests          # whole suite, regression check
All tests passed (39088 assertions in 538 test cases)
```

**exit code 0**, captured without a pipe. The 12 cases cover: taxonomy disjointness
(asserted over *every* registry entry, not just the named pair); the battery-vs-air
separation including its JSON rendering; a plausible 46.9 °C SoC reading still failing to
satisfy an atmospheric request; plausibility, non-finite, missing-timestamp, future-skew,
quality and negative-uncertainty refusals; nothing-stored-on-rejection; per-channel
freshness including the differing battery/illuminance schedules; inverse-variance fusion
with an exact expected value; undeclared-uncertainty degradation; bounded memory over 500
ingests; the positional double gate plus coarsening; atmospheric authorization
(grant/deny/absent); thermal and daylight threshold injection; the non-predictive
vocabulary; correlation inside and outside the lag window; and byte-identical replay.

### 5.2 Cost — measured on the host, x86-64

```
channels simulated     : 5 (the Mi A3's available set)
ingest                 : 200000 readings in 178.6 ms = 0.89 us/reading (1.12M readings/s)
snapshot               : 20000 snapshots in 51.5 ms  = 2.57 us/snapshot
engine RSS (bounded)   : 3596 kB -> 3696 kB (delta +100 kB)
retained readings      : 160  (5 channels x max_readings_per_channel=32)
```

A second run reproduced it (0.79 µs/reading, 2.74 µs/snapshot), so the figures are stable
rather than a lucky cache. Memory is bounded by design: 500 ingests still retain 160.

**ARM64 numbers are NOT measured on-device** and I will not present an estimate as a
measurement. Cross-compiling this benchmark for `arm64-v8a` and running it on the Mi A3 is
the obvious next step, and it must go through the app rather than `/data/local/tmp`
execution, which the operator ruled out.

### 5.3 Battery-impact assessment

Mostly an *absence* of impact, because the engine is not where cost lives:

* **Engine CPU is negligible.** At the Mi A3's realistic rate — one illuminance reading a
  minute, motion events, battery temperature every two minutes — that is a few thousand
  readings a day. Even at a deliberately pessimistic 1 Hz across all five channels
  (432,000 readings/day) the host figures imply well under a second of CPU per day; an
  ARM64 penalty of an order of magnitude does not change that conclusion.
* **The measured baseline it must not regress** is the existing service at **0.045 % of one
  core** (9.8 s CPU over 21,497 s, HG-EDGE-02 §10.10) — the engine sits two to three orders
  of magnitude below that.
* **The real cost is sensor activation, on the Android side.** Registering the light sensor
  adds nothing measurable because the phone already polls it at 8 Hz. `significant_motion`,
  `motion_detect` and `stationary_detect` are one-shot hardware triggers. The accelerometer
  is the one streaming sensor worth measuring before adoption, and its 3000-event FIFO buys
  ~10 minutes of AP sleep at 5 Hz.
* **GNSS is the expensive sensor and it is off by default.** That single default is the
  most consequential power decision in this design, and the double gate exists because of it.
* **Loopback-only MCP is untouched** — this module opens no socket, and the service's
  binding stays `0100007F:2256` (asserted by the edge toolkit). Foreground-service
  behaviour is likewise unchanged: nothing here alters the service lifecycle.

---

## 6. What is not done, and what I would not claim

* **Not wired into the app.** No JNI surface, no Android side, no permission requested. The
  brief scoped this phase to the interface plus an isolated prototype, and that is what
  exists.
* **No on-device ARM64 measurement** (see 5.2). Host x86-64 only.
* **No atmospheric data exists on this device**, and the prototype cannot change that. Until
  an external sensor or weather source is connected, `atmospheric_series_available()` is
  `false` — which is the correct answer, not a gap to be papered over.
* **No GNSS enabled**, no camera/microphone, no new permissions.
* **No claim of 24/7 readiness** and no soak, no unattended charging, no reboot experiment,
  no public exposure.

## 7. Recommended next step

Wire the two lowest-risk real channels — `illuminance` and `motion.presence` — into the
existing foreground service behind the same consent gate, cross-compile the benchmark for
`arm64-v8a`, and measure the delta against the 0.045 % baseline before adding anything
else. That is a measurement, not a commitment, and it is the honest way to find out whether
the accelerometer is affordable on this handset.
