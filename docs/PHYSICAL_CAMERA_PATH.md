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
