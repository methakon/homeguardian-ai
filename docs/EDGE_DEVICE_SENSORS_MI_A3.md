# Mi A3 sensor discovery — HomeGuardian environmental context (HG-EDGE-03)

**Phase:** discovery and specification only. No sensor was activated, no permission was
requested or granted, no code was changed, nothing was deployed.
**Device:** Xiaomi Mi A3 (`laurel_sprout`), Android 11 / API 30, serial tail `…04de`.
**Method:** `adb shell dumpsys sensorservice` — read-only, needs no app and no
permission. 439 lines of output, captured 2026-10-17 10:16:44 IST.

> **Headline.** The device reports **28 hardware/HAL-backed sensors**, and among the
> four environmental metrics HomeGuardian already accepts it can supply exactly
> **one: `illuminance`**. Ambient temperature, barometric pressure and humidity are
> **absent hardware** — confirmed by searching the entire sensor dump for those types,
> not by failing to notice them. The phone does expose 60+ thermal zones including
> `battery` and `bms`, and **none of them measures air temperature**; this report does
> not substitute one, and section 4 explains why that would be a fabrication.

---

## 1. Scope, and what was deliberately not done

The brief for this phase allows discovery only. Accordingly:

* No sensor was registered, sampled, or enabled by this work. Nothing was added to
  the edge-service app.
* **No permission was requested or granted** — not `ACTIVITY_RECOGNITION`, not
  `ACCESS_FINE_LOCATION`, not `BLUETOOTH`. The installed app still declares only
  INTERNET, FOREGROUND_SERVICE and RECEIVE_BOOT_COMPLETED.
* **No location was collected, and no coordinates appear anywhere in this document.**
  Where location state had to be inspected, the command was filtered to provider
  names and enablement flags, and the `Last Known` blocks were counted rather than
  echoed (there were **0** at the time of capture).
* No hardware was purchased, and nothing was exposed to a network.
* The existing service/reboot work was not modified; the node kept running throughout
  (see section 9 for its live cost).

---

## 2. Sensor inventory (measured)

Wake-up-flag bits were derived from the dump's own labels and are unambiguous:
**`0x1` = WAKE_UP, `0x2` = ON_CHANGE, `0x4` = ONE_SHOT.** A wake-up sensor may wake the
application processor; a non-wake-up sensor cannot. That distinction, not a datasheet
number, is the power lever that this device actually exposes (see section 8).

### 2.1 Hardware / HAL-backed sensors (28)

| Android type (id) | Vendor | Part / name | Reporting mode | Wake-up | Permission |
|---|---|---|---|---|---|
| `accelerometer` (1) | BOSCH | BMI160_ACCELEROMETER | continuous 1–400 Hz | **no** | n/a |
| `accelerometer_uncalibrated` (35) | BOSCH | BMI160 | continuous 1–400 Hz | no | n/a |
| `gyroscope` (4) | BOSCH | BMI160_GYROSCOPE | continuous 1–400 Hz | no | n/a |
| `gyroscope_uncalibrated` (16) | BOSCH | BMI160 | continuous 1–400 Hz | no | n/a |
| `magnetic_field` (2) | akm | ak0991x | continuous 1–100 Hz | no | n/a |
| `magnetic_field_uncalibrated` (14) | akm | ak0991x | continuous 1–100 Hz | no | n/a |
| `light` (5) | sensortek | stk_stk3x3x | on-change | no | **n/a** |
| `light` (5) | sensortek | stk_stk3x3x | on-change | **yes** | **n/a** |
| `proximity` (8) | sensortek | stk_stk3x3x | on-change | no | n/a |
| `proximity` (8) | sensortek | stk_stk3x3x | on-change | **yes** | n/a |
| `gravity` (9) | qualcomm | fused | continuous 5–200 Hz | no | n/a |
| `linear_acceleration` (10) | qualcomm | fused | continuous 5–200 Hz | no | n/a |
| `game_rotation_vector` (15) | qualcomm | fused | continuous 5–200 Hz | no | n/a |
| `device_orientation` (27) | qualcomm | fused | on-change | no | n/a |
| `device_orientation` (27) | qualcomm | fused | on-change | **yes** | n/a |
| `significant_motion` (17) | qualcomm | sns_smd | one-shot | **yes** | n/a |
| `stationary_detect` (29) | qualcomm | sns | one-shot | no | n/a |
| `stationary_detect` (29) | qualcomm | sns | one-shot | **yes** | n/a |
| `motion_detect` (30) | qualcomm | sns | one-shot | no | n/a |
| `motion_detect` (30) | qualcomm | sns | one-shot | **yes** | n/a |
| `step_detector` (18) | qualcomm | pedometer | special-trigger | no | **ACTIVITY_RECOGNITION** |
| `step_detector` (18) | qualcomm | pedometer | special-trigger | **yes** | **ACTIVITY_RECOGNITION** |
| `step_counter` (19) | qualcomm | pedometer | on-change | no | **ACTIVITY_RECOGNITION** |
| `step_counter` (19) | qualcomm | pedometer | on-change | **yes** | **ACTIVITY_RECOGNITION** |
| `fod_detector` (33171030) | XiaoMi | vendor | on-change | no | n/a |
| `fod_detector` (33171030) | XiaoMi | vendor | on-change | **yes** | n/a |
| `pickup` (33171036) | XiaoMi | vendor | on-change | no | n/a |
| `pickup` (33171036) | XiaoMi | vendor | on-change | **yes** | n/a |

### 2.2 AOSP virtual / fused sensors (8)

`gravity`, `linear_acceleration`, `rotation_vector`, `game_rotation_vector`,
`geomagnetic_rotation_vector`, `orientation`, plus a debug accelerometer alias. These
are computed in the framework from the hardware sensors above — they cost nothing extra
in hardware terms but they do **not** add information the device does not already have.

### 2.3 Batching (what makes low-power sampling possible)

`FIFO (max, reserved)` is the hardware buffer. The reserved figures are the usable ones:

| Sensor | min / max rate | FIFO reserved | Buffered at a low rate |
|---|---|---|---|
| accelerometer | 1–400 Hz | 3000 events | 5 Hz → **~10 min** of buffering |
| magnetic_field | 1–100 Hz | 600 events | 5 Hz → ~2 min |
| gyroscope | 1–400 Hz | 0 | not batchable |
| gravity / linear_acceleration / game_rotation_vector | 5–200 Hz | 300 events | 5 Hz → ~1 min |
| proximity | on-change | 300 events | event-counted |
| light / device_orientation / pickup / fod | on-change | 0 | not batchable |

---

## 3. Absent hardware — the finding that shapes this phase

Searching the **whole** 439-line dump:

| Type searched | Occurrences | Verdict |
|---|---|---|
| `ambient_temperature` | **0** | **ABSENT** |
| `pressure` | **0** | **ABSENT** |
| `relative_humidity` | **0** | **ABSENT** |
| `temperature` (any form) | **0** | **ABSENT** |
| `heart_rate` | 0 | absent |
| `light_uncalibrated` / `rgb` | 0 | absent |

The device has **no atmospheric sensor at all**. This is not an omission in the dump:
the same search returns 1+ hits for types that *do* exist (`light`, `proximity`,
`accelerometer`), so the method discriminates.

### 3.1 Consequence for HomeGuardian

The engine's environmental allowlist is exactly
`temperature | humidity | pressure | illuminance`
(`src/core/EnvironmentalCorrelation.h`, `EnvironmentalBounds::metric_supported`).
**Three of those four cannot be sourced from this phone, ever.** Any design that
assumes the edge node is a weather station is building on hardware that does not exist.

---

## 4. The battery-temperature trap (explicitly refused)

The device exposes **60+ thermal zones**, and several are named in a way that invites
exactly the substitution the brief forbids:

```
battery            bms                skin-therm-adc      quiet-therm-adc
xo-therm-adc       rf-pa0-therm-adc   camera-ftherm-adc   emmc-ufs-therm-adc
pm6125-tz          pmi632-tz          cpuss-0-usr         aoss0-usr  … and more
```

**None of these measures air temperature**, and they are not interchangeable with it:

| Zone | What it actually measures |
|---|---|
| `battery`, `bms` | the cell/pack of the phone — this is the battery temperature the brief names |
| `skin-therm-adc` | the phone's own case/skin, i.e. a *device* surface |
| `xo-therm-adc` | the crystal-oscillator reference |
| `rf-pa0-therm-adc` | the RF power amplifier |
| `emmc-ufs-therm-adc` | the storage die |
| `cpuss-*`, `aoss0-usr`, `gpu-usr` | SoC subsystems, and they swing with load |
| `lmh-dcvs-00`, `*-lvl*`, `*-step` | **limiter thresholds, not measurements** (`lmh-dcvs-00` reads a flat 75 °C) |

A phone held in a pocket, charging, or under CPU load reports a battery temperature
that tracks the *phone's* state, not the room. Reporting it as `metric="temperature"`
would produce a plausible-looking number in a valid-looking `EnvironmentalSample`,
would pass `EnvironmentalBounds::plausible()` (−90…70 °C), and would be **fabricated
data flowing into an evidence path**. This report therefore records temperature as
UNAVAILABLE rather than approximating it, and the edge-service app's config already
reports `capture_enabled=false` for the same reason — it does not claim what it cannot
measure.

**If a temperature/humidity/pressure channel is wanted, it needs separate hardware**
(an external BLE or Home-Assistant sensor), which the brief places out of scope.

---

## 5. Usefulness assessment against the engine's existing vocabulary

The important design constraint is that HomeGuardian **already has** two ingestion
models, and a new sensor must map onto one of them rather than start a third.

| Existing model | File | Vocabulary |
|---|---|---|
| `EnvironmentalSample` | `src/core/EnvironmentalCorrelation.h` | metric ∈ {temperature, humidity, pressure, **illuminance**} |
| `SecurityObservation` | `src/core/SecurityObservation.h` | `source_type` ∈ {motion, …}, state on/off/open/closed/present/absent |

| Mi A3 channel | Maps to | Value for HomeGuardian |
|---|---|---|
| **Ambient light** (`light`, on-change, no permission) | `EnvironmentalSample`, `metric="illuminance"` | **Genuinely useful and the only environmental metric available.** Day/night state, cloud/overcast transition, room-light change, and a strong cross-check for a *synthetic* illuminance claim. On-change reporting means no polling cost. |
| **Accelerometer / linear_acceleration / gravity** | `SecurityObservation`, `source_type=motion` | Useful: vibration, impact, door/surface disturbance. The 3000-event FIFO allows ~10 min of 5 Hz buffering, so it can be sampled without holding the AP awake. |
| **`significant_motion`, `motion_detect`, `stationary_detect`** | `SecurityObservation` motion transitions | **Best power/value ratio**: one-shot, hardware-detected, some are wake-up capable. Ideal for presence/activity-change without streaming. |
| **Gyroscope / rotation_vector / device_orientation** | security/motion (orientation change) | Marginal on a stationary hub; more useful as a tamper/moved indicator. |
| **Magnetometer** | weak | Indoors it is dominated by building distortion; not a reliable environmental channel. Uncalibrated variant is debug-grade. |
| **Proximity** | security (enclosure/tamper) | Coarse but free; useful as "something is in front of the sensor". |
| **`pickup`, `fod_detector`** | vendor-specific | Not portable; ignore for the engine. |
| **`step_detector`, `step_counter`** | activity | Requires **`ACTIVITY_RECOGNITION`**, verified `prot=dangerous\|instant` — a runtime prompt. Not needed for a fixed hub; **not requested**. |
| **GNSS** | location | Enabled device-wide (section 6) but **not collected by our app** and not approved. |
| **BLE** | external sensors | Available in principle; blocked by an API-30 permission rule (section 7). |
| **ambient temperature / humidity / pressure** | — | **Absent hardware.** |

---

## 6. GNSS / location status (inspected, not collected)

| Fact | Value |
|---|---|
| GNSS HAL | `init.svc.gnss_service = running` |
| Assisted GNSS | `persist.sys.gps.lpp = 2` |
| Device master switch | `settings get secure location_mode = 3` → **high accuracy is ON across the whole device** |
| Providers active | only `passive` requests from `com.google.android.gms` (fused + network), `monitoring location: false` |
| Recent GPS use | `+ gps request from com.google.android.gms … At 10-17 05:59:10`, released 2 s later; `gps: … Last active 262 minutes ago` |
| Cached coordinates in the dump | **0** `Last Known` blocks — nothing to leak at capture time |
| **Our app** | requests **no** location permission of any kind |

Two points worth recording. First, high-accuracy location is enabled **device-wide and
predates this work** — it is the phone's own setting, not something this project turned
on, and Google Play Services requests GPS periodically for its own reasons. Second, that
is precisely why the brief's caution is right: the honest design keeps HomeGuardian's
permission set at zero location and treats the device's own location activity as
something to be aware of, not to piggyback on.

**Recommendation: collect nothing from GNSS. Geographic context must keep arriving from
the existing external source, not from this phone.**

---

## 7. BLE external sensors — and the permission wall that actually blocks them

BLE hardware is present and the controller is already in a BLE-capable state:

```
ro.bluetooth.library_name = libbluetooth_qti.so
state: BLE_ON          (even though `enabled: false` for classic BT)
Enabled 10-17 04:29:11  due to APPLICATION_REQUEST by com.google.android.gms
1 BLE app registered
```

An external BLE sensor (a real temperature/humidity/pressure puck) is therefore the
**only** credible route to the three missing environmental metrics — and it hits a hard
platform rule on this device:

| Check | Result on this API-30 device |
|---|---|
| `pm list permissions` contains `BLUETOOTH_SCAN` | **0 occurrences — the permission does not exist** |
| `pm list permissions` contains `BLUETOOTH_CONNECT` | **0 occurrences** |
| Legacy `BLUETOOTH` permission | present (1) |

`BLUETOOTH_SCAN` with `android:usesPermissionFlags="neverForLocation"` is the API-31+
mechanism that lets an app scan without location. **On API 30 it does not exist**, so a
BLE scan requires **`ACCESS_FINE_LOCATION`** — a sensitive runtime permission that the
brief explicitly gates behind separate approval.

**So: BLE external sensors are technically available, and cannot be enabled without a
location permission on this Android version.** That is a decision for the operator, not
a discovery to be quietly worked around. Nothing was requested.

---

## 8. Proposed Android → C++ sensor interface

**Design rule: do not invent a parallel model.** The engine already defines both the
value object and its validation, so the interface is a *mapping* plus three small
additions.

### 8.1 Mapping asked-for field → existing field

| The brief asks for | Existing field | Status |
|---|---|---|
| timestamped | `EnvironmentalSample::observed_at` **and** `SecurityObservation::ingested_at` | **EXISTS** — both acquisition and ingestion times are already modelled |
| typed readings | `metric` + `value` + `unit` (allowlisted) | **EXISTS** |
| source identifiers | `source` (producer) + `device_id` on the security path | **EXISTS** |
| synthetic/live separation | `Provenance provenance` | **EXISTS** |
| explicit consent | `authorization` (must contain `granted`) | **EXISTS** |
| **accuracy** | — | **MISSING** on `EnvironmentalSample` |
| **confidence** | `confidence` exists on `SecurityObservation` only | **MISSING** on `EnvironmentalSample` |
| **freshness** | derivable from `observed_at` vs `ingestion_now`; future-skew already bounded | **PARTIAL** — no staleness bound |

### 8.2 Proposed additions (skill: additive, three fields)

```cpp
// EnvironmentalSample — additive, so every existing caller keeps compiling.
enum class ReadingAccuracy { unknown, low, medium, high };  // from SensorEvent accuracy
ReadingAccuracy accuracy = ReadingAccuracy::unknown;
std::optional<double> confidence;      // [0,1]; mirrors SecurityObservation
// Freshness is COMPUTED, not stored: ingestion_now - observed_at.
// Add config: std::chrono::seconds max_staleness{...} to reject stale readings,
// exactly as max_future_skew already rejects future ones.
```

Rationale for each: `SensorEvent.accuracy` is the platform's own calibration state
(`SENSOR_STATUS_ACCURACY_LOW/MEDIUM/HIGH`) and belongs on the reading; `confidence`
already exists one struct over, so mirroring it is consistency rather than invention;
staleness is the missing half of a bound the code already applies in one direction.

### 8.3 Source identifier convention

```
source   = "node:mi-a3-<serial-tail>"     // producer, for EnvironmentalSample
device_id = "mi-a3-<serial-tail>"          // entity, for SecurityObservation
```

The serial tail is used, never the full serial, and never a coordinate. This keeps a
reading attributable without making the document or the log personally identifying.

### 8.4 What this device can actually emit, concretely

Only one `EnvironmentalSample` metric is reachable today, and it was observed live:

```
metric="illuminance"  value=96.12  unit="lux"   (stk_stk3x3x, on-change, no permission)
```

`EnvironmentalBounds::plausible("illuminance", 96.12)` → **true** (bound is 0…200000 lux),
so a real reading from this phone already validates against the existing engine with no
changes. That is the whole environmental contribution of the device, and it is honest.

Everything else the phone can contribute arrives as `SecurityObservation`
(`source_type=motion`) — vibration, motion, presence, orientation change.

### 8.5 Registration policy (power-first, and it is not negotiable)

1. **Register non-wake-up sensors by default.** A wake-up sensor can wake the AP; on an
   edge node whose entire measured cost is 7.5 s of CPU per 5 h 45 m (section 9), adding
   a wake source is the single most expensive thing this design could do.
2. **Prefer one-shot / on-change over continuous.** `significant_motion`,
   `motion_detect`, `stationary_detect`, `light`, `proximity` are all event-driven.
3. **Use batching** — `requestTriggerSensor`/`registerListener` with a max report
   latency, and let the 3000-event accelerometer FIFO hold data while the AP sleeps.
4. **Never request a permission to obtain a channel that is already permission-free.**

---

## 9. Battery and power implications

### 9.1 What this node costs today (measured, not estimated)

Re-measured with one instrument (`/proc/<pid>/stat`) and one clock (`CLK_TCK=100`),
normalised by each process's own age so the figures are directly comparable:

| Process | CPU | Age | Rate |
|---|---|---|---|
| `org.homeguardian.edgeservice` (our node) | **9.8 s** | 21,497 s | **0.045 % of one core** |
| `cnss_diag` (system Wi-Fi/GNSS diagnostics) | 304.7 s | 21,558 s | 1.414 % |
| `logd` (system log daemon) | 48.2 s | 21,578 s | 0.224 % |

The phone's own connectivity daemon costs **~31×** this node. These are always-present
system processes, deliberately chosen over the third-party app I compared against earlier
(that app has since exited, so its figure was a transient observation and not a baseline).

Any sensor work must be judged against the 0.045 % figure above.

### 9.2 Why no mW figure appears here

`dumpsys sensorservice` on this device contains **no** `power=`, `maxRange` or
`resolution` fields (verified: 0 matches in 439 lines). Per-sensor power in mW is
available only in-process via `Sensor.getPower()`. Rather than quote datasheet numbers
as if they were measurements, this report uses the properties the device **does**
expose and which actually determine cost:

* **wake-up vs non-wake-up** — the AP-wake lever, measured above;
* **continuous vs on-change vs one-shot** — whether the sensor streams at all;
* **FIFO reserved events** — how long the AP may stay asleep and still lose nothing.

### 9.3 Expected impact of the proposed (approval-free) set

| Channel | Mode | Expected effect |
|---|---|---|
| `light` (illuminance) | on-change, non-wake-up | **≈none.** The sensor is *already* being polled on this phone: the dump's ring held 50 events spanning 6.2 s with 48 of 49 deltas at exactly **125 ms (8 Hz)**. Something on the device is streaming it continuously, so registering an additional on-change listener adds no new hardware polling. |
| `significant_motion` / `motion_detect` / `stationary_detect` | one-shot | **≈none** — hardware-detected, fires rarely. |
| `accelerometer` at 5 Hz | continuous, **non-wake-up**, batched | Small but non-zero: it is a streaming sensor. The 3000-event FIFO means the AP need only wake to drain it. This is the one channel worth measuring before committing. |
| GNSS | — | **Excluded** (not approved). This is also by far the most expensive sensor on the device, which is a second reason to leave it alone. |
| BLE scan | — | **Excluded** (needs `ACCESS_FINE_LOCATION` on API 30). A continuous BLE scan is the most battery-costly of the candidate features. |

**Recommended first step: a measurement, not a commitment** — register the light sensor
and `significant_motion` only, over a multi-hour window, and compare the node's CPU and
the phone's discharge rate against the 7.5 s baseline above. Those two are the
lowest-risk channels and the light sensor is the only one that feeds the environmental
engine.

---

## 10. Integration with the existing animal-activity, weather and geographic context

The repository already constrains what this can honestly be, and the constraint is
helpful rather than limiting:

* The precursor DST frame is **`{routine, activity_change, environmental_anomaly}`**.
  `SeismicPrecursor` was deliberately removed because the source skill's own correction
  ledger records that animal seismology is debunked (`docs/ANIMAL_ACTIVITY_DETECTION.md`).
  **A disaster output is unrepresentable, and a test asserts the frame's size.**
* Outputs are observations (`activity_change_detected`, `audio_activity_deviation`,
  `possible_animal_activity`) — **never a prediction**. `possible_animal_activity` has
  exactly one path: a sustained motion change plus corroboration.
* Environmental correlation is **Category B** evidence, and the code states so:
  *"plausible but incompletely validated correlation … correlation is stated as
  correlation, not causation."*
* The doc additionally rejects gecko/frog folk weather prophecy and p-hole/radon
  ionisation precursors.

### What the Mi A3 can therefore contribute, and what it cannot

| Contribution | Channel | Where it lands |
|---|---|---|
| **Day/night and light-state context** | `illuminance` | `EnvironmentalSample` → `EnvironmentalCorrelation` → observation, Category B. Correlating a *motion/audio* change against a light change is a legitimate, bounded use. |
| **Presence / activity change** | `significant_motion`, `motion_detect`, `accelerometer` | `SecurityObservation` (`source_type=motion`) → the existing detector path that can reach `possible_animal_activity` when combined with audio and a vector shift. |
| **Tamper / moved indicator** | `stationary_detect`, orientation change | security path |
| **Local weather** | — | **Not possible.** No pressure, humidity or temperature sensor. |
| **Geographic context** | — | **Not from this phone** (no location permission, none requested). It must keep coming from the existing external source. |

**So the honest recommendation is narrow:** wire the light sensor in as an
`illuminance` `EnvironmentalSample` under the existing authorization model, wire motion
sensing in as `SecurityObservation` motion transitions, and explicitly **do not** create
a local weather or geographic channel. Adding a second, phone-derived "environment"
model alongside the Home-Assistant/weather one would duplicate a source of truth and
would be exactly the kind of unsupported correlation the existing documentation was
written to prevent.

---

## 11. What would need separate approval (none requested)

| Item | Why it is needed | Status |
|---|---|---|
| `ACTIVITY_RECOGNITION` | step detector/counter — verified `prot=dangerous\|instant`, so a runtime prompt | **not requested** |
| `ACCESS_FINE_LOCATION` | the only way to scan BLE on API 30 (`BLUETOOTH_SCAN` does not exist here) | **not requested** |
| GNSS position collection | geographic context from the device | **not requested** |
| External BLE or wired sensor hardware | the only route to temperature/humidity/pressure | **out of scope per the brief; nothing purchased** |
| CAMERA / RECORD_AUDIO | not implicated by this phase at all | **not requested** |

---

## 12. Documentation impact

* This file is the sensor discovery deliverable. It is a new document; no existing
  document was rewritten.
* `docs/EDGE_DEVICE_MI_A3.md` already carries the device's hardware/software discovery,
  the managed-service implementation and the reliability results; this phase extends the
  same device's capability record and should be read alongside sections 1 and 10 there.
* The `docs/ROADMAP.md` HG-EDGE section now carries a HG-EDGE-03 entry summarising this
  phase, appended to the block that already records HG-EDGE-01/02 under the same
  "experimental, unmerged" status. The standing note that the documentation owner
  decides whether the HG-EDGE workstream becomes a numbered phase still applies.
  **Nothing was inserted as a numbered control-plane row** (AGENTS.md rule 9).
* One correction worth carrying forward: the earlier HG-EDGE reports describe the
  device's thermal zones as a monitoring channel. They are, but **only for the phone's
  own health** — this document is the explicit statement that they are never an
  environmental reading.

---

## Appendix — evidence

| Item | Where |
|---|---|
| Full sensor dump (439 lines, at capture) | device: `dumpsys sensorservice`; hard-copied to `/tmp/sensors_raw.txt` on the host during this session |
| Sensor table extraction | derived from the dump above; the extraction script and its output are in this session's log |
| Absent-type proof | full-dump search for `ambient_temperature`, `pressure`, `relative_humidity`, `temperature` → 0 each |
| Thermal zone list | `for z in /sys/class/thermal/thermal_zone*/; do cat $z/type; done` → 60+ zones, all SoC/battery/board |
| Permission facts | `pm list permissions` for `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT` → 0; `dumpsys package android` → `ACTIVITY_RECOGNITION … prot=dangerous\|instant` |
| Node cost baseline | `docs/EDGE_DEVICE_MI_A3.md` §10.10 |
| Existing environmental contract | `src/core/EnvironmentalCorrelation.h`, `EnvironmentalBounds::plausible()` |
