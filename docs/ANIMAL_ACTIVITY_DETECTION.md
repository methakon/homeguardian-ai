# Animal Activity and Environmental Correlation Detection (S-03)

**Source skill:** `animal-calamity-precursor-detection` v1.0.0 (MIT) -
`~/.hermes/skills/multimodal-affective-computing/animal-calamity-precursor-detection/`
**Milestone:** D-02x, branch `feature/skills-detection-integration`
**Scope:** what is measured, what is inferred, and what is explicitly not claimed.

---

## 1. Adopted

| Adopted | Why it is defensible | Implementation |
|---|---|---|
| Bounded audio descriptors (RMS, peak, zero-crossing rate) | Closed-form functions of the input samples; no model, no dataset, reproducible | `src/core/BoundedFeatures.cpp:extract_audio_features` |
| Bounded motion descriptors (changed-pixel fraction, mean absolute delta) between consecutive luma planes | Same: arithmetic on the permitted frame, no inference | `src/core/BoundedFeatures.cpp:extract_motion_features` |
| Activity baseline with a slow EMA and **sustained** confirmation | The skill's rule: "one anomalous frame is a door slamming"; a single spike must not confirm a change | `src/core/ActivityDetectors.cpp:ActivityChangeDetector` |
| **Vector drift before labels** - cosine distance from a baseline centroid, confirmed over consecutive windows | The skill's own build order: this needs no annotated dataset, only a quiet baseline. Works today on feature vectors and later on an approved embedding unchanged | `src/core/ActivityDetectors.cpp:VectorDriftDetector` |
| Dempster-Shafer fusion with conflict mass K and suppression above 0.9 | The skill's rule: "K -> 1 means the sensors contradict. Suppress the alert; do not average." | `src/core/DempsterShafer.cpp:combine` |
| Environmental correlation with the measured lag | The skill's most defensible environmental channel (pressure/humidity/temperature sensitivity) | `src/core/EnvironmentalCorrelation.cpp:CorrelationWindow::correlate` |
| Liveness distinct from silence | The skill's own safety argument: a dead sensor must be distinguishable from a silent one | `src/core/SecurityObservation.cpp:DeviceAvailabilityTracker` |

## 2. Adapted

| Adapted | What changed, and why |
|---|---|
| The DST frame of discernment | The skill proposes `{Routine, SeismicPrecursor, WeatherAnomaly}`. **`SeismicPrecursor` was removed**: the same skill's correction ledger records that ant seismology is debunked and the Haicheng case is overstated, so a life-safety hypothesis has no place in an alerting frame. The frame is now `{routine, activity_change, environmental_anomaly}`, which makes a calamity output unrepresentable rather than merely discouraged (a test asserts the member count) |
| Temporal deviation | Not re-implemented: the existing `AnomalyMonitor` (z-score over a bounded rolling window) is reused, exactly as the milestone requires |
| Activity/audio confirmation | Expressed as ratio-against-baseline with a consecutive-sample requirement rather than the document's ad-hoc thresholds; thresholds are documented as **acceptance targets, not measured performance** |
| Multi-sensor fusion | Folded pairwise with re-normalisation after each fold, and K reported as the **largest** conflict seen (conservative: "how much did any one source disagree with everything else") |
| Missing observations | Added a staleness guard the skill does not describe: no fresh observation within `stale_after` (default 300 s) yields `insufficient_evidence` with "a stale finding is not an update", because silence is not agreement |

## 3. Rejected

| Rejected | Reason |
|---|---|
| Species identification (gecko, rodent, dog, bird, ant, cockroach) | No defensible classifier or annotated dataset is available, and the skill names labelled rodent USVs and gecko calls as the hardest data to obtain. No species claim is emitted anywhere |
| Ant seismology / earthquake prediction | **Debunked by the skill's own ledger**: Landers studies show ants altered behaviour only above M2.0, i.e. they reacted to shaking. Tangshan 1976 had the same folklore and killed 240,000+ with no warning |
| Gecko-call meaning (*tiktiki*), frog/fox vocalisation as weather prophecy | Superstition. A call rate may track ecosystem activity; the folk *meaning* is not a detector |
| p-hole / radon / ionisation precursors | Half-proven at best, needs dedicated instrumentation, and the skill itself puts them in research rather than an alerting path |
| YAMNet log-mel + 1024-d embeddings | Needs ONNX Runtime plus AudioSet weights: a licensing and resource review the project forbids skipping, and no ML runtime is present |
| YOLOv8 individuals, Farneback dense optical flow | Need OpenCV (absent). The swarm-kinematics signal is also unnecessary while no camera is streaming to the hub |
| MQTT / Alexa Proactive Events delivery | Alexa remains externally blocked (AVS withdrawn, docs 404, Alexa+ partner-only). MQTT would add the project's first messaging dependency for no capability the read-only HTTP path lacks |

## 4. Result vocabulary (closed, test-enforced)

`activity_change_detected` · `audio_activity_deviation` · `possible_animal_activity` ·
`environmental_correlation` · `insufficient_evidence`

`insufficient_evidence` also covers contradiction (K above the suppression threshold) and
staleness. `possible_animal_activity` is reachable by exactly one path - sustained motion change
**and** (sustained audio change **or** a flagged audio deviation) **and** a confirmed vector-drift
regime shift **and** at least two confirming sources **and** no environmental reading strong
enough to explain the change - and its own note says "HYPOTHESIS ONLY ... NOT a species
identification", asserted by a test.

**Never produced** (and unrepresentable in the code): `earthquake_imminent`,
`flood_will_occur`, `dog_predicts_disaster`, any storm/tsunami forecast, any species name, any
fall or intruder claim.

## 5. Implementation files

`src/core/BoundedFeatures.{h,cpp}` · `src/core/ActivityDetectors.{h,cpp}` ·
`src/core/DempsterShafer.{h,cpp}` · `src/core/PrecursorDetection.{h,cpp}` ·
`src/core/EnvironmentalCorrelation.{h,cpp}` · plus the reused existing
`src/core/AnomalyMonitor.{h,cpp}` and `src/core/Observation.{h,cpp}`.

## 6. Tests

| Suite | Command | Result |
|---|---|---|
| Features, activity, vector drift, DST, suite, environmental | `homeguardian_tests "[precursor]"` | 6 cases / 200 assertions |
| Section-J gaps: missing observation, bounds | `homeguardian_tests "[gaps]"` | 4 cases / 84 assertions (shared with the other skills) |
| Pipeline integration and severity invariants | `homeguardian_tests "[integration][skills]"` | 7 cases / 65 assertions |

Negative-space coverage is deliberate: oversize and non-finite input refused, warm-up never
votes, a single spike never confirms, a wrong-length vector is refused without resetting,
contradictory sources suppress, ties are reported as undecided, a stale finding degrades to
insufficient evidence, and the environment-precedence path is proven reachable.

## 7. Measured behaviour (real runs, not predicted)

- A sustained 4x activity rise confirms after 3 consecutive samples; a single spike leaves
  `direction=rising, confirmed=false`.
- Two agreeing activity sources combine to belief ≈ 0.98 on `activity_change` with K = 0.
- Motion + audio + a confirmed vector shift reach `possible_animal_activity`; adding an
  environmental deviation of 0.9 changes the label to `environmental_correlation`.
- A 1.0 cosine distance (orthogonal vector) sustained for 3 windows confirms a regime shift; a
  single orthogonal window stays `drifting`.
- Stale input (600 s with `stale_after = 60 s`) yields `insufficient_evidence`, not a repeated
  finding.

## 8. Missing hardware and remaining validation

| Channel | Blocked by |
|---|---|
| Species identification from sound | No annotated dataset; no model runtime |
| Rodent ultrasonic vocalisation (22 kHz) | Phone microphones roll off far below the band. The skill's own hardware floor requires an INMP441-class I2S microphone - hardware this project does not have |
| Infrasound storm detection (0.05-10 Hz) | Requires a dedicated infrasound sensor |
| Swarm kinematics (ant columns) | Requires a fixed camera streaming to the hub (D-03/D-05) and OpenCV |
| Any live-media acceptance | `D-02 PHYSICAL CAPTURE ACCEPTANCE: BLOCKED - NO DEVICE ATTACHED`; every result above comes from deterministic fixtures, labelled synthetic |

**Remaining validation requirement:** the only honest path to stronger animal-activity claims is
(a) a device streaming media to the hub, and (b) an approved, licensed annotated dataset or an
approved embedding model, followed by a measured false-positive rate on real household data. Until
then the layer reports measurements and one clearly-labelled hypothesis, and nothing else.
