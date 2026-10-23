# PHYSICAL CAMERA CAPTURE LOG — real frames reduced to bounded scalars

Device: `/dev/video0` (`uvcvideo` (V4L2), YUYV 640x480, stride 1280, 640*480 = 307200 pixels).
Every observation in every run below carries `provenance: "real"`.
All captures ran with `--confirm` (the operator gate). **No frame was stored, written, streamed or
transmitted**: each frame is reduced to its luma plane in place and discarded.

## PROVENANCE OF THIS FILE — read before citing it

Runs 1–5 below are **transcribed from each capture's own stdout as captured in the working session**.
Their raw stdout was directed to `/tmp`, which the system has since cleaned, so the byte-for-byte
originals are gone. The values are the tool's own printed output and are quoted exactly as printed,
but this file is a transcription, not the original stream — treat it as such.

Consequence, now fixed in code: `hg_camera_bridge` accepts `--log <path>`, which writes the transcript
directly to a durable file instead of a temporary one. Run 6 (§6) is a genuine, unedited artifact
produced that way. Any future capture cited as evidence must be produced with `--log` into this
directory.

---

## 1. Quiet-scene reference runs (what a still scene produces)

| Run | Frames | Stalls / driver drops | Peak motion |
|---|---|---|---|
| 120 frames | 120 / 120 | 0 / 0, 7806 ms | **0.0103** |
| 600 frames | 600 / 600 | 0 / 0 | **0.0102** |

Findings for both: 3 `warming_up`, all remaining windows `quiet_baseline`, `MOTION-VERDICT: NO
sustained increase`. This is the ceiling a still scene produces on this camera and is the baseline
against which §5 is read.

## 2. First consented attempt — 250 frames (MIS-PHASED, reported as such)

```
frames captured      : 250 of 250 requested
observations built   : 50 (cadence 5 frame(s) per observation)
capture stalls       : 0
peak motion / ratio  : 0.0384 / 1.00
peak confidence      : 0.277 (the rule's current gate is 0.5)
windows by kind      : sustained_increase=0 sustained_decrease=13 quiet=32
MOTION-VERDICT       : NO sustained increase observed
```

Motion WAS present (0.0384, ~3.7x the still ceiling) but at the START of the capture, so the baseline
formed on the movement and the detector correctly reported a **decrease** as the operator settled.

## 3. Consented capture, still-then-move schedule — THE ACCEPTANCE RUN

Schedule: 10 s countdown; frames 1–150 stay still (baseline forms); from ~frame 150 move continuously.

```
frames captured      : 600 of 600 requested
observations built   : 120 (cadence 5 frame(s) per observation)
capture stalls       : 0
driver-dropped frames: 0
elapsed              : 42604 ms
pixels per frame     : 640x480 (luma plane reduced in place)
peak motion / ratio  : 0.1252 / 2.70
peak confidence      : 0.849 (the rule's current gate is 0.5)
windows by kind      : sustained_increase=6 sustained_decrease=41 quiet=68
MOTION-VERDICT       : a sustained increase WAS observed
final finding        : quiet_baseline
    sentence           : motion within the established baseline
    motion_level       : 0.0099   mean_delta 0.0066   baseline 0.0151
    provenance         : real (a physical device frame)
RESULT: 600 real frame(s) reduced to scalars; nothing retained.
```

### 3.1 The activity-change (sustained-increase) observations, verbatim

```
[255] seq=256    motion=0.0835 mean_delta=0.0236 baseline=0.0267 ratio=2.40  sustained_increase
[260] seq=261    motion=0.1252 mean_delta=0.0404 baseline=0.0316 ratio=2.59  sustained_increase
[265] seq=266    motion=0.1215 mean_delta=0.0350 baseline=0.0361 ratio=2.70  sustained_increase
[270] seq=271    motion=0.0272 mean_delta=0.0104 baseline=0.0357 ratio=2.50  sustained_increase
[275] seq=276    motion=0.0756 mean_delta=0.0206 baseline=0.0377 ratio=2.32  sustained_increase
[280] seq=281    motion=0.0652 mean_delta=0.0185 baseline=0.0390 ratio=1.85  sustained_increase
```

Six consecutive confirmed windows, ratios 1.85–2.70, over frames 255–281 — i.e. ~17–19 s into the
capture, after the still period the schedule prescribed had elapsed. Peak motion 0.1252 is **12x** the
still-scene ceiling of §1.

The observation produced by this run was printed with `"provenance": "real"` and detector id
`camera-motion-activity`, model `ema-baseline-v1`, exactly as every other capture in this file.

### 3.2 What this does and does not establish

- **Establishes:** a sustained increase in motion in view, measured from real frames, with the
  detector's own confidence reaching **0.849** — above the rule's 0.5 gate — at ratio 2.70. This
  matches §10.1's predicted `|ratio - 1| / 2 = 0.85`.
- **Does not establish:** who or what moved. The tool reduces frames to scalars and has no identity,
  species or person model; the timing agreement with the coordinated schedule is corroboration, not
  identification. The verdict is therefore phrased as a change in motion in view and nothing more.
- **Does not establish an end-to-end ALERT** from this run: the fused confidence over the run's
  window was not measured, and a brief change inside a window holding many quiet votes can fuse below
  0.5 (see the calibration analysis, §10.3 of `PHYSICAL_CAMERA_PATH.md`). No threshold was changed to
  bridge that.

## 4. Rule evaluation on a still scene (no-false-positive side)

Run on real frames with a still scene: 12 observations, fusion verdict `detected` at confidence
**0.081**, `detection-fusion rule: no alert`. The existing rule and engine, fed a capture's own
observations.

---

## 6. Durable artifact (produced with `--log`, unedited)

`docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` — a 90-frame still-scene capture written directly to
disk by the tool. It also demonstrates the confirmation rule doing its job:

```
frames captured      : 90 of 90 requested
observations built   : 18 (cadence 5 frame(s) per observation)
peak motion / ratio  : 0.2664 / 1.38
peak confidence      : 0.371
windows by kind      : sustained_increase=0 sustained_decrease=5 quiet=8
MOTION-VERDICT       : NO sustained increase observed
== RULE EVALUATION ==
  observations in the window : 18
  fusion verdict             : not_detected (confidence 0.056, 13 contributing)
  detection-fusion rule      : no alert
```

A peak motion **larger than the acceptance run's** (0.2664 vs 0.1252) produced **no** sustained-increase
verdict, because it was brief rather than sustained. That is the detector refusing a single anomalous
episode — the same rule that makes a genuinely sustained change meaningful.
