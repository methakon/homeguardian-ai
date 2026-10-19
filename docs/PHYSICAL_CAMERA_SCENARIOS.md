# Physical camera scenarios, false-positive evaluation and the animal-activity abstraction

**Branch:** `experiment/hg-v380-09-secure-camera-integration` — isolated, nothing merged into PR #20.
**Authorization:** the project owner authorised physical camera and microphone testing in the
designated non-private area (no private conversations or unrelated people; no published identifiable
media; no public streams; nothing through the Oracle VPS). Every capture below ran with the operator
gate (`--confirm`) and stored no frames.
**No threshold was changed anywhere in this work.**

Two halves: what a REAL camera produced (§1–§3) and what SYNTHETIC streams of the requested phenomena
produce (§4–§7). They are labelled apart throughout, and a replay of the real run is used to compare
them on the same pipeline (§3.3).

---

## 1. Hardware and authorized test conditions

| | |
|---|---|
| Device | `/dev/video0`, USB UVC (`uvcvideo`, V4L2), YUYV 640×480, stride 1280 |
| Module reported by the kernel | `card=W100: W100`, bus `usb-0000:00:14.0-9` |
| Location | the owner's designated **non-private** testing area |
| Consent | `--confirm` operator flag on every run; the tool cannot capture without it |
| Storage | none — each frame is reduced to its luma plane in place and discarded; no file, socket or stream |
| Microphone | **not activated** in this work; the audio channel exists only as a reserved evidence kind (§8) |
| V380 cameras | untouched; still control-only with media BLOCKED |

## 2. Capture methodology

One physical run this session, chosen because it needs nobody to stage anything:

```bash
./build/hg_camera_bridge --capture --confirm --frames 1200 --cadence 5 \
    --log docs/evidence/PHYSICAL_CAMERA_AMBIENT_RUN.log
```

* **Ambient run:** 600 frames (the tool's own cap) reduced to **120 observations**, cadence 5 frames
  per observation, **42.47 s**, **0 driver-dropped frames**, **0 stalls**. The room was nominally
  empty of deliberate activity.
* Reference runs already in the repository: the consented 600-frame motion capture
  (`PHYSICAL_CAMERA_CAPTURE_LOG.md`) and the 90-frame still-scene run
  (`PHYSICAL_CAMERA_STILL_SCENE.log`).

Staged conditions (lighting change, occlusion, vibration, sustained and brief movement, repetitive
motion) require a person in the room; the procedure is written out in §9 so it can be run in about
three minutes when the operator is available. **Nothing in §4–§7 is a substitute for that.**

## 3. What the REAL camera produced

### 3.1 The ambient run

```
frames captured      : 600 of 600 requested      driver-dropped: 0
observations built   : 120 (cadence 5)           elapsed: 42474 ms
peak motion / ratio  : 0.0745 / 2.42
peak confidence      : 0.710
windows by kind      : sustained_increase=3 sustained_decrease=0 quiet=112
MOTION-VERDICT       : a sustained increase WAS observed
```

The three confirmed increases, as printed:

```
[195] motion=0.0431 ratio=2.42  sustained_increase
[200] motion=0.0151 ratio=2.42  sustained_increase
[205] motion=0.0184 ratio=1.80  sustained_increase
```

**What this does not tell us, and the honest reading.** Something changed in the field of view for
about 1.5 seconds, 14 seconds into the run, at twice the ambient level and about 7× the still-scene
ceiling (0.0103). The tool has no model of *what* — a person crossing, an object moving, a reflection,
or the illumination changing all produce this. **Either the camera correctly caught unplanned movement,
or it caught a lighting/auto-exposure change: the pixel proxy cannot distinguish them, and that is the
finding.** It is not evidence of an animal, a person or an event.

### 3.2 The same run through the existing decision paths

The tool evaluates the real capture's own observations with the shipped rule and engine:

```
== RULE EVALUATION over this capture's own observations ==
  observations in the window : 120
  fusion verdict             : not_detected (confidence 0.015, 115 contributing)
  detection-fusion rule      : no alert
```

So on real hardware the **classic path refuses**: three evidence votes against 112 baseline votes is a
dilution of the same shape the synthesis predicted.

### 3.3 The same run replayed into the opt-in path (Pipeline cross-check)

`tests/core/test_camera_scenarios.cpp` `[recorded]` replays those recorded scalars (3 confirmed
increases at 0.710/0.710/0.400 among 112 quiet and 5 unusable) through the real engine and rule,
labelled `synthetic` because a replay is not a measurement:

```
RECORDED AMBIENT classic: verdict=not_detected conf=0.0148 contributing=115 alert=no
RECORDED AMBIENT episode: ALERT
```

Two independent computations agree: the C++ replay gives **0.0148** and the capture tool's own run
printed **0.015** for the same window. That agreement is the cross-check the owner asked for between
physical observation and synthetic replay.

And the opt-in episode path **does** alert on it (mean confidence **0.607** over the three confirmed
increases). That is the episode path's false-positive risk measured on real hardware data, from a room
nobody deliberately entered — which is precisely why it stays off by default (§10).

## 4. The synthetic scenario matrix (16 streams)

Each stream is a motion-level sequence fed through the **real** `ActivityChangeDetector` at the
capture's cadence (one observation per 5 frames ≈ 2.81 obs/s), then through the **real**
`ActivityEpisodeSelector` and `DetectionFusionRule`. Provenance is `synthetic` throughout. Verdicts are
the human reading of each story: **TP** = genuine motion in view, **TN** = no genuine motion, **FP** =
alerted without genuine motion, **FN** = genuine motion not reported.

| # | scenario | verdict | detector confirms | classic alert | episode alert | story |
|---|---|---|---|---|---|---|
| 1 | still_scene | TN | no | no | no | nothing moves |
| 2 | sudden_lighting_transient | **FP** | yes (run 3) | no | **YES 0.814** | light switched on for two observations |
| 3 | sustained_lighting_shift | **FP** | yes (run 15) | no | **YES 0.641** | illumination changes and stays changed |
| 4 | gradual_day_night_30 | TN | no | no | no | slow drift, baseline tracks |
| 5 | gradual_day_night_60 | TN | no | no | no | as above, slower |
| 6 | gradual_day_night_120 | TN | no | no | no | as above, slowest |
| 7 | camera_vibration | TN | no | no | no | mount shakes every 2 observations |
| 8 | reflection_flicker | TN | yes (weak) | no | no | flicker every observation |
| 9 | shadow_sweep | TN | yes (2 runs) | no | no | shadow swells to 3.5× and recedes |
| 10 | curtain_movement | **FN** | yes (2 runs, max 3) | no | no | irregular real motion |
| 11 | temporary_occlusion | TN | yes (falling) | no | no | view blocked, then restored |
| 12 | repetitive_movement | **FN** | yes (run 8) | no | no | three moving observations in six |
| 13 | short_animal_sized_motion (1.9×, 4 obs) | **FN** | no | no | no | hand-sized body crosses briefly |
| 14 | sustained_animal_sized_motion (2.1×, 20 obs) | **FN** | once | no | no | hand-sized body moves, smoothly |
| 15 | short_high_contrast_motion (3.5×, 4 obs) | **FN** | yes (max 2) | no | no | four observations at 3.5× |
| 16 | longer_high_contrast_motion (3.5×, 8 obs) | **TP** | yes (run 5) | no | **YES 0.563** | eight observations at 3.5× |

```
MATRIX TOTALS tp=1 tn=8 fp=2 fn=5 (of 16 scenarios)
```

**The matrix is the finding, not a scoreboard.** Against this pipeline's closed vocabulary ("a
sustained increase in motion in view"), a lighting change and a curtain are *indistinguishable* — the
proxy measures changed pixels, and both change pixels. The suite therefore records the two lighting
false positives and the five false negatives **as measured behaviour**, so a future mitigation is a
visible change to a test rather than a discovery in the field.

## 5. False-positive and false-negative mechanisms (measured)

**False positives — the pixel proxy cannot separate light from motion.** Both lighting forms alert, and
only through the opt-in episode path (the classic path refuses both):

| form | mechanism |
|---|---|
| transient (2 observations at 10×) | the 4-sample recent mean stays high for ~3 observations, which is exactly the confirmation count — so a *two-observation* jump confirms |
| sustained shift (30 observations at 10×) | the changed-pixel fraction rises and stays raised; the baseline adapts, but not before the run confirms 15 consecutive observations |
| distinguishing signal for a future fix | a lighting step is **one** long contiguous run (≥10) whose level never returns; motion oscillates and produces repeated runs (the recorded capture: 6 rises then 41 settling falls) |

**False negatives — two separate mechanisms.** These matter more, because a home-safety sensor that
misses is worse than one that over-reports:

1. **The confirmation window cannot see short or smooth events.** A four-observation event at 3.5× the
   baseline reaches only **2** consecutive confirmed samples: the 4-sample recent window dilutes the
   first two (ratio 1.28, then 1.55) while the 0.05-alpha baseline climbs to meet the event, so the
   ratio clears 1.6 for too few consecutive samples. With `confirm_samples = 3` and the episode
   selector's `min_span = 3`, **any event shorter than about six observations is unreportable**, at any
   amplitude. The same mechanism explains row 14: a *clean* step to 2.1× is confirmed **once**, while
   the recorded physical rise — noisier, ratios 1.85–2.70 — was confirmed six times. **Contrast over
   time, not amplitude, is what the detector rewards.**
2. **The classic fusion needs ≈85 % of its window to be evidence.** With this run's confidence level
   (mean 0.697) an alert needs `0.697 · f² ≥ 0.5`, i.e. `f ≥ 0.85`. Measured exactly on the recorded
   rise's own confidences:

```
WINDOW evidence=6 quiet=0 total=6 agreement=1.0000 conf=0.6965 alert=YES
WINDOW evidence=6 quiet=1 total=7 agreement=0.8571 conf=0.5117 alert=YES
WINDOW evidence=6 quiet=2 total=8 agreement=0.7500 conf=0.3918 alert=no
WINDOW evidence=6 quiet=3 total=9 agreement=0.6667 conf=0.3096 alert=no
```

So one quiet observation beside the evidence is survivable and two are not. A 60-second window at
2.81 observations/second (~170 observations) can never report a two-second event, however clearly the
detector saw it.

## 6. Detection latency, confidence stability and episode boundaries (measured)

| scenario | event onset | first confirmed | episode | latency (obs / seconds) | episode mean / weakest |
|---|---|---|---|---|---|
| ambient run (real) | obs 39 (frame 195) | 39 | 3 consecutive | 0 / 0 s at onset | 0.607 / 0.400 |
| longer_high_contrast | obs 25 | 29 (first_detected=29) | start 28, length 5 | 4 obs ≈ 1.4 s | 0.563 / 0.450 |
| short_high_contrast | obs 25 | 29 | none | n/a (never confirms 3) | n/a |
| sustained_lighting_shift | obs 30 | 33 | start 32, length 15 | 3 obs ≈ 1.1 s | 0.641 / 0.302 |
| curtain_movement | obs 20 | 24 | none | n/a | n/a |
| repetitive_movement | obs 20 | 24 | none | n/a | n/a |

Two properties worth carrying forward:

* **Latency is dominated by confirmation, not by the algorithm's arithmetic**: the first confirmed
  observation arrives about **3–4 observations (1.1–1.4 s)** after onset, and an episode needs 3
  consecutive confirmed changes on top of that, so a reportable episode costs roughly **6 observations
  (≈2.1 s)** of event duration. That is the price of "one anomalous frame is a door slamming".
* **Confidence stability**: within an episode the weakest member is materially below the mean (0.450 vs
  0.563; 0.302 vs 0.641), so the conservative bound is the *weakest* confidence, which the episode
  records and the alert details carry. Fragmentation is visible as the run count: the sustained
  lighting shift produces one long run, the shadow sweep and the curtain produce two short ones, and
  the resting scene produces none.

## 7. The 60-second window, measured

The window is what decides the classic verdict, and the arithmetic is exact (§5.2). Shortening the
window does **not** rescue a short event by itself: a five-second window at the capture cadence holds
14 observations of which only ~6 can be evidence, and the same `f ≥ 0.85` rule applies inside it. What
changes the outcome is the **cadence** — emitting one observation per second instead of per five
frames puts ~5 observations in a five-second window, of which 4 are evidence (`f = 0.8`) and the metric
becomes reachable. That is a configuration decision to be made on measured false-positive cost, not a
threshold to nudge.

## 8. The animal-activity abstraction (Tasks C and D)

`src/core/AnimalActivity.{h,cpp}` — **opt-in, `enabled = false` by default**, referenced by nothing in
the release's decision path. Four layers, separated by the type system rather than by documentation:

| layer | type | what it can say |
|---|---|---|
| 1. measurement | `ActivityEvidence` (`motion`, `audio`, `environmental`, `schedule`) | one bounded number with its own detector, model and provenance |
| 2. motion only | `AnimalActivityClass::motion_only` | "a sustained change in motion in view" — no animal claim of any kind |
| 3. hypothesis | `AnimalActivityClass::candidate_animal_activity` | "a multi-source sustained activity pattern consistent with a small body moving in view", prefixed `HYPOTHESIS ONLY`, with the confounders named (a curtain, a shadow or a person produces the same evidence) |
| 4. confirmed identity | `ConfirmedSpeciesIdentity` | **cannot exist in this build**: every constructor is deleted and `feature_available()` returns `false` |

Refusals that are asserted rather than promised: a single sustained source is never a candidate; a
spike is never evidence at all, however strong; audio-only evidence is not an animal claim; a policy
that requires audio corroboration refuses to promote when no audio channel exists; the classification
vocabulary is closed and an unknown enum value degrades to `insufficient_evidence`; provenance is
derived from the contributing evidence (so a replay stays `synthetic` and mixed stays `mixed`); and an
emitted-text scanner fails the build if any sentence contains a species name, an identification verb,
or an unnegated "species"/"animal" claim — which is why the honest denials say "not a species
identification" and still pass.

Tests: `tests/core/test_animal_activity.cpp`, **10 cases** (17 when run with the pre-existing `[animal]`
tagged precursor tests), all passing.

## 9. Three real ambient runs (added this stage)

Two further captures were taken under the same authorization (600 frames, 120 observations, cadence 5,
0 driver drops, nothing stored): `PHYSICAL_CAMERA_AMBIENT_RUN_2.log` and `_3.log`. Together with run 1
they are the real evidence, and they are less flattering than one sample suggested.

| run | elapsed | peak motion | confirmed increases | confirmed decreases | classic verdict (tool's own) | episode path (replay) |
|---|---|---|---|---|---|---|
| 1 | 42.5 s | 0.0745 | 3 (one contiguous run) | 0 | not_detected 0.015 -> no alert | **ALERT** 0.607 |
| 2 | 24.6 s | **0.4227** | 8 (runs of 2 and 3) | 41 | inconclusive 0.080 -> no alert | **ALERT** 0.593 |
| 3 | 24.6 s | 0.1163 | 3 (contiguous) | 8 | not_detected 0.025 -> no alert | no alert (mean 0.478) |

**All three runs produced confirmed sustained increases in a room nobody was deliberately using.** Run
2 peaked at motion 0.4227 — 41× the still-scene ceiling — with 41 settling falls afterwards, which is
the shape of something crossing the field. The classic path refused all three, and the opt-in episode
path would have alerted on two of three.

That inverts the framing of §5. The episode path's exposure is not mainly "a lighting change looks like
motion"; it is that **a home's ordinary ambience contains sustained increases**, because people move
through their own rooms. An alert source keyed on "a sustained increase in motion in view" would fire
during normal life, which is the strongest argument available for leaving it disabled and treating the
signal as *reporting* rather than as an alarm.

Run 3 is the useful negative and shows the machinery working: three contiguous confirmed rises whose
mean confidence (0.478) sits below the unchanged 0.5 gate, so the episode path refuses them too.

**A configuration finding from comparing the runs.** Runs 2 and 3 captured the same 600 frames in
24.6 s where run 1 took 42.5 s — the sensor's frame rate is not fixed, so "cadence 5" means 0.205 s per
observation in one run and 0.356 s in another. **The observation rate depends on the camera's
throughput, not on a wall-clock cadence**, which means the evidence count inside a five- or sixty-second
fusion window varies run to run. A wall-clock observation cadence would make the window's behaviour
predictable; that is a design change to propose, not a threshold to tune.

## 10. Task 1 — the const-correctness question, answered

The report was that `test_camera_scenarios.cpp` calls `DetectionFusionRule::evaluate()` on a const
object. That was true of an intermediate revision of this stage's own work and was fixed before the
commit; the committed file constructs the rule non-const (`DetectionFusionRule classic("detection-fusion", cfg);`),
which is why the suite compiles. The analysis behind the choice:

* `ICorrelationRule::evaluate` is declared **non-const** (`virtual std::optional<Alert> evaluate(const std::vector<Event>&) = 0;`),
  so every rule in the project must have a non-const `evaluate`.
* `DetectionFusionRule` holds `name_`, `cfg_` and `fusion_`; nothing is `mutable`, `evaluate` calls the
  const `FusionEngine::fuse`, and `build_alert` is declared const. **The method does not mutate state**:
  its constness is inherited from the interface, not earned by the body.
* Therefore the correct fix is the test's object, and the production signature was left alone — which is
  what the instruction required. Making `evaluate` const would have changed the interface for every
  rule in order to satisfy one test.

## 11. Task 3 — candidate comparison (what each knob buys, and what it costs)

Ten candidates measured on the same sixteen scenarios, with the human verdicts as ground truth (6
positive-class, 10 negative-class). **Every row keeps the rule's 0.5 gate and the engine's 0.6
agreement threshold fixed** — nothing here relaxes a gate, so a better row is better use of evidence.

| candidate | detector confirmed (of 16) | classic tp/tn/fp/fn | episode tp/tn/fp/fn |
|---|---|---|---|
| shipped (recent=4, alpha=0.05, confirm=3, span=3, rising required) | 10 | 0/10/0/6 | 1/8/2/5 |
| recent_window=2 | 11 | 0/10/0/6 | 1/9/**1**/5 |
| recent_window=6 | 9 | 0/10/0/6 | 0/8/2/6 |
| slow_alpha=0.02 (baseline follows more slowly) | 12 | 0/10/0/6 | **4**/6/**4**/2 |
| slow_alpha=0.10 (baseline absorbs faster) | 6 | 0/10/0/6 | 0/9/1/6 |
| confirm_samples=2 | 10 | 0/10/0/6 | 3/8/2/3 |
| recent_window=2 + slow_alpha=0.02 | 13 | 0/10/0/6 | 3/7/3/3 |
| episode min_span=2 | 10 | — | 3/8/2/3 |
| episode **min_span=4** | 10 | — | 1/9/**1**/5 |
| direction rule off | 10 | — | 1/8/2/5 |

What the table says:

1. **The classic path alerts on nothing in this matrix** (0 tp / 6 fn, no false positives). At this
   cadence it is a floor, not a competitor: every positive scenario is diluted below its gate.
2. **Two candidates halve the observed false positives at no measured true-positive cost**, and both do
   it by *tightening*, not loosening: `recent_window=2` (a faster recent mean stops a lighting step
   from staying above the confirm ratio) and `episode min_span=4` (a three-observation lighting
   transient is not a four-observation episode). Neither is a threshold relaxation.
3. **Every candidate that recovers more true positives pays in false positives**: `slow_alpha=0.02`
   buys 3 more detections and takes 2 more false alerts; `confirm_samples=2` and `min_span=2` buy 2
   more each at the same false-positive count. The trade is real and monotone in this matrix.
4. **The direction guard is cheap insurance, not the active mechanism**: turning it off changes no row
   here, because the only falling run in the matrix (the occlusion) is too weak to qualify anyway. It
   stays because its cost is zero and it prevents "activity stopped" from ever being reported as
   "activity started".
5. **No knob fixes the real problem.** The false positives come from a feature that cannot separate
   light from motion; the false negatives come from a confirmation rule that rewards contrast over
   time rather than amplitude. Both are feature/rule design questions, not threshold questions.

## 12. Known limitations

1. **Nominated species:** the layer names no species at any confidence, and cannot be extended without
   replacing `ConfirmedSpeciesIdentity` and bringing an approved annotated dataset plus a measured
   false-positive rate.
2. **The pixel proxy is not a motion detector in the human sense.** Light, shadow, curtain, reflection
   and a small animal produce the same signal; §4 measures this rather than mitigating it.
3. **Smooth or short events are missed:** a clean 2.1× step is confirmed once where a noisier rise of
   the same size is confirmed six times, and any event shorter than ~6 observations is unreportable.
4. **The episode path's false positives are real and were observed on hardware**, not only in synthesis:
   two of three ambient runs would have alerted through it.
5. **Ambient runs are samples, not rates.** Three runs, at two times of day, with the illumination
   system in unknown states, and with no record of whether anyone walked past the lens.
6. **The staged phenomena remain synthetic**: vibration, occlusion, repetitive movement, shadows and
   curtains have not been induced physically.
7. **The observation rate is not wall-clock controlled** (§9): the same cadence setting yields 0.205 s
   or 0.356 s per observation depending on the sensor's throughput, so window contents vary run to run.
8. **No microphone work was performed.** The microphone is authorized but was not activated; the audio
   evidence kind has no producer.
9. **No latency budget has been agreed**, and no candidate in §11 was evaluated for its effect on
   latency beyond the shipped configuration (first confirmation 1.1-1.4 s after onset).

## 13. Recommendation

**Keep the episode path disabled by default, and treat this stage's two tightening candidates as the
only change worth carrying forward — after real validation, not before.**

* The provenance fix is unaffected and should travel with the camera path: correctness, no behaviour
  change.
* **Do not enable `use_episode_evidence`.** Two of three real ambient runs would have alerted through
  it, in a room nobody was deliberately using. Ordinary domestic ambience is a sustained-increase
  generator, so this signal belongs in reporting/analytics, not in an alarm path.
* If it is ever enabled, the measured starting point is `min_span = 4` and/or `recent_window = 2`: both
  halve the observed false-positive count in this matrix with no measured true-positive cost, and both
  tighten rather than relax. That claim rests on 16 synthetic scenarios and three real runs, which is a
  hypothesis to validate, not a result.
* The productive next step is not a knob: separate **illumination change from motion** (the "one long
  run whose level never returns" signature measured in §5.1, or an illumination-normalised feature),
  and give the observation stream a **wall-clock cadence** so a fusion window contains a predictable
  amount of evidence.
* Validation to run first, in this order: the six-phase staged procedure below (§14), then a day of
  timed ambient runs to turn a sample into a rate, then the same matrix with real observations in place
  of the synthetic streams.

## 14. Staged validation procedure (unchanged, still pending an operator)

```bash
./build/hg_camera_bridge --capture --confirm --frames 600 --cadence 5 --log docs/evidence/STAGED_01_STILL.log
# phase 2 - the room light switched on and left on during the run (sustained lighting shift)
# phase 3 - a hand waved slowly across the lens for ~1 s (short, brief event)
# phase 4 - sustained movement in view for ~10 s
# phase 5 - the lens covered for ~5 s and uncovered (occlusion + restoration)
# phase 6 - the camera mount tapped repeatedly for ~5 s (vibration)
```

Phase 2 remains the one that matters most, and §9 raises its stakes: if a real lighting change produces
the same evidence as a person crossing the room, then no alert source built on this feature can be
trusted in a home with windows.
