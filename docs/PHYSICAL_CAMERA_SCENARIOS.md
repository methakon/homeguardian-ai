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

## 9. Known limitations

1. **Nominated species:** the layer names no species, at any confidence, and cannot be extended to
   without replacing `ConfirmedSpeciesIdentity` and bringing an approved annotated dataset plus a
   measured false-positive rate.
2. **The pixel proxy is not a motion detector in the human sense.** Light, shadow, curtain, reflection
   and a small animal all produce the same signal. The matrix measures this rather than mitigating it.
3. **Smooth or short events are missed** (mechanism 1 in §5): the detector rewards contrast over time,
   so a clean 2.1× step is unconfirmed while a noisier rise of the same size is confirmed six times.
4. **The episode path's false positives are real**: a lighting transient and a sustained lighting shift
   both alert, and the real ambient run would alert through it. It stays off by default.
5. **The staged phenomena are unmeasured on hardware**: vibration, occlusion, repetitive movement,
   shadow and curtain scenarios are synthetic streams. §10 is the procedure to close that.
6. **One 43-second ambient run is one sample.** It is not a false-positive *rate*; a rate needs
   repeated runs at different times of day, with the illumination system in known states.
7. **No audio work was performed.** The microphone is authorised but was not activated; the audio
   evidence kind is a reserved type with no producer.
8. **No latency budget has been agreed.** 1.1–2.1 s is what the current confirmation policy costs; it
   has not been compared against a requirement.

## 10. Recommendation for future real-camera validation

Run the staged session below when an operator is available (about three minutes), then compare each run
against its synthetic counterpart in §4. Each phase uses the existing operator-gated tool and stores no
frames; the log goes to `docs/evidence/` with `--log`.

```bash
# phase 1 - still reference (60 s, nobody in view)
./build/hg_camera_bridge --capture --confirm --frames 600 --cadence 5 --log docs/evidence/STAGED_01_STILL.log
# phase 2 - the room light switched on and left on during the run (sustained lighting shift)
# phase 3 - a hand waved slowly across the lens for ~1 s (short, brief event)
# phase 4 - sustained movement in view for ~10 s
# phase 5 - the lens covered for ~5 s and uncovered (occlusion + restoration)
# phase 6 - the camera mount tapped repeatedly for ~5 s (vibration)
```

What each phase settles:

* **phase 1** gives the real false-positive floor for this room and illumination.
* **phase 2** is the one that matters most: it tests whether a real lighting change alerts the episode
  path as the synthetic stream predicts, and whether the "one long run, level never returns" signal
  separates it from motion. If it does, the mitigation in §5.1 is implementable from measured data.
* **phase 3** establishes the real minimum reportable event duration (synthetic says ≈6 observations).
* **phase 4** is the true-positive control: it must alert in episode mode.
* **phase 5** verifies that the occlusion restoration does not alert while the blockage itself is
  direction-refused.
* **phase 6** tests whether real mechanical vibration reaches confirmation.

Until those runs exist, the honest status is: **the classic path's behaviour is measured on real data;
the episode path's behaviour is measured on real data for one accidental case and synthetic streams for
everything else, and it should stay disabled.**
