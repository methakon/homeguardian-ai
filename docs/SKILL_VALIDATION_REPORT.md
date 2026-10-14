# Skill Validation Report (S-01, milestone D-02x)

**Branch:** `feature/skills-detection-integration` (created from the verified D-02 tip
`3c8ff46`, `feature/distributed-homeguardian`)

**Companion documents** (implementation detail, tests, measurements and remaining validation
requirements per skill):

| Document | Covers |
|---|---|
| `docs/ANIMAL_ACTIVITY_DETECTION.md` | S-03: the precursor/activity layer - adopted, adapted, rejected, files, tests, measured behaviour, missing hardware |
| `docs/HOME_SECURITY_SENSOR_INTEGRATION.md` | S-04: the security observation vocabulary and adapters |
| `docs/HOME_ASSISTANT_INTEGRATION.md` | S-05: the read-only Home Assistant adapter and its transport |
**Scope:** inspect three installed skills, validate what they claim, and implement only the
parts that are technically feasible, testable and honest. Nothing here predicts a calamity.

---

## 0. What this milestone is, and what it is not

| Statement | Status |
|---|---|
| A bounded C++20 detection layer exists for activity change, audio-energy deviation, vector drift and multi-sensor correlation | **IMPLEMENTED** (`src/core/ActivityDetectors.*`, `PrecursorDetection.*`, `DempsterShafer.*`) |
| A validated security-sensor observation vocabulary exists and flows through the existing pipeline | **IMPLEMENTED** (`src/core/SecurityObservation.*`) |
| A native, read-only Home Assistant adapter exists, disabled by default | **IMPLEMENTED** (`src/core/HomeAssistantAdapter.*`, `HttpClient.*`) |
| Species identification (gecko/rodent/dog/bird) | **NOT IMPLEMENTED** - no defensible classifier or dataset |
| YAMNet / YOLOv8 / MediaPipe pose / Farneback optical flow / Frigate | **NOT IMPLEMENTED** - runtimes absent; model weights require a licensing + resource review the project forbids skipping |
| Earthquake / flood / storm prediction from animal behaviour | **NOT IMPLEMENTED, BY CONSTRUCTION** - the DST frame of discernment has no such hypothesis, so the code cannot emit one |
| Physical phone or Home Assistant instance exercised | **NOT DONE** - no device attached; no Home Assistant instance exists here |

Every number in this document is from an executed command on this host. Where a value is a
proposal rather than a measurement it says **acceptance target, not measured**.

---

## 1. Skill discovery (exact identifiers and paths)

| Requested name | Found? | Exact path / identifier |
|---|---|---|
| `animal-calamity-precursor-detection` | **YES** | `~/.hermes/skills/multimodal-affective-computing/animal-calamity-precursor-detection/` (SKILL.md 13,047 B; `references/cpp-pipeline.md` 12,632 B; `references/plan-only-and-correction-ledger.md` 6,669 B). v1.0.0, MIT, author Swarna Sekhar Dhar |
| `home-security-sensor-nodes` | **YES** | `~/.hermes/skills/smart-home/home-security-sensor-nodes/SKILL.md` (11,494 B). v1.0.0, MIT, author Swarna Sekhar Dhar |
| `Home Assistant hub` | **NO SKILL EXISTS** | No skill, plugin or catalog entry carries that identifier. Nearest artefacts, inspected instead: (a) the installed Hermes **plugin** `homeassistant` at `~/.hermes/plugins/homeassistant/` - `plugin.yaml` name `homeassistant`, label "Home Assistant", kind `platform`, v2.0.1, author NousResearch, homepage `github.com/NousResearch/hermes-homeassistant`, license MIT, catalog sha `ba30cb0cf86c52bdb5cde98974bc062e97966529`; (b) the `home-security-sensor-nodes` skill's "Tooling" section, which treats Home Assistant as the hub |

**Consequence of the missing skill.** The Home Assistant work below was specified from the
*installed plugin's* actual mechanisms (its `plugin.yaml`, `tools.py`, `adapter.py` and tests)
plus the milestone brief, not from a `Home Assistant hub` skill that does not exist. The
plugin is a **Python gateway for the Hermes agent** (four tools: `ha_list_entities`,
`ha_get_state`, `ha_list_services`, `ha_call_service`; a WebSocket event subscription that
forwards state changes to the agent; replies via `notify.notify`). It is the wrong layer and
the wrong runtime for the C++ hub, and it is **not** a dependency of this milestone.

---

## 2. Skill Compatibility Matrix

### 2.1 `animal-calamity-precursor-detection`

| Field | Evaluation |
|---|---|
| Skill name | `animal-calamity-precursor-detection` v1.0.0 (MIT) |
| Version/source | Installed locally; derived from an Alexa/ESP32 design document (`~/Downloads/trading_agent_inputs/alexa/Alexa Animal Calamity Detection App.pdf`, 271 lines, 77 citations) plus its own correction ledger |
| Main functions | IoT sensing + sensor fusion + alert delivery for fauna-anomaly detection; C++ port path; Alexa Proactive Events delivery; ethnozoological folklore grading |
| Relevant detection methods | (1) bounded audio energy + log-mel; (2) YAMNet 521-class / 1024-d embedding transfer learning; (3) YOLOv8 individuals; (4) Farneback dense optical flow for swarms; (5) unsupervised **cosine vector drift from a baseline centroid** ("vector before labels"); (6) **Dempster-Shafer** fusion with conflict mass K and an ignorance model; (7) MQTT/Alexa delivery |
| Existing HomeGuardian overlap | `Observation`/`Event` models, `ConsentGate`, `ConsentGuardedDevice`, `Pipeline` + `ICorrelationRule`, `DetectionFusionRule` evidence allowlist, `AnomalyMonitor` (z-score), `FusionEngine`, `AlertPolicy`, SQLite persistence - all REUSED; no second event engine was created |
| New dependencies | **None.** Every adopted method is pure C++20 with `<cmath>`/`<chrono>` only. The model-dependent methods (YAMNet/ONNX Runtime, OpenCV, TensorFlow) were NOT added: ONNX Runtime + AudioSet weights would need a licensing/resource review; OpenCV is absent. Zero new third-party code |
| Security/privacy | Audio is handled only as bounded int16 PCM passed to a scalar extractor; **nothing is recorded, stored, transcribed or transmitted**; no speech/voice/species/emotion analysis. Environmental samples require an explicit authorization context string and are refused without a recorded grant |
| Scientific reliability | Mixed, and the skill grades it honestly: "vector drift" and DST are sound engineering; infrasound storm detection is proven; rodent USV literature is well characterised; **ant seismology is debunked** and the Haicheng story is overstated (the skill's own ledger says so) |
| Integration decision | **ADOPT (adapted)**: bounded features, activity baseline, vector drift, DST with K, environmental correlation. **DEFER**: YAMNet/embeddings, YOLO, optical flow, MQTT, Alexa. **REJECT in an alerting path**: seismic/calamity hypotheses. **REJECT**: species classification |

### 2.2 `home-security-sensor-nodes`

| Field | Evaluation |
|---|---|
| Skill name | `home-security-sensor-nodes` v1.0.0 (MIT) |
| Version/source | Installed locally; derived from `Home_Family_Security_Sensing_Agent_Guide.pdf` (Sept 2026) |
| Main functions | Old phones as IP-camera/mic/motion nodes; Frigate NVR + Home Assistant hub; person/vehicle/zone logic; known-vs-unknown face matching; fall detection; sound-event detection; routine baselining; ethics/consent guardrails |
| Relevant detection methods | Person/vehicle detection (YOLOv8n / MobileNet-SSD); polygon zones + dwell time; face embedding + cosine match; MediaPipe Pose fall heuristic; YAMNet/PANNs sound events; Isolation Forest / control-chart routine anomaly; confirm/dismiss self-learning |
| Existing HomeGuardian overlap | Direct: `IMediaDevice`, `ConsentGuardedDevice`, `Camera1Bridge`, `Camera2` NDK backend, `AAudioCaptureDevice`, `CaptureAccounting`, `NodeIdentity`, `DeviceAvailabilityTracker`, `SensorDeduplicator`, `AnomalyMonitor`, the Docker hub, SQLite |
| New dependencies | None added. Frigate (a separate service), MediaPipe, YOLO, face_recognition/DeepFace, Frigate - all DEFERRED |
| Security/privacy | The skill's own boundary is adopted verbatim: **no** facial-emotion inference, **no** voice stress/deception analysis, **no** body-vibration or thermal "stress" profiling aimed at people, and identity matching is NOT implemented. Observations carry consent context and authorization state; an unauthorized report claims no detection |
| Scientific reliability | The event-level capabilities are well established (motion/person detection, glass-break audio events). The skill explicitly documents that deception/voice-stress analysis fails independent validation (~75-79% lab accuracy) and is a consent problem regardless |
| Integration decision | **ADOPT (adapted)**: the event-not-psychology boundary, the observation vocabulary (motion, contact, presence, availability, tamper, threshold), liveness vs silence distinction, de-duplication/cooldown, hub-side-only inference. **DEFER**: Frigate, face matching, fall detection, sound-event models, self-learning loop. **REJECT**: any person-directed affective profiling |

### 2.3 `Home Assistant` (the missing skill's replacement target)

| Field | Evaluation |
|---|---|
| Skill name | **NOT FOUND** as a skill. Installed plugin `homeassistant` v2.0.1 (MIT, NousResearch) used as the specification source |
| Version/source | `~/.hermes/plugins/homeassistant/`, catalog sha `ba30cb0cf86c52bdb5cde98974bc062e97966529`, installed 2026-10-04 |
| Main functions | Agent-side gateway: REST tools (`ha_list_entities`, `ha_get_state`, `ha_list_services`, `ha_call_service`) + WebSocket event subscription + `notify.notify` replies |
| Relevant detection methods | None of its own - it exposes entity state and an event bus |
| Existing HomeGuardian overlap | `Observation`/`Event`, `SecurityObservation`, availability tracking, de-duplication, consent context, the existing pipeline |
| New dependencies | **None**: a hand-written HTTP/1.1 GET client (`HttpClient.*`) over POSIX sockets, matching the project's existing no-third-party-networking stance (cpp-httplib was previously rejected). No Python at runtime, no SDK |
| Security/privacy | Read-only (`GET /api/states/<entity_id>` only; no service calls, no actuation). Disabled by default. Requires `allow_plaintext=true` because no TLS is linked in this build - an `https://` URL is REFUSED explicitly rather than downgraded. Token read from a mounted secret file **refused if group/other-readable**; the token lives only inside the transport, never in a log, an observation, a feature payload, `describe()` output, or an image layer. Entity allowlist is explicit; an unmapped entity is skipped, never guessed |
| Scientific reliability | Not applicable (a state source, not a detector) |
| Integration decision | **ADOPT (native C++ REST adapter, read-only, default-off)**. **DEFER**: WebSocket event subscription, MQTT. **REJECT**: using the Python agent plugin as a hub dependency; any actuator control (locks/alarms/sirens/cameras) without separate approval |

---

## 3. Animal and natural-hazard precursor detection

### 3.1 Evidence classification

**Category A - established observations** (implemented as measured quantities, no interpretation):
bounded audio energy (RMS/peak/zero-crossing rate) from permitted PCM; per-pixel motion change
(changed-pixel fraction, mean absolute delta) between permitted luma planes; sustained activity
level change against a learned baseline; audio-energy deviation via the existing statistical
monitor.

**Category B - plausible but incompletely validated correlations** (implemented, `info` only,
never an alert, labelled as correlation): association between a detected change and an
authorized environmental reading (temperature/humidity/pressure/illuminance) inside a bounded
time window, with the measured lag reported.

**Category C - folklore and speculative precursors** (documented as hypotheses, **no code
path**): haicheng/tangshan-style animal earthquake prediction, ant seismology, p-hole/radon/
ionisation precursors, gecko-call meaning (*tiktiki*), frog/toad and fox vocalisation as
drought/flood signals. The DST frame of discernment has three elements and none of them is a
calamity, so such an output is unrepresentable in this build (asserted by a test).

### 3.2 Per-method records

Legend for **Output**: O = observation, A = advisory/anomaly, C = correlation, X = explicitly
not produced.

| Animal / event class | Modality | Observable features | Detection logic | Hardware floor (from the skill) | Window | Environmental confounders | Main false-positive risk | Evidence quality | Validation dataset | CPU/RAM | Privacy | Output |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Any household fauna (activity level) | Camera luma | changed-pixel fraction, mean delta | EMA baseline + sustained-ratio confirmation | phone camera (implemented path) | 3+ consecutive windows | lighting change, shadows, IR cut | lighting switch, moving curtain | A (measured) | none needed | O(pixels), no alloc | no frames retained | O + A |
| Any household fauna (acoustic activity) | PCM mic | RMS, peak, ZCR | EMA baseline + z-score deviation | phone mic (implemented path) | 8+ samples | HVAC, appliances, traffic | TV/appliance noise | A (measured) | none needed | O(samples), no alloc | no audio retained | O + A |
| Regime shift in *any* bounded feature vector | any feature vector | cosine distance from baseline centroid | sustained cosine drift (direction, not magnitude) | none (pure math) | 3+ drifted windows | baseline drift itself | slow legitimate change | A (method) | none needed | O(dim) | none | O + A |
| Multi-sensor agreement | cross-modal | belief/plausibility/ignorance, conflict K | Dempster-Shafer fold, suppress when K>0.9 | none (pure math) | per decision | source disagreement | conflating "loud" with "meaningful" | A (method) | none needed | O(8) | none | C (fusion hypothesis) |
| Pressure/humidity-linked activity (ants, amphibians - Khonar Bachan) | env sensor + camera/mic | environmental value, coincident change | windowed association with lag | env sensor entity | <=10 min window, <=5 min lag | seasonality, cooking, opening doors | coincidence | **B** (plausible, incompletely validated) | none available | negligible | env readings only | C |
| Ant brood migration / ant seismology | - | - | - | INMP441-class mic, barometer | - | - | **debunked as seismic precursor** (Landers: reaction only above M2.0) | C | none | - | - | **X** |
| Cockroach swarming | airflow/vision | - | - | pressure/flow sensor | - | - | skill calls it "weakest evidence of the set" | C | none | - | - | **X** |
| Rodent ultrasonic vocalisation (22 kHz) | ultrasonic mic | USV band | - | INMP441/ultrasonic-capable mic | - | - | phone mics roll off far below 22 kHz | A for the literature, **not achievable on this hardware** | UltraVox literature | - | - | **X** (hardware) |
| Dog agitation / howling (pre-quake) | accelerometer/audio | - | - | - | - | - | mechanism sound, thresholds uncalibrated; "dog predicts disaster" is exactly the claim forbidden | C | none | - | - | **X** |
| Bird infrasound (storm) | infrasound mic | 0.05-10 Hz | - | dedicated infrasound sensor | - | - | phone mics cannot capture infrasound | A for storm detection, hardware absent | - | - | - | **X** (hardware) |
| House gecko call rate | audio | ~3.4 kHz peak | - | general mic | - | - | folklore meaning is superstition; only an ecosystem-activity proxy | C (meaning) / A (call rate) | none | - | - | **X** (no species claim) |

**The honest summary:** the only channels reachable with the hardware HomeGuardian actually has
(a phone camera and microphone) are **activity level** and **acoustic energy** - Category A
measurements with no species content - plus the Category B environmental correlation. Every
species-specific channel needs hardware the project does not have (ultrasonic-capable or
infrasound microphones), and every disaster-precursor channel is Category C.

### 3.3 The bounded first detection layer (implemented)

| # | Requested detector | Implementation | Evidence |
|---|---|---|---|
| 1 | Activity baseline detector | `ActivityChangeDetector` - slow EMA baseline vs recent window, direction + ratio, confirmation over `confirm_samples` | `tests/core/test_precursor_detection.cpp` `[precursor][activity]` |
| 2 | Temporal deviation detector | **reuses the existing `AnomalyMonitor`** (z-score) - not re-implemented | `[precursor][suite]`, detector id `audio-activity-deviation` |
| 3 | Audio activity detector | `extract_audio_features()` - RMS/peak/ZCR, bounded, refusal with a reason | `[precursor][features]` |
| 4 | Motion/event detector | `extract_motion_features()` + NV21 geometry validation (Y-plane only) | `[precursor][features]` |
| 5 | Multi-source correlation | `PrecursorSuite` folding motion/audio/environmental evidence with DST; `CorrelationWindow` for time association | `[precursor][dst]`, `[precursor][environmental]` |
| 6 | Environmental correlation adapter | `EnvironmentalAdapter` + `CorrelationWindow` (validation, window, lag) | `[precursor][environmental]` |

### 3.4 Result vocabulary (only these, enforced by tests)

`activity_change_detected`, `audio_activity_deviation`, `possible_animal_activity`,
`environmental_correlation`, `insufficient_evidence`.

`insufficient_evidence` covers both "not enough data" and **"the sources contradict each
other"**: when the conflict mass K exceeds 0.9 the fusion is suppressed rather than averaged,
because a contradiction is not evidence.

`possible_animal_activity` is reachable only through one path, and the code comments say it:
a sustained motion change **and** a sustained audio change (or a flagged audio deviation),
**plus** a confirmed vector-drift regime shift, **plus** at least two independent confirming
sources, **and** no environmental reading strong enough to explain the change. Its own
explanation string says "HYPOTHESIS ONLY ... NOT a species identification", and a test asserts
that text survives into the observation.

---

## 4. Home security sensor-node integration (implemented adapters)

`SecurityObservation` carries every field the brief requires - device identifier, source type,
acquisition timestamp, ingestion timestamp, real/synthetic provenance, confidence/quality,
consent subject + purpose + authorization decision, and a bounded validated feature payload -
and converts into the existing `Observation` -> `Event` path via `to_observation()`.

| Observation | Source type | Detector id | Status mapping | Conservative severity |
|---|---|---|---|---|
| Motion detected | `motion` | `sensor.motion` | on/detected -> detected; off/clear -> not_detected; else inconclusive | info |
| Door/window state | `contact` | `sensor.contact` | open -> detected; closed -> not_detected | info |
| Presence/occupancy change | `presence` | `sensor.presence` | present/on -> detected; absent/off -> not_detected | info |
| Camera availability | `camera_availability` | `sensor.camera_availability` | unavailable/offline/error -> **detected** (the problem is the finding) | warning |
| Microphone availability | `microphone_availability` | `sensor.microphone_availability` | as above | warning |
| Device offline/online | `device_availability` | `sensor.device_availability` | offline -> detected; online -> not_detected | warning (offline) / info |
| Tamper or unexpected disconnect | `tamper` | `sensor.tamper` | tamper/unexpected_disconnect -> detected | warning |
| Environmental threshold crossing | `environmental_threshold` | `sensor.environmental_threshold` | crossed/above/below -> detected; within/normal -> not_detected | info |

Supporting components:

- **`DeviceAvailabilityTracker`** - the skill's "a dead sensor must be distinguishable from a
  silent one" is implemented literally: `mark_seen()` returns nothing while a device is
  healthy, so liveness does not generate events; silence past `offline_after` produces exactly
  one offline transition; a reconnect produces an **availability** transition only and never a
  replay or an invented state event.
- **`SensorDeduplicator`** - bounded (device, source type, state) cooldown suppression.
- **No fabricated telemetry**: every observation is either produced by a real source (the node,
  a Home Assistant entity) or marked `Provenance::synthetic` for fixtures. Nothing is invented.

---

## 5. Home Assistant adapter (behaviours 1-10, and where each is proven)

| # | Required behaviour | Implementation | Test |
|---|---|---|---|
| 1 | Configure the instance address | `HomeAssistantConfig::base_url` | `[ha][config]` |
| 2 | Store credentials securely | `read_secret_file()`: regular file, <=4096 B, **refuses group/other-readable**, trims newline; token private to the transport | `[ha][secret]` |
| 3 | Discover permitted entities | explicit `entity_allowlist`; nothing else is requested | `[ha][adapter]` (asserts every request path) |
| 4 | Allowlist what may be consumed | allowlist + entity-id shape validation; unmapped entity skipped with a reason | `[ha][mapping]` |
| 5 | Convert authorized updates into validated observations | `map_entity()` + `SecurityObservation::validate()` + `to_observation()` | `[ha][mapping]`, `[ha][adapter]` |
| 6 | Track online/offline | `DeviceAvailabilityTracker` on successful reads | `[ha][adapter]` |
| 7 | Reconnect without fabricating events | reconnect -> availability observation only (`first_seen=false`) | `[ha][adapter]` "a reconnect yields an availability observation only" |
| 8 | Avoid duplicate ingestion | `SensorDeduplicator` + deterministic observation id | `[ha][adapter]` "an unchanged second poll is fully de-duplicated" |
| 9 | Preserve real-vs-synthetic provenance | `Provenance::real` for HA entities; synthetic fixtures stay synthetic | `[ha][adapter]`, `[integration][skills]` |
| 10 | Disable by default until configured | `enabled=false`; `poll_once()` refuses when disabled, misconfigured, or unauthorized, performing **no request at all** | `[ha][adapter]` (three sections assert zero requests) |

Security posture, stated plainly: **read-only** (`GET /api/states/<entity_id>`; there is no
POST/PUT/DELETE code path, so the adapter cannot actuate anything); **no TLS in this build**, so
an `https://` base URL is refused and an `http://` endpoint additionally requires
`allow_plaintext=true`; the bearer token is never logged, never placed in an observation, never
returned by `describe()`, and never baked into an image layer; header values containing CR/LF
are rejected outright rather than sanitised; responses are capped at 64 KiB with a socket
timeout.

**Deferred:** WebSocket event subscription (REST polling is the smaller surface and needs no
framing implementation), MQTT, actuator control, and any Home Assistant add-on.

---

## 6. Unified observation model and integration

```
Android camera/mic (D-02) ─┐
Home Assistant entities  ──┤→ SecurityObservation / PrecursorSuite observations
Synthetic fixtures       ──┘        │  (validate: bounds, timestamps, provenance, consent)
                                    ▼
                             Observation::to_event()
                                    ▼
                    EXISTING Pipeline (+ ICorrelationRule set)
                                    ▼
        DetectionFusionRule (EXPLICIT evidence allowlist) + AlertPolicy
                                    ▼
                   SQLite persistence / native HTTP / dashboard
```

- **No second alert engine.** Every new detector produces `Observation`s that convert to the
  standard `Event` and go through the existing `Pipeline`, rules and `AlertPolicy`.
- **`EventValidator`, `Pipeline`, `AlertPolicy` and the consent enforcement are not bypassed.**
- **Compatibility with historical synthetic scenarios is preserved by construction**: the new
  detector ids are not in `DetectionFusionRule`'s default evidence allowlist, so the ten v17/v21
  scenarios and the H-01s impact scenario behave exactly as before. A test asserts the default
  allowlist membership explicitly (`[integration][skills]`).
- **Opt-in is explicit**, using the existing L-12 mechanism: `set_evidence_detector_ids({...})`.
  A test shows that allowlisting `motion-activity` + `audio-activity` makes the same
  observations alert, and that they never exceed `warning`.
- **Target registration is explicit and measured**: the new translation units are registered
  in the `homeguardian_tests` target, where they are referenced. They are deliberately NOT
  linked into `homeguardian` or `hg_alexa_bridge` yet, because nothing in those binaries calls
  them - and the Release sizes prove it (`homeguardian` 3,210,648 B and `hg_alexa_bridge`
  3,342,280 B are byte-identical to the D-02 baseline). The hub-side ingestion route (D-03's
  authenticated transport, or the D-04 node registry) is the point at which they get registered
  there. Adding unreferenced translation units now would grow the shipped footprint for no
  behaviour.
- **Schema extended only additively**: `SecurityObservation` is a new type that maps onto the
  existing `Observation`; no existing field changed meaning and no existing fixture needed
  editing.

---

## 7. Severity and uncertainty model

- **info** - a verified device event or a routine observational change (all motion, contact,
  presence and threshold observations; all correlation results).
- **warning** - a validated abnormality or credible security concern with supporting evidence
  (tamper/unexpected disconnect; a device, camera or microphone that has become unavailable).
- **critical** - **unreachable from this milestone's code.** `SecurityObservation::suggested_severity()`
  has no branch returning `critical`, asserted across the full type x state matrix; the fusion
  rule maps fused confidence only to info/warning. The existing `synthetic-critical-impact`
  scenario remains the only critical path and is explicitly synthetic.
- Animal-behaviour anomalies **cannot** raise a critical or disaster warning: the strongest
  label available is a hypothesis at `info`-grade severity.
- No camera motion, audio energy or statistical deviation is presented as proof of a species, an
  intruder, a real fall, or an impending calamity - in the code, in the features, in the notes,
  or in the docs.

---

## 8. Proof-bearing capability inventory

`EXISTS` = implemented and tested here. `UNVERIFIED` = could not be checked this session.

| Capability | Status | Proof |
|---|---|---|
| Bounded audio/motion feature extraction | EXISTS | `src/core/BoundedFeatures.cpp:extract_audio_features/extract_motion_features`; `tests/core/test_precursor_detection.cpp` `[precursor][features]` |
| Activity baseline detector | EXISTS | `src/core/ActivityDetectors.cpp:ActivityChangeDetector::observe`; `[precursor][activity]` |
| Vector drift (cosine) detector | EXISTS | `src/core/ActivityDetectors.cpp:VectorDriftDetector::observe`; `[precursor][vector]` |
| Dempster-Shafer fusion + conflict K | EXISTS | `src/core/DempsterShafer.cpp:combine/belief/plausibility`; `[precursor][dst]` |
| Precursor suite + result vocabulary | EXISTS | `src/core/PrecursorDetection.cpp:PrecursorSuite::fusion/observations`; `[precursor][suite]` |
| Environmental correlation adapter | EXISTS | `src/core/EnvironmentalCorrelation.cpp:EnvironmentalAdapter/CorrelationWindow`; `[precursor][environmental]` |
| Security observation model + strict wire ingest | EXISTS | `src/core/SecurityObservation.cpp:validate/to_observation/from_json`; `[sensors][observation]`, `[sensors][json]` |
| Availability tracker (dead vs silent) | EXISTS | `src/core/SecurityObservation.cpp:DeviceAvailabilityTracker`; `[sensors][availability]` |
| De-duplication with cooldown | EXISTS | `src/core/SecurityObservation.cpp:SensorDeduplicator`; `[sensors][dedup]` |
| Home Assistant config fail-closed | EXISTS | `src/core/HomeAssistantAdapter.cpp:HomeAssistantConfig::problems`; `[ha][config]` |
| Secure secret file reading | EXISTS | `src/core/HomeAssistantAdapter.cpp:read_secret_file`; `[ha][secret]` |
| Entity -> observation mapping | EXISTS | `src/core/HomeAssistantAdapter.cpp:map_entity`; `[ha][mapping]` |
| Read-only polling with dedup + liveness | EXISTS | `src/core/HomeAssistantAdapter.cpp:poll_once`; `[ha][adapter]` |
| HTTP/1.1 GET client (plaintext-only, capped) | EXISTS | `src/core/HttpClient.cpp`; `[ha][http]` incl. a real loopback exchange |
| Pipeline integration, advisory-by-default | EXISTS | `tests/core/test_observation_integration.cpp` `[integration][skills]` |
| Real Home Assistant instance end-to-end | MISSING (no instance exists here) | would require a configured instance + token; the transport seam is designed for it |
| YAMNet embedding extractor | MISSING | no ONNX Runtime, no weights; requires a licensing/resource review |
| Optical flow / YOLO / MediaPipe pose | MISSING | no OpenCV/MediaPipe on this host |
| Species classifier (gecko/rodent/dog) | MISSING | no defensible dataset |
| MQTT delivery | MISSING | deferred; no dependency added |
| Physical phone exercising this layer | BLOCKED | D-02 remains `PHYSICAL CAPTURE ACCEPTANCE: BLOCKED - NO DEVICE ATTACHED` |
| ESP32 / INMP441 acquisition path | MISSING by design | not part of HomeGuardian's node strategy |

---

## 8b. Dashboard surface (section K)

`/api/integrations` is a new read-only route on the loopback bridge. Every value in it is
DERIVED from real state rather than written down: the Home Assistant block is built from
`HomeAssistantConfig::problems()` (so the reason the integration is off comes from the adapter's
own validation), the device count comes from a real `DeviceAvailabilityTracker` instance, and
the result vocabulary comes from the detection layer's closed label set.

The dashboard renders it as **Sensor & integration status**, with the three provenance labels
stated verbatim - `LIVE SENSOR DATA`, `SYNTHETIC TEST DATA`, `HYPOTHESIS / UNVALIDATED
CORRELATION` - and a per-label legend that maps each result id to whether it is an observation, a
correlation, or a hypothesis. Empty surfaces are shown with their reason ("no acquisition node is
connected", "only synthetic fixtures exercise this layer"), because an unexplained zero reads as
a working sensor reporting calm.

The route also carries the uncertainty model (`belief`, `plausibility` and `ignorance` are
reported separately; conflict above 0.9 suppresses the fusion; the maximum severity this layer
can produce is `warning`), and the payload contains **no** calamity, identity or fall claim at
all, so a client cannot render one as a result. `scripts/verify_integration_dashboard.sh`
enforces that: it asserts the labels and the no-prediction scope are present, that every count is
zero, that each empty surface has a reason, and that a list of forbidden tokens is absent from
the response and no credential leaks with it.

---

## 9. Correction ledger (extended)

The `animal-calamity-precursor-detection` skill already carries seven verified corrections from
its source document. This milestone **adopts that ledger as-is** (nothing here contradicts it)
and adds the corrections found while implementing:

| # | Claim | Status | Engineering consequence |
|---|---|---|---|
| 1 | A skill named `Home Assistant hub` exists in this environment | **WRONG - no such skill, plugin or catalog entry exists** | The Home Assistant work was specified from the installed `homeassistant` *plugin* (v2.0.1) and the brief. Do not cite a "Home Assistant hub skill" in later documents |
| 2 | A phone microphone can serve the rodent-USV (22 kHz) channel | **WRONG - the skill's own hardware floor contradicts it** | 22 kHz USV, gecko call analysis and infrasound storm channels are unreachable with HomeGuardian's hardware. They are recorded as hardware-blocked, not "deferred" |
| 3 | A species label can be attached to bounded motion/audio features | **NOT SUPPORTED** | No species claim is emitted; the only faunal output is the explicitly-hypothetical `possible_animal_activity` |
| 4 | "Sum over supersets" is belief in Dempster-Shafer | **WRONG - that is commonality** (`Bel(A)=sum_{B subset A} m(B)`) | The first implementation computed the wrong quantity and reported belief 1.0 on total ignorance. Caught by a unit test; `plausibility()` was added so the report shows the belief/plausibility interval honestly |
| 5 | An environmental explanation can be detected by requiring the environmental hypothesis to top the DST fusion | **WRONG in practice** | Strong activity evidence dominates the fusion, so the rule could never fire. The precedence is now decided on the environmental reading's own strength *and* the existence of a change to explain; the DST beliefs for both hypotheses are still reported |
| 6 | Copying an observation to build a related one is safe after moving it | **WRONG** | The availability observation was constructed from a moved-from object (empty device id, empty consent context, defaulted provenance). Built explicitly now |
| 7 | The Alexa Proactive Events delivery path is usable for fauna alerts | **UNCHANGED FROM D-01: BLOCKED** | AVS tooling withdrawn, docs 404, Alexa+ partner-gated. The skill's `AMAZON.ThingStatus` correction is adopted and delivery stays deferred |

---

## 10. Verification evidence (executed on this host)

| Check | Command | Result |
|---|---|---|
| New precursor suite | `hg_build/homeguardian_tests "[precursor]"` | **6 cases / 200 assertions, all pass** |
| New sensor suite | `hg_build/homeguardian_tests "[sensors]"` | **7 cases / 487 assertions, all pass** |
| New Home Assistant suite | `hg_build/homeguardian_tests "[ha]"` | **6 cases / 179 assertions, all pass** |
| Pipeline integration | `hg_build/homeguardian_tests "[integration][skills]"` | **7 cases / 65 assertions, all pass** |
| Full host suite | `hg_build/homeguardian_tests` | **417 cases / 4,068 assertions, all pass** (was 387 / 3,053) |
| CTest | `ctest --test-dir hg_build` | 100% (1/1) |
| Fresh out-of-source Release | all three host targets | build 0 errors; suite passes in Release. Sizes: `homeguardian` 3,210,648 B (unchanged), `hg_alexa_bridge` 3,529,496 B (**+187,216 B / +5.6%**, because the status route links `HomeAssistantAdapter` + `SecurityObservation` + `HttpClient` + `NodeIdentity`), `homeguardian_tests` 7,671,664 B |
| ASan + UBSan | `hg_asan/homeguardian_tests` | 417 cases / 4,065 assertions, all pass; **0 sanitizer reports** |
| Section-J gap suite | `hg_build/homeguardian_tests "[gaps]"` | **4 cases / 84 assertions, all pass** |
| Sensor/integration dashboard surface | `bash scripts/verify_integration_dashboard.sh` | **31 API assertions + 5 shell checks, 0 failed** (incl. an adversarial malformed-credential case) |
| Docker D-01 regression | `docker build --target runtime` then `bash scripts/verify_docker_hub.sh` | image rebuilt on the current tree (build rc 0); **26 passed, 0 failed, 0 warned** - health, persisted alerts, container-replacement survival, clean SIGTERM, non-root, caps dropped, no published ports, loopback-only listener, secret mount, WAL header, 3.438 MiB idle / 1 thread / 31.0 MiB compressed |
| Existing behaviour unchanged | `[integration][skills]` allowlist assertions + all pre-existing tags | ten v17/v21 scenarios and H-01s untouched |

Assertion totals move by a few between invocations while the CASE count stays fixed: the
shipped-config test resolves `config/homeguardian.json` from the working directory and falls back to
`HG_SOURCE_DIR`, so a Debug run from the repo root reports 4,068 assertions and a Release run from
the build directory reports 4,065 for the same 417 cases. The case count is the stable figure and is
what this document quotes elsewhere.

Unit-test coverage of the *negative* space is deliberate: refusals, bounds, unauthorized
reports, unmapped entities, hostile payloads, header injection, conflict suppression, ties, and
the "no critical severity" matrix.

---

## 11. What is deferred, and the exact condition to unblock each

| Deferred item | Blocking condition |
|---|---|
| YAMNet / embedding-based acoustic classification | An owner-approved licensing + resource review for ONNX Runtime and AudioSet weights, plus a host with the runtime installed. `VectorDriftDetector` already accepts the embedding vector, so only the extractor is missing |
| Species identification | A defensible, licensed annotated dataset per species. Without it, no claim is made |
| Optical flow / YOLO / MediaPipe pose | OpenCV/MediaPipe present and an approved model-license review; also the project's "no heavyweight ML runtime without review" rule |
| Frigate / NVR | Would add a separate background service and raw-media retention, both excluded by the milestone's resource and privacy constraints |
| MQTT delivery | A deliberate transport decision (the current egress is HTTPS-to-HA only); the skill's QoS/LWT discipline is recorded for when it is taken up |
| Home Assistant WebSocket events | The REST adapter must first be exercised against a real instance; then a framed WebSocket client can replace polling |
| Home Assistant actuation | Separate explicit owner approval (locks, alarms, sirens, cameras) |
| Alexa delivery | Amazon partner access - unchanged external block from D-01/D-06 |
| Phone-side exercise of any of this | A phone attached (D-02 physical acceptance remains BLOCKED) |
