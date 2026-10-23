# Home Security Sensor-Node Integration (S-04)

**Source skill:** `home-security-sensor-nodes` v1.0.0 (MIT) -
`~/.hermes/skills/smart-home/home-security-sensor-nodes/SKILL.md`
**Milestone:** D-02x, branch `feature/skills-detection-integration`
**Scope:** the observation vocabulary and adapters for security-relevant events, built on the
acquisition path D-02 already ships.

---

## 1. Adopted

| Adopted | Why | Implementation |
|---|---|---|
| Phones as sensor terminals, **inference on the hub** | The skill's core architecture; it is also what makes old hardware adequate | Already HomeGuardian's design: D-02 acquires, the hub decides |
| The skill's exclusion boundary, verbatim | "Pointing an emotion model at family members or visitors who did not consent converts a security tool into ongoing psychological surveillance" | No facial-emotion inference, no voice stress/deception analysis, no tremor or thermal "stress" profiling, no identity matching anywhere in this layer |
| Zone/cooldown discipline as **de-duplication** | The skill calls cooldown the single biggest false-alarm reducer | `SensorDeduplicator` (bounded, per device + source type + state, cooldown window) |
| Liveness vs silence | "Set a Last Will so a dead sensor is distinguishable from a silent one - that distinction is the entire safety argument" | `DeviceAvailabilityTracker` |
| Event-level observance, not psychology | The legitimate version of behaviour-change detection models events and routines | Every observation is an event record with provenance and consent context |
| Baseline before anomaly alerts | "Step 7 is the one people skip and regret" | The existing `AnomalyMonitor` refuses to flag during warm-up (`inconclusive`), unchanged |

## 2. Adapted

| Adapted | What changed, and why |
|---|---|
| The skill's streaming stack (IP Webcam/RTSP + Frigate NVR) | **Not adopted.** HomeGuardian already has a native acquisition path with per-payload consent; adding RTSP + a separate NVR service would mean raw-media retention and an extra background service, both excluded by the project's constraints. The skill's *observation vocabulary* was taken instead |
| Hub-side detection models (YOLOv8n person/vehicle, MediaPipe Pose fall, YAMNet/PANNs sound events) | Deferred: runtimes and weights are absent and require an approved review. What replaces them today is bounded motion/audio measurement with no classification claim |
| Known-vs-unknown face matching | Not implemented. It is identity inference about people; it needs consent and legal review, and the milestone's own D-list does not include it |
| Isolation Forest routine anomaly | Adapted to the existing control-chart-equivalent (`AnomalyMonitor`, z-score over a bounded window) which is already wired into the pipeline |
| The confirm/dismiss self-learning loop | Deferred: it needs a feedback surface and storage. Thresholds are plain configuration so a bad tune can be rolled back |

## 3. Rejected

| Rejected | Reason |
|---|---|
| Facial-emotion, voice-stress/deception, tremor and thermal "stress" inference aimed at people | Fails independent validation (~75-79% even in controlled lab conditions) **and** is a consent problem regardless of accuracy |
| Identity matching / face recognition | Out of scope; needs explicit approval, an enrolled-consent story and a legal review |
| Fall detection claims | Needs pose estimation (absent) and is explicitly excluded from this milestone: camera motion is never presented as a fall |
| Actuator control (sirens, locks, cameras) | Never in scope without separate explicit approval |
| Frigate as a hub component | A separate service with raw-media retention; conflicts with "no unnecessary background services" and "no raw media persistence by default" |

## 4. The observation model

`src/core/SecurityObservation.{h,cpp}` - every record carries the fields the milestone requires:
device identifier, source type, acquisition timestamp, ingestion timestamp, real/synthetic
provenance, optional confidence/quality, consent subject + purpose + authorization decision, and
a bounded validated feature payload.

| Observation | Detector id | Status mapping | Severity |
|---|---|---|---|
| Motion detected | `sensor.motion` | on/detected -> detected; off/clear -> not_detected | info |
| Door/window state | `sensor.contact` | open -> detected; closed -> not_detected | info |
| Presence/occupancy change | `sensor.presence` | present -> detected; absent -> not_detected | info |
| Camera availability | `sensor.camera_availability` | unavailable/offline/error -> **detected** (the problem is the finding) | warning |
| Microphone availability | `sensor.microphone_availability` | as above | warning |
| Device offline/online | `sensor.device_availability` | offline -> detected; online -> not_detected | warning / info |
| Tamper or unexpected disconnect | `sensor.tamper` | tamper/unexpected_disconnect -> detected | warning |
| Environmental threshold crossing | `sensor.environmental_threshold` | crossed/above/below -> detected | info |

An **unauthorized** report is stored with status `unavailable` and claims no detection; its
consent context travels with it so an audit can see it was refused. `suggested_severity()` has no
branch returning `Severity::critical`, asserted across the full source-type x state matrix.

`from_json` / `to_json` provide a strict wire form for the D-03 transport: type-checked,
bounded, unknown enumerations rejected rather than defaulted, and `acquired_at` may not be after
`ingested_at`.

## 5. Reuse of existing HomeGuardian components (no duplicate interfaces)

The skill's node stack maps onto what already exists, so nothing was re-implemented:
`IMediaDevice`, `ConsentGuardedDevice`, `Camera1Bridge`/Camera2 NDK backend,
`AAudioCaptureDevice`, `CaptureAccounting`, `NodeIdentity` (device identity), the Docker hub,
SQLite persistence, and the `Observation`/`Event` pipeline.

## 6. Tests

| Suite | Command | Result |
|---|---|---|
| Observation model, status mapping, severity matrix, wire format, liveness, de-duplication | `homeguardian_tests "[sensors]"` | 7 cases / 487 assertions |
| Multi-device simultaneity, unauthorized/withdrawn sources, resource bounds | `homeguardian_tests "[gaps]"` | 4 cases / 84 assertions |
| Pipeline integration (advisory by default, opt-in alerting) | `homeguardian_tests "[integration][skills]"` | 7 cases / 65 assertions |

Specifically proven: a healthy device is silent per heartbeat; silence past the timeout yields
exactly one offline transition; a reconnect is an availability transition and never a state
replay; the same event from three devices is delivered three times while a repeat from one device
is suppressed; a 401 from an upstream source yields errors and zero observations; no sensor
observation from any device at any state can reach `critical`.

## 7. Measured performance

Measured on this host (Debug for behaviour, Release for size):

- `[sensors]` 7 cases / 487 assertions and the whole host suite (see
  `docs/SKILL_VALIDATION_REPORT.md` section 10) pass; 0 sanitizer reports under ASan+UBSan.
- Cost of the model itself is arithmetic over a bounded payload: no allocation in the extractors,
  a bounded registry (64 devices by default), a bounded dedup table (<= 256 entries) and a bounded
  feature payload (<= 1024 bytes, <= 16 keys).
- Sustained-input bounds are asserted: 500 device registrations stay within the configured cap,
  500 dedup insertions stay within theirs, and a 5000-sample activity stream keeps the detector
  correct with a bounded history.

## 8. Missing hardware and remaining validation

| Item | Blocked by |
|---|---|
| Any live security observation | No phone attached (D-02 physical acceptance BLOCKED) and no node transport yet (D-03) |
| Person/vehicle classification | No detector runtime and no approved model |
| Sound-event classes (glass break, alarm beep) | Needs an approved audio-event model; a loud-bang heuristic is documented in the skill as high-false-positive and is deliberately not implemented |
| Fall detection | Out of scope for this milestone (needs pose estimation) |
| Retention of a confirmed event's footage | The project stores metadata only; raw media is never persisted by default |

**Remaining validation requirement:** a real deployment needs a measured false-positive rate per
zone over a baseline period (the skill's own 2-4 week guidance) before any alerting is enabled on
live data, plus an operator-facing confirm/dismiss loop to drive threshold tuning. Neither exists
yet, so the security observations remain advisory and the hub reports zero connected devices until
D-03 lands.
