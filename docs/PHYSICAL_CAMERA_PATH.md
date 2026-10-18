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

Every frame carried `provenance: real`. **No physical motion appeared in view during either attempt**, so
"real motion produces an activity-change observation" is **still unverified** and is not claimed. What the
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
