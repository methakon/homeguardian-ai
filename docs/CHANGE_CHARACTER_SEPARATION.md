# Separating illumination change from localized motion

**Branch:** `experiment/hg-v380-09-secure-camera-integration` — isolated, nothing merged into PR #20.
**Authorization:** physical camera testing in the owner's designated non-private area, operator-gated
(`--confirm`), no frame stored, transmitted or streamed, microphone not activated.
**Thresholds:** no existing threshold was changed. The detector's confirm ratio, the rule's 0.5 gate and
the engine's 0.6 agreement threshold are all untouched and are not read by the new component. The
numbers introduced here belong to a **new, experimental descriptor** and are marked as such.

---

## 1. The problem, restated from evidence

The camera path reduces a frame pair to one scalar — the fraction of pixels that changed by at least a
fixed per-pixel threshold. Two physically different things produce that scalar:

* a room light switching on changes most of the plane at once, one direction (brighter);
* a body crossing the field changes a compact region, two directions (it covers here, reveals there).

Measured consequences of not distinguishing them:

| evidence | result |
|---|---|
| three real ambient runs (§4 of `PHYSICAL_CAMERA_SCENARIOS.md`) | confirmed sustained increases in 3 of 3, in a room nobody was deliberately using |
| synthetic lighting transient (2 observations at 10×) | alerted through the opt-in episode path |
| synthetic sustained lighting shift | alerted through the opt-in episode path |

So the missing distinction was never a threshold. It is a second dimension of evidence.

## 2. What was built

**`src/core/BoundedFeatures.{h,cpp}` — region descriptors** (closed-form arithmetic over the permitted
planes, allocation-free, refused with a reason outside its bounds):

| descriptor | what it is | why |
|---|---|---|
| `peak_cell_share`, `occupied_cell_fraction` | changed-mass distribution over a bounded grid | descriptive; the coarse "how spread" view |
| `brighter_share`, `darker_share`, `bipolar_balance` | how much of the change went each way; `2·min/max`, 0 = unipolar, 1 = balanced | a light moves every pixel the same way; a body produces both signs |
| `centroid_x/y` | where the changed mass is | travel between frames = direction consistency |
| **`spread_radius`** | RMS radius of the changed mass from its own centre, ÷ frame diagonal | the scale-free form of "compact or all over the frame" — the measure the classifier keys on |

**`src/core/ChangeCharacter.{h,cpp}` — the classifier**, with a closed vocabulary of five characters
plus `unknown`:

| character | meaning |
|---|---|
| `quiet` | small **and** spread: sensor noise, a slow illumination flutter |
| `global_illumination` | spread **and** unipolar: a light, an exposure shift |
| `localized_unipolar` | compact **and** unipolar: something appeared, or a source switched on in view — real, and **not** motion |
| `localized_motion` | compact **and** bipartite: something moved through the field |
| `diffuse_motion` | bipartite but spread: large or close movement, foliage, rain, **or a moving shadow** |

**Wiring, all additive and all opt-in:**

| where | change | default |
|---|---|---|
| `CameraActivitySession::Config` | `width`, `height` — without geometry the character stays `unknown` rather than guessing | `0` (unknown) |
| `CameraActivitySession` | computes the region descriptors in the same frame window as the motion descriptor; `CameraFinding` carries `change_character`, `spread_radius`, `bipolar_balance` | computed when geometry is supplied |
| the observation's features | `change_character`, `spread_radius`, `bipolar_balance` added after the detector builds the observation | additive; nothing that reads the feature set breaks |
| `ActivityEpisodeSelector::Config` | `require_motion_character` — an episode whose members are at least half `global_illumination` is refused; members with **no** character feature are not counted against a run, so the gate cannot silently disable the path for evidence it cannot see | **`false`** |

## 3. Frame-level validation (exact ground truth)

A scalar replay cannot validate this, because the whole problem is that one scalar cannot tell the cases
apart. So the fixtures are **generated luma planes**: we know which pixels changed, by how much, in which
direction, and where. `tests/core/test_change_character.cpp`, **15 cases / 59 assertions**, all passing.

Two frame pairs with the *same* kind of scalar and different pixels:

```
global step  (100 -> 120 everywhere)   changed_fraction 1.000   spread_radius 0.2887   balance 0.00
block moved  (30x40 block, one step)   changed_fraction 0.125   spread_radius 0.1041   balance 0.98
```

Measured properties the tests pin:

* **The step and the moving block separate by 2.8× in spread radius** (0.2887 vs 0.1041) with the
  threshold at 0.18 leaving room on both sides.
* A step, a step the other way, and a saturating step are all `global_illumination`.
* A bright body, a dark body, and a small (8×6) body moving are all `localized_motion` — including the
  animal-sized case.
* Something appearing in place, and its removal, are `localized_unipolar`: real, and not called motion.
* A change below the per-pixel delta threshold, and sub-threshold noise, are `quiet` — which is how the
  baseline tracks a gradual day/night ramp.
* The centroid travels with the change (direction consistency in its smallest form).
* Descriptors refuse what they cannot describe: geometry that does not multiply to the pixel count, a
  null plane, too few pixels, a grid beyond the bound; an invalid descriptor classifies as `unknown`.
* The explanation text never contains `person`, `animal`, `species`, `face`, `intruder` or `fall`.

## 4. Chain validation — the same two streams through the real pipeline

`tests/core/test_change_character.cpp` feeds two synthetic frame streams through the **real**
`CameraActivitySession` (geometry supplied), then the **real** rule and episode selector:

| stream | detector sees | character of the confirmed frames | episode WITHOUT the gate | episode WITH the gate |
|---|---|---|---|---|
| illumination (a light switching on and off) | sustained increase | `global_illumination` | **alerts** (the false positive) | **refused** |
| motion (a small body crossing for twelve frames) | sustained increase | `localized_motion` | alerts (true positive) | **alerts** |

Both streams are identical to the old scalar's view; their characters differ from the pixels. That is
the before/after this stage exists to produce, and it is measured, not asserted:

```
WITHOUT gate:  lighting -> alert        motion -> alert
WITH gate:     lighting -> refused      motion -> alert (evidence_mode=episode)
```

## 5. Calibration, and a mistake the real data caught

The first version of the classifier used `quiet_fraction = 0.002` (0.2% of pixels). A real capture
(`docs/evidence/PHYSICAL_CAMERA_CHARACTER_RUN.log`) showed what that does:

```
motion distribution over 120 observations: min 0.0000  p10 0.0094  median 0.0098  p90 0.0106  max 0.3522
102 of 120 observations below the still-scene ceiling of 0.0103
character mix: 117 of 120 classified diffuse_motion
```

The classifier was reading the sensor, not the scene: the camera's **idle band is ≈1% of pixels**
crossing the delta threshold, so a 0.2% floor made every noisy frame "motion".

Two corrections, both from that measurement:

1. **`quiet_fraction` = 0.02**, which sits clear above the measured idle band (p90 = 0.0106) and far
   below real movement in the same run (0.3522).
2. **Quiet means small AND spread.** A floor alone would have thrown away the case this project cares
   about: a small body changes ~0.7% of pixels in a 160×120 frame (and ~0.04% in a 640×480 frame), which
   is *below* the sensor's idle band, but it is **compact** while the idle band is scattered. So the
   rule is joint, and both sides of it are pinned by tests — a moving 8×6 body classifies as motion
   while a 1% scattered idle band stays quiet.

Both are descriptor parameters of a new component. Neither touches the detector or the rule.

### 5.1 The correction, measured on hardware

A second capture with the corrected classifier
(`docs/evidence/PHYSICAL_CAMERA_CHARACTER_RUN2.log`, 600 frames, 120 observations, 72.7 s, 0 driver
drops) reads:

| run | character mix over 120 observations | idle band | the one large change |
|---|---|---|---|
| before calibration (`..._RUN.log`) | **116 `diffuse_motion`**, 1 `unknown`, 1 `global_illumination` | labelled as motion | 0.3522 -> `global_illumination` |
| after calibration (`..._RUN2.log`) | **116 `quiet`**, 1 `unknown`, 1 `global_illumination` | correctly quiet | 0.2965 -> `global_illumination` |

### 5.2 What that reinterprets about the earlier ambient evidence

Both captures are `sustained_increase=0` - they are the settling case, not the moving one - and in both,
the single large change is a **spread, unipolar** change on the **warm-up** frame pair: the camera
settling its exposure between the first two frames. `PHYSICAL_CAMERA_CHARACTER_RUN.log` reached 0.3522
of pixels and `RUN2.log` 0.2965, an order of magnitude above the idle band; both carried the
`global_illumination` character, and both belong to a `warming_up` finding.

That changes the reading of the three ambient runs reported earlier. Their large early excursions
(0.4227, 0.1163, 0.0745) sat at the START of each capture with a long settle afterwards, which is the
signature of this warm-up/exposure change rather than of someone crossing the room. The earlier report
said the cause was unknown to the tool and named movement and illumination as equally likely; the
character evidence now narrows it for the two runs that carry it, and the correct conclusion is that
**the earlier ambient false-positive evidence may have been a session-start illumination artifact, not
domestic activity.** That weakens the "ordinary ambience is a sustained-increase generator" claim in
`PHYSICAL_CAMERA_SCENARIOS.md` §9 and replaces it with a narrower, better-supported one: *the start of a
capture produces a large illumination-classified change, and the detector's own warm-up handling keeps
it out of the episode path* (a warm-up observation is `inconclusive`, so it is not a vote - asserted by
`[character][session][real]`).

The staged procedure in §9 is what settles it: three ambient captures cannot distinguish "someone
walked past" from "the exposure settled", and a capture with an operator present can.

## 6. Detection latency, persistence and direction consistency

* **Latency:** confirmation still costs what it did — the first confirmed observation arrives ~3-4
  observations (1.1-1.4 s at the capture cadence) after onset, and a reportable episode needs ~6
  observations (≈2.1 s). The character is computed per frame pair and adds no latency beyond one pass
  over the plane.
* **Persistence and direction consistency** are present only in their smallest form: `centroid_x/y`
  gives the direction of travel between consecutive pairs (tested), and the episode selector's
  run-continuity already provides frame-to-frame persistence. A shadow that tracks its illumination
  source, or a body that reverses, is **not** yet distinguishable — see §8.
* **Baseline drift** is handled where it always was: a slow ramp stays under the per-pixel delta
  threshold and the EMA baseline tracks it, so it never becomes evidence (tested at 30/60/120-frame
  ramps in the scenario matrix).

## 7. False positives and negatives after the change

| case | before | after (gate on) |
|---|---|---|
| synthetic lighting transient | alert (FP) | **refused** |
| synthetic sustained lighting shift | alert (FP) | **refused** |
| synthetic brief/spread motion that the detector confirms | alert (TP) | alert (TP) |
| real ambient runs (3) | episode path would alert on 2 | lighting-classified frames refused; motion-classified frames still eligible |
| still scene, gradual drift, vibration, reflection flicker | no alert | no alert |
| events the detector never confirms (short/smooth) | no alert | no alert (unchanged: the character cannot help where there is no evidence) |

The gate is a **refusal**, never a promotion: it can only remove alerts whose evidence is wholly
illumination-like, so it cannot manufacture a detection.

## 8. Known limitations (the honest list)

1. **A moving shadow is indistinguishable from a moving object.** Both are a moving luminance boundary:
   compact, bipartite, with a travelling centroid. The test states it as a limitation rather than
   pretending otherwise. What would separate them is not in a frame pair — a shadow tracks its
   illumination source and never moves against the light, and it does not persist the way a body does.
   Both need temporal and illumination-relative analysis, and neither is claimed here.
2. **`localized_unipolar` is not proof of a lamp.** An object arriving and staying looks the same as a
   source switching on.
3. **Dense sensor noise with both polarities above the delta threshold classifies as `diffuse_motion`** —
   a painted boundary, and a real one: a noisy camera at dusk can look like diffuse movement.
4. **The classifier's thresholds are calibrated on one camera, one room and one day.** The idle band
   (≈1%) is a property of this sensor, this exposure and this lighting; it must be re-measured before
   deployment, and the honest form is a per-session calibration rather than a constant.
5. **The episode gate keeps the episode path opt-in.** Nothing here justifies enabling it: the gate
   removes the lighting false positives in the fixtures, but it is itself unvalidated against real
   lighting changes, because staging one needs an operator in the room.
6. **No microphone work.** Authorized, not activated; the audio evidence kind still has no producer.
7. **Latency is unchanged and still unbudgeted.**
8. **The observation rate is not wall-clock controlled** (`PHYSICAL_CAMERA_SCENARIOS.md` §9), so window
   contents still vary with the sensor's throughput.

## 9. Recommendation

1. **Keep the change character computed and reported** — it is additive, it costs one plane pass, and it
   is the first evidence in this project that distinguishes a light from a body.
2. **Keep `require_motion_character` disabled**, and validate it first with the staged procedure below.
   The fixtures say it removes both lighting false positives without losing a confirmed movement; the
   fixtures are not a room.
3. **Before enabling anything, measure the real lighting case** (phase 2). If a real light produces
   `global_illumination` on real frames, the gate is doing what the synthesis says; if it produces
   `localized_motion`, the discriminator is wrong for this camera and the work stops there.
4. **Next work, in order:** a per-session idle-band calibration (replace the constant with a measured
   one); illumination-relative features for limitation 1 (shadow vs body); a wall-clock observation
   cadence; then the staged procedure, then a day of timed ambient runs to turn samples into rates.

### Staged procedure (needs an operator, ~3 minutes)

```bash
./build/hg_camera_bridge --capture --confirm --frames 600 --cadence 5 --log docs/evidence/STAGED_01_STILL.log
# phase 2 - the room light switched on and off a few times during the run  <- the decisive test
# phase 3 - a hand waved slowly across the lens for ~1 s
# phase 4 - sustained movement in view for ~10 s
# phase 5 - the lens covered for ~5 s and uncovered
# phase 6 - the camera mount tapped repeatedly for ~5 s
```

Each capture now prints a character column per observation, so phase 2 is read directly: `global_illumination`
for the lighting frames, `localized_motion` for the movement frames, and nothing else.

---

## Real-hardware results: an induced illumination change (2026-10-19, branch as above)

The first test that is not synthetic. Rather than waiting for a person to switch a light, the camera's
**own V4L2 brightness control** was stepped (±10 from 0, its full range) while a 600-frame capture ran,
so the illumination change is real, camera-visible and applied under control. The honest caveat sits at
the top of the result: this is a **uniform gain change, not a lamp** — a room light has spatial
structure, and this does not.

`docs/evidence/PHYSICAL_CAMERA_BRIGHTNESS_STEP.log` (before), `..._STEP2.log` (after), both 600/600
frames, cadence 5, 120 observations, 0 driver-dropped frames, ~40 s.

| | STEP (before gain correlation) | STEP2 (after) |
|---|---|---|
| peak motion / ratio | 0.9744 / 3.40 | 0.9841 / 3.80 |
| peak confidence | 1.000 | 1.000 |
| `quiet` | 43 | **93** |
| `diffuse_motion` | 54 | **8** |
| `localized_motion` | 11 | 11 |
| `global_illumination` | 9 | 5 |
| frames labelled as any motion | 65 / 120 | **19 / 120** |

**What this establishes.** The four largest changes (0.44–0.98 of all pixels, i.e. the steps I applied)
are classified `global_illumination` on real frames, with the reason naming the gain explanation. The
region descriptors work on real data for the case that matters most: a large illumination change is not
called movement.

**What it also established, by failing.** 65 of 120 frames carried a motion character in a scene where
the only deliberate change was illumination. Spatial statistics alone could not separate the camera's
**automatic-gain response** from movement: AGC produces changes that are compact, structured and
sometimes bipolar, which is exactly the signature the classifier reads as a body. Correlating the signed
delta against each pixel's own previous value fixed it — a gain multiplies the scene, so the delta scales
with brightness (correlation → +1), while a body changes a region by a contrast its background knows
nothing about (correlation → 0). Adding `gain_correlation` and checking it first moved **46 frames from
motion to quiet**, with `localized_motion` unchanged at 11 and the big steps still recognised.

**Residual, stated as a limitation rather than hidden.** 19 of 120 frames still read as motion in a
static scene (11 `localized_motion`, 8 `diffuse_motion`): AGC transients at lower amplitude and scene
structure. The episode gate refuses a run only when at least half its members are `global_illumination`,
so a run of these would remain eligible — which is one more reason the gate stays disabled by default.

**Still unrun, and not claimed.** A person crossing the field with consent, a small object moved by
hand, shadow movement, reflections, real camera movement, curtains or vegetation. Every one of them
needs a human in the room to stage it, and none was present for this capture. The descriptors are
therefore validated on real illumination and synthetic motion — not on real motion.

## Per-session idle-band calibration and region tracking

The measured idle band (~0.0098 median, ~0.0106 p90 on this camera) is a property of *this* sensor,
exposure and lighting, so the constant floor was the wrong shape. `CameraActivitySession` now measures
its own band over the first `idle_calibration_samples` (8) frame pairs and lifts the character's quiet
floor to `idle_band_multiplier` (2.0) times the measured median, never below the policy default; the
measured value is reported in the observation as `idle_band` and in the finding as `idle_band`. A
subject present during calibration inflates the floor, which suppresses rather than invents detections —
the safe direction, and it is logged.

Region tracking is deliberately minimal and honest: `region_travel` is the displacement of the changed
mass's centroid since the previous *characterised* frame, and `travel_consistent` counts consecutive
frames travelling roughly the same way. A quiet frame has no centroid and resets both, rather than
inventing movement.

## The same experiment, three runs, two very different answers

The tail attribution below was written from run 2's analysis and then measured against a third run of
the *identical* experiment. The third run does not confirm it, and the honest record is both runs side
by side.

| run (all 600/600 frames, 120 observations, 0 driver drops) | quiet | localized_motion | diffuse_motion | global_illumination | motion-labelled |
|---|---|---|---|---|---|
| 1 · `PHYSICAL_CAMERA_BRIGHTNESS_STEP.log` | 43 | 11 | 54 | 9 | **65** |
| 2 · `PHYSICAL_CAMERA_BRIGHTNESS_STEP2.log` | 93 | 11 | 8 | 5 | **19** |
| 3 · `PHYSICAL_CAMERA_TAIL_STEP.log` | 51 | 35 | 26 | 5 | **61** |

Run 3: peak motion 0.9786, ratio 4.39, peak confidence 1.000, windows `sustained_increase=6
sustained_decrease=24 quiet=85`, verdict "a sustained increase WAS observed".

**Zero `illumination_settling` labels appeared in run 3.** The attribution requires both an illumination
step within 16 observations AND a changed mass whose centroid did not travel more than 0.02. On real
frames the changed mass's centroid moved between observations, so the condition declined to fire. That is
the safe direction - it refuses rather than mis-attributes - but it means the *real-frame* calibration of
the travel epsilon is not established, and only the synthetic tests pin the logic.

**Why runs 2 and 3 differ is not known, and the difference is not the code.** The only production change
between them can convert a motion label into `illumination_settling`; it cannot add motion labels. So the
scene differed between the runs. The most likely cause is the one this document has to name: **the
designated test area was not controlled for occupancy.** This camera looks at the workstation area, and
run 3 was taken while the owner was active at that workstation; run 2 was not. Real movement in the scene
would produce exactly this - more, and travelling, motion labels - and would be *correct*, not a false
positive. That reading is plausible and **unproven**: no occupancy was recorded, so the possibility that
this is descriptor instability instead cannot be excluded from this evidence alone.

**What follows from it.** The three-run spread is the strongest argument yet for keeping
`require_motion_character` and the episode path disabled: on this camera, with this code, the same
controlled lighting experiment produced 19 motion labels once and 61 another time, and the difference is
unattributed. Establishing a trustworthy false-positive rate requires a capture whose occupancy is
recorded - ideally the same scene, once known-empty and once with a consented person - before any
threshold or epsilon is calibrated on it. Tuning the travel epsilon so run 3's numbers look like run 2's
would be fitting to an unmeasured scene, which is exactly what this work has refused to do.
