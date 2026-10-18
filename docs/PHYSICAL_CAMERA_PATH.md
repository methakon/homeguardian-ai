# The physical camera path (Ubuntu, V4L2)

**Date:** 2026-10-18
**Repository:** `methakon/homeguardian-ai`
**Branch:** `experiment/hg-v380-09-secure-camera-integration`

This document records the first **real physical sensor path** in the Linux build: a supported
kernel API, the owner's own hardware, bounded scalars, and an operator gate. It replaces nothing —
the V380 camera work stays blocked as recorded, no vendor protocol was touched, and no synthetic
scenario was added.

## 1. Why this path, and why now

Every other real capture route on this host is blocked by a missing dependency or a missing
authorized interface:

| Route | State |
|---|---|
| V380 cameras (both) | no supported authenticated interface exists; proprietary access is forbidden |
| Linux **audio** capture | needs ALSA development headers (absent) and the audio bridge's build is gated on PipeWire headers (absent) |
| Home Assistant environmental source | no instance is running here (nothing on 8123) |
| Android handset sensor | no device attached over adb at the time of this work |
| **V4L2 camera capture** | **works**: `/usr/include/linux/videodev2.h` is kernel UAPI already present, so capture needs **no library at all** — not OpenCV, not libv4l |

## 2. What was built

| File | Role |
|---|---|
| `src/backend/linux/V4l2CameraDevice.{h,cpp}` | real capture on the existing `IMediaDevice` contract: capability query, format negotiation with read-back, bounded mmap buffers, YUYV luma extraction, sequence-gap and stall counters, deterministic stop/close |
| `src/core/CameraActivitySession.{h,cpp}` | luma planes → the **existing** `extract_motion_features` → `changed_fraction` → the **existing** `ActivityChangeDetector` → one finding with a closed vocabulary and carried provenance |
| `src/main_camera_bridge.cpp` | `--probe` (metadata only) and `--capture --confirm` (bounded, scalars only) |
| `tests/core/test_camera_activity.cpp` | 19 cases: derivation, bounds, geometry change, vocabulary, stall semantics, provenance, the gate — **no hardware needed** |

**Reuse, not duplication:** there is no second feature extractor, no second detector and no second
event model. The camera path feeds the same components the audio path feeds.

**The gate:** `--capture` requires `--confirm`, and `V4l2CameraDevice::start()` throws unless the
operator has authorized capture. Constructing the object opens nothing; `--probe` captures nothing.
No shell history accident and no test can switch a camera on.

**Privacy:** a frame exists only long enough to be reduced to scalars. One bounded buffer is
retained (so the next frame has a reference); nothing is written, streamed, queued or transmitted,
and the chroma a colour frame carries is never copied.

## 3. First real run — measured

```
$ ./build/hg_camera_bridge --probe
  /dev/video0    driver=uvcvideo card=W100: W100   VIDEO_CAPTURE STREAMING   usable=yes   formats: YUYV
  /dev/video1    driver=uvcvideo card=W100: W100   STREAMING META_CAPTURE    usable=NO
  capturable nodes: 1     (the second node is the UVC metadata channel, not a camera)

$ ./build/hg_camera_bridge --capture --confirm --frames 30
negotiated: driver=uvcvideo card=W100: W100 bus=usb-... 640x480 YUYV stride=1280
frames captured 30 of 30 · stalls 0 · driver-dropped frames 0 · elapsed 2706 ms
finding: quiet_baseline — "motion within the established baseline"
  motion_level 0.0103   mean_delta 0.0068   baseline 0.0093   ratio 1.12
  observation: provenance "real", source "/dev/video0", detector camera-motion-activity,
               status not_detected, confidence 0.0
RESULT: 30 real frame(s) reduced to scalars; nothing retained.
```

Two things that run is worth stating plainly:

1. **It is a REAL observation.** The event carries `"provenance": "real"` because it was computed
   from physical device frames — the first time this build has produced one from a camera. The
   synthetic/demo paths cannot claim that, and the label cannot be set by a caller.
2. **A static scene produced no alert.** `not_detected` with a ratio of 1.12 against a learned
   baseline is the correct answer for a room that is not moving, and a false positive there would
   have been the more interesting failure.

**Not yet verified on real hardware:** that the path reports a *change* from real frames. That needs
someone to move in front of the camera during a capture; without physical motion the honest result is
the quiet one above. Motion-change detection itself is covered by the synthetic tests, including the
confirmation rule that rejects a single anomalous frame.

## 4. Two real defects found by running it (both fixed at the root)

1. **A crash on the first physical run.** `ActivityChangeDetector::to_observation` clamped confidence
   with `std::min(1.0, (ratio - 1.0) / 2.0)` — a one-sided clamp. A *confirmed falling* change has
   ratio < 1, so the expression goes negative and the validated `Event` constructor threw
   `std::invalid_argument`, aborting the process on a legitimate finding. Fixed to clamp at both ends
   over the magnitude of the deviation. This was reachable from the **audio** path too (a confirmed
   fall in level), so it was fixed in the detector rather than in the camera code that surfaced it.
2. **A self-contradicting finding.** `snapshot()` reported the raw last value as `ratio`, so a
   caller could print "sustained increase" beside a ratio below 1. The snapshot now returns the last
   *observed* result, so a finding's numbers can never disagree with its own verdict. The camera test
   that caught this asserts exactly that consistency.

Both have regression coverage, and the full suite passes: **7419 assertions / 570 cases**
(`[camera]` 19 cases / 161 assertions).

## 5. What is NOT claimed

- No person, animal, species, face, gesture, fall, intruder or emergency claim. The vocabulary is
  closed and asserted to be free of those words; motion energy is motion energy.
- No recording, streaming, storage or transmission. No frame was written anywhere.
- No 24/7 suitability, thermal or power evidence: one 2.7-second spot check.
- Nothing about the V380 cameras, whose media remains **BLOCKED** as recorded in
  `HG_CAM_10_ADDITIONAL_CAMERA_ONBOARDING.md`.
- The camera card name is generic device metadata; no private address, identifier or credential
  appears in this document or in the committed code.

## 6. Next verification, and its prerequisites

1. **Physical motion test (needs the owner, ~30 seconds):** run
  `./build/hg_camera_bridge --capture --confirm --frames 120` and walk in front of the camera. A
  `sustained_increase` finding with `provenance: real` is the missing piece of evidence; without it
  the path is proven to capture and to stay quiet, not to detect.
2. **Environmental channel (needs a decision):** the precursor layer's Category B path
   (barometric/humidity/temperature correlation) has an adapter and no source here. A Home Assistant
   instance or a node sensor would make it real; the adapter already refuses unauthorized samples.
3. **Audio channel (needs one package):** `libpipewire-0.3-dev` (or ALSA headers) would let the
   existing permission-gated audio bridge build. That is a system change, so it waits for approval.

---

## 7. Second capture round (120 and 600 real frames) — real motion still UNVERIFIED

Two further bounded captures were run with the operator present and approval granted:

| Run | Frames | Stalls | Driver drops | Wall | Motion range | Findings |
|---|---|---|---|---|---|---|
| quiet baseline | 40 / 40 | 0 | 0 | — | — | `not_detected`, no alert |
| motion attempt A | 120 / 120 | 0 | 0 | 7806 ms | 0.0000 – 0.0103 | 3 `warming_up`, 115 `quiet_baseline` |
| motion attempt B | 600 / 600 | 0 | 0 | — | 0.0000 – 0.0102 | 3 `warming_up`, 595 `quiet_baseline` |

Every frame carried `provenance: real`. **No physical motion appeared in view during either attempt**
(see §12 for the consented capture that followed, in which it did). What the
runs do establish:

- the camera is **live**, not frozen: the driver's sequence number advances frame by frame and the
  frame-to-frame deltas are non-zero, so these are distinct captured frames rather than one repeated
  buffer;
- the path is **stable** over 600 frames: no stalls, no driver drops, no memory growth;
- a static scene produces **no alert** — twice, deliberately, which is item 4's answer.

The likely practical cause is framing: if the camera does not cover where the operator stood, the
capture is honest and quiet. The next attempt should be made **while standing inside the field of
view**, which is a physical fact this observer cannot check from a scalar stream.

## 8. Audio dependencies (item 7) — a correction: NO installation is needed

The premise of the task was that ALSA/PipeWire development headers had to be installed. **They are not
needed, and the earlier claim in this document that `libpipewire-0.3-dev` was required was wrong.**
Verified:

- `grep` of `CMakeLists.txt` for `pipewire|PIPEWIRE|libasound|ALSA` returns **nothing**: the build has
  no such dependency and no conditional gate around the audio target.
- `hg_audio_bridge` **builds as-is** (`Built target hg_audio_bridge`) with no package installed.
- The reason is the design: `PwAudioCaptureDevice` deliberately avoids libasound by driving PipeWire's
  own CLI tools through the very server the desktop uses, because this host has no ALSA headers and
  `sudo` requires a password. So the real-PCM path exists without a single new dependency.

`sudo -n true` confirms a password is required, so no package could be installed from this session
anyway — and none is required. **Audio dependency changes: none.**

## 9. Do physical sensor observations reach the existing pipeline? (item 9) — YES, measured

Question: can a physical-sensor observation reach the existing event, correlation, alert and MCP
pipeline without a parallel detector or a new event model? Answered by measurement, in
`[camera][integration]`:

| Measurement | Result |
|---|---|
| The observation round-trips as a first-class `Observation` | `Observation::from_event` returns **ok**, with status `detected` and its confidence intact |
| The existing pipeline consumes it | `Application::process_event()` — the same entry point the demo runner uses — increments `events_processed` |
| The existing fusion rule counts it as evidence | with `set_evidence_detectors({"camera-motion-activity"})` the observations become votes for `DetectionFusionRule` |
| **The rule fires on camera evidence** | over a window of three peak-transition camera observations (status `detected`, confidence **1.0**, ratio 6.67): **ALERT**, severity info/warning |

So there is **no parallel detector and no new event model** — the camera path emits the same model the
detector layer already emits, and one configuration call registers its detector as evidence.

### The two calibration findings (real, and deliberately not papered over)

1. **The confidence scale does not reach the rule's threshold in steady motion.** The rule requires
   fused confidence ≥ 0.5. The detector's confidence is `|ratio - 1| / 2`, so it needs ratio ≥ 2.0, and
   under *sustained* motion the EMA baseline adapts and the ratio decays toward 1 — measured at 1.79,
   i.e. confidence 0.39, no alert, over 14 observations.
2. **The fusion window dilutes.** The pipeline evaluates the rule over a 5-second window of
   observations; the early low-confidence `not_detected` votes contribute confidence 0.0 and drag the
   fused mean below the threshold.

Both are calibration decisions (threshold vs. confidence scale, and window composition), not wiring
gaps. **No threshold was loosened to manufacture an alert** — the wiring proof above uses the detector's
own peak confidence, which its existing scale produces without modification.

MCP visibility is unaffected and already covered: the read-only `get_alert_summary` tool is built from
the same Application state, and the `[animal]` suite asserts that its answer is derived from the alerts
the pipeline actually delivered.

---

## 10. Controlled calibration analysis (no thresholds changed)

The two problems found in §9 were analysed rather than tuned away. Method: a deterministic **step
input** (6 quiet windows, then a fixed-size large change) driven through the **real**
`ActivityChangeDetector`, and the **real** `FusionEngine` fed the **real** vote mix. Nothing was
modified — no threshold, no baseline rate, no window length, no confidence formula. Reproduce with
`./build/homeguardian_tests '[camera][analysis]'` (2 cases, 45 assertions).

### 10.1 EMA baseline adaptation

| `slow_alpha` | strong windows | windows reported `detected` | windows at confidence ≥ 0.5 | peak confidence |
|---|---|---|---|---|
| 0.05 (default) | 8 | 6 | — | 1.0 |
| 0.05 | 20 | 17 | 11 | 1.0 |
| 0.05 | 40 | 17 | 11 | 1.0 |
| 0.02 | 40 | 38 | — | 1.0 |
| 0.01 | 20 | 18 | — | 1.0 |
| 0.01 | 40 | 38 | **38** | 1.0 |

Two facts, both measured:

1. **The confidence REACHES 1.0 at the transition** in every configuration. So the scale is not
   "too low" — the earlier 0.39 was a *late* sample of a decaying signal, not a weak detector.
2. **A sustained change stops being reported at all once the baseline absorbs it**: at the default
   `slow_alpha = 0.05`, only 17 of 40 strong windows were still `detected`; at 0.01, 38 of 40. The
   time spent above the rule's 0.5 gate follows the same mechanism: **11 windows at 0.05 versus 38 at
   0.01**, a factor of ~3.5.

So the lever for sustained-change sensitivity is the **baseline adaptation rate**, and it is a design
choice with a stated cost: a slower baseline stays sensitive longer and is slower to accept a new
normal.

### 10.2 Five-second window dilution (real engine, real vote mix)

| detected votes (confidence 1.0) | quiet votes (confidence 0.0) | fused confidence | clears the 0.5 gate? |
|---|---|---|---|
| 1 | 0 | 0.00 | no — `unavailable`: fewer than 2 usable inputs |
| 3 | 0 | 1.00 | **yes** |
| 3 | 4 | 0.245 | no |
| 5 | 4 | 0.309 | no |
| 15 | 4 | **0.623** | **yes** |
| 15 | 10 | 0.360 | no |
| 30 | 4 | **0.779** | **yes** |
| 30 | 60 | 0.160 | no |

Dilution is monotone — adding quiet votes to the same evidence never raises the fused confidence
(asserted in the test) — and it is vicious: **3 detected votes beside 4 quiet ones fall to 0.245**.

### 10.3 The consequence, in the units that matter

| Scenario (5 s window) | Votes | Fused | Verdict |
|---|---|---|---|
| a 1 s change, one observation per frame | 75 (15 detected + 60 quiet) | **0.16** | no alert |
| a 1 s change, one observation per second | 5 (1 + 4) | 0.16 | no alert |
| a 5 s change, one observation per frame | 75 (75 + 0) | **1.00** | alert |
| a 5 s change, one observation per second | 5 (5 + 0) | 1.00 | alert |

The observation-cadence code path is now a supported knob (`--cadence` in the bridge), because a
per-frame observation stream puts ~75 votes in the window and dilutes any brief change.

### 10.4 What this says, and what it does not do

- The 0.5 gate is **reachable** by real camera evidence: at the moment of a change the detector
  produces confidence 1.0, and §9 shows the rule firing on that evidence.
- What decides an alert for a *sustained* change is the **vote mix**, governed by two design levers:
  the baseline adaptation rate (§10.1) and the observation cadence plus how long the change persists
  inside the window (§10.3).
- **No threshold was changed.** These are measurements presented so the choice is made with numbers;
  the choice itself (stricter sensitivity vs fewer false alarms) belongs to the owner.

## 11. The prepared motion capture

The bridge now supports a coordinated capture, so a person knows when to move:

```
./build/hg_camera_bridge --capture --confirm --frames 200 --hold 15 --cadence 5
```

- `--hold 15` counts down for fifteen seconds before any frame is captured, so the operator can step
  into the field of view (the countdown is printed and flushed one line per second).
- `--frames 200` bounds the run (~15-20 s of real capture at this camera's rate).
- `--cadence 5` emits one observation every fifth frame, i.e. ~3 observations per second, so the
  5-second fusion window holds ~15 votes rather than ~75 (§10.3).
- Reports at the end: peak motion, peak ratio, **peak confidence**, windows by kind, and an explicit
  `MOTION-VERDICT` line stating whether a sustained increase was observed — plus the observation JSON
  with `provenance: "real"`.
- Stores nothing: frames are reduced to scalars in place and no file, socket or stream is involved.

---

## 12. REAL MOTION VERIFIED (the consented capture)

Capture run with the operator in frame under the schedule below. **600 frames, 120 observations,
0 stalls, 0 driver drops, 42.6 s, every frame `provenance: real`, nothing stored.**

### 12.1 The protocol, and why it had to change

The first consented attempt (250 frames) was **mis-phased**: the operator stepped into view as the
countdown ended, so the motion was present *while the baseline formed*, the baseline learned the
movement as normal, and what followed was the operator settling. The detector reported
`sustained_decrease=13, sustained_increase=0` — with a peak motion of **0.0384**, four times every
quiet run. It was reading the scene correctly.

The corrected schedule, which the two-phase capture implements:

```
0-10 s    countdown            get in front of the camera, stay still
10-20 s   frames 1-150         STAY STILL - the baseline forms here
20-50 s   frames 150-600       MOVE continuously (walk across, wave, sit/stand)
```

### 12.2 The measurement

| Metric | Value |
|---|---|
| frames / observations | 600 / 120 (cadence 5) |
| peak motion (`changed_fraction`) | **0.1252** — against a quiet-scene ceiling of 0.0103 from this session's still runs, i.e. **12x** |
| peak ratio (recent/baseline) | **2.70** |
| peak confidence | **0.849** — above the rule's 0.5 gate |
| finding windows | `sustained_increase=6`, `sustained_decrease=41`, `quiet_baseline=68` |
| the increase, window by window | `ratio 2.40, 2.59, 2.70, 2.50, 2.32, 1.85` over frames 255-281 |

**So: real physical motion in view produces a sustained activity-change observation.** That is the
claim this document previously withheld, and it is now supported by the numbers above — with the
vocabulary unchanged: it is a *sustained increase in motion in view*, and nothing more.

**The calibration analysis and the field measurement agree.** §10.1 predicted confidence
`= |ratio - 1| / 2`; at the observed ratio of 2.70 that is 0.85, and the measured peak was **0.849**.
The gate is therefore reachable by real camera evidence, and the reason it is not *always* reachable
is the vote mix (§10.3), not the scale.

### 12.3 A protocol finding worth keeping

A baseline is a claim about what is *normal*, and whatever is happening while it forms becomes
normal. This was measured twice from opposite directions: a still scene on a still baseline is
quiet, and a moving scene on a moving baseline is quiet too — the *change* is what the detector
reports, never the absolute level. Any field deployment must therefore form its baseline in the
condition it plans to call normal.

### 12.4 The rule over a real capture's own observations

`hg_camera_bridge` now evaluates **the existing `DetectionFusionRule` and `FusionEngine`** over the
observations the capture itself produced, and prints the verdict — no parallel detector, no new event
model, no threshold change:

```
== RULE EVALUATION over this capture's own observations ==   (still scene, 60 frames)
  observations in the window : 12
  fusion verdict             : detected (confidence 0.081, 7 contributing)
  detection-fusion rule      : no alert
```

That is the no-false-positive side of the ledger on real frames.

### 12.5 Still NOT claimed

- **No end-to-end alert from real motion yet.** The motion capture produced peak confidence 0.849 and
  six sustained-increase windows, and the rule is proven to fire on such evidence (§9); but the fused
  confidence *over that run's window* has not been measured, and §10.3 shows a window holding a brief
  change beside many quiet votes can still fuse below 0.5. Re-running the same schedule now prints the
  rule verdict directly, which closes it in one capture.
- No person, animal, species, face, gesture, fall, intruder or emergency reading: the vocabulary
  remains closed and asserted to be free of those words.
- No recognition, identity, storage or transmission of any frame.
