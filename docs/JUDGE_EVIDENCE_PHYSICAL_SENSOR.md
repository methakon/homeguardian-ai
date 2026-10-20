# Judge-facing evidence note — physical sensor path (camera)

**Source commit:** `f6afdcf4bdf27d713302faba002d4d38fea200b6`
**Branch:** `experiment/hg-v380-09-secure-camera-integration` (experimental, not merged)
**Date:** 2026-10-19
**Scope:** one owner-authorized USB UVC camera on the development workstation, reduced to bounded
motion scalars. No V380 camera work, no Oracle involvement, no production integration.

This note exists so a reviewer can check every claim against a measurement and an artifact, and see
precisely where the evidence stops. Nothing here is a product claim about recognising anything.

## 1. Exact claim table

| # | Claim | Verdict | Measured evidence | Artifact | NOT claimed |
|---|---|---|---|---|---|
| 1 | Real capture from an authorized physical camera, not a simulator | **VERIFIED** | 600/600 frames from `/dev/video0` (`uvcvideo`, YUYV 640x480), 0 stalls, 0 driver drops, 42.6 s; every observation labelled `provenance: "real"` | `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` §3 | nothing about any V380 camera |
| 2 | Bounded scalar-only processing; no frame stored, written, streamed or transmitted | **VERIFIED** | frames reduced to a luma plane in place and discarded; no file, socket or stream in the capture path; capture requires the `--confirm` operator gate | `src/backend/linux/V4l2CameraDevice.{h,cpp}`, `src/core/CameraActivitySession.{h,cpp}`, `src/main_camera_bridge.cpp` | — |
| 3 | A sustained change in motion in view is detected from **real** frames | **VERIFIED** | peak motion **0.1252** against a still-scene ceiling of **0.0103** (12x); **six consecutive** confirmed `sustained_increase` windows, ratios 1.85-2.70, frames 255-281; peak confidence **0.849** | capture log §3, §3.1 | **who or what moved** — the tool has no person/animal/species model, and the verdict is phrased as motion in view only |
| 4 | A still scene produces no false alert | **VERIFIED** | 120-frame and 600-frame still runs: `sustained_increase=0`, `MOTION-VERDICT: NO sustained increase`; the durable 90-frame still log agrees | capture log §1, §6; `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | — |
| 5 | A large but **brief** change does not become a false alarm | **VERIFIED** | still-scene run peaked at motion **0.2664** — larger than the accepted run's 0.1252 — and still produced `sustained_increase=0`; rule verdict `not_detected` at 0.056 | `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | — |
| 6 | Observations reach the **existing** event, correlation and alert pipeline with **no parallel detector and no new event model** | **VERIFIED** | `Observation::from_event` round-trips (status `detected`, confidence intact); `Application::process_event()` consumes them; `DetectionFusionRule` raises an alert over peak-confidence camera observations | `tests/core/test_camera_activity.cpp` `[camera][integration]` | that a **real** fused alert was observed — see §5 |
| 7 | The rule's confidence gate is reachable by real camera evidence, without changing it | **VERIFIED** | measured peak confidence **0.849** at ratio 2.70, against the analytic `|ratio-1|/2 = 0.85` predicted by the calibration analysis | capture log §3.2; `tests/core/test_camera_calibration.cpp` | — |
| 8 | **Real camera → fusion → ALERT** end to end | **NOT VERIFIED (GAP-11)** | the *fused* confidence over the real run's window was not measured; a brief change beside many quiet votes can fuse below the gate (quantified) | §5 below | — |
| 9 | Animal behaviour, species, emotion or intent inference | **NOT CLAIMED** | the finding vocabulary is closed and the tests assert it contains no person/animal/species/face/gesture/fall/emergency word; there is no classifier in the build | `tests/core/test_camera_activity.cpp` `[camera][finding]` | — |
| 10 | Calamity, earthquake, flood or other disaster prediction | **NOT CLAIMED, and unrepresentable** | the fusion frame of discernment has no element for it, so no code path can emit such a claim | `src/core/DempsterShafer.h`, `tests/core/test_camera_calibration.cpp` | — |
| 11 | V380 camera media (video, microphone, two-way audio) | **BLOCKED** | no supported authenticated interface exists on those units; the proprietary path is forbidden and untouched | `docs/HG_CAM_10_ADDITIONAL_CAMERA_ONBOARDING.md` | any live footage |

## 2. Test evidence

| Suite | Result |
|---|---|
| `[camera]` — capture path, bounds, finding vocabulary, stall semantics, provenance, gate, pipeline integration, and the crash regression | 22 cases / 217 assertions, passing |
| `[camera][analysis]` — EMA decay and fusion-window dilution, synthetic step input, provenance `synthetic` | 2 cases / 45 assertions, passing |
| Full suite | **7483 assertions / 574 cases, all passing** |

Synthetic calibration is deliberately kept in its own suite with `provenance: synthetic`. **No synthetic
result is presented as a physical measurement anywhere.**

## 3. Provenance of the evidence itself (raw vs transcribed)

Stated plainly so a reviewer can weigh it:

- **Transcribed:** the values in `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` for runs 1-5 are the
  capture tool's **own printed stdout**, recorded in the working session. Their original raw streams
  were written to `/tmp`, which the system has since cleaned, so the byte-for-byte originals no longer
  exist. The file says so at the top; it is a transcription, not the original stream.
- **Raw and unedited:** `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` was written directly to disk by
  the tool via its new `--log <path>` option and is committed exactly as produced. That option exists
  because of this gap: a capture whose transcript is gone cannot support the claim it made.
- Every capture cited above states its own frame count, stall count and driver-drop count, so a
  truncated or lossy run would be visible rather than inferred.

## 4. No raw footage

No frame, image, audio sample, clip or thumbnail is stored, transmitted or published by this work, and
none exists in the repository. The repository contains scalars, verdicts and counters only. The
`--confirm` operator gate means no capture can be started by running a binary or a test: it must be
requested explicitly.

## 5. GAP-11 — the distance to a fused real alert

What is proven: real motion produces observations whose confidence reaches **0.849** (> the unchanged
0.5 gate), and the existing rule fires on such evidence.

What is missing: the fused confidence **over the real run's window** was never measured, so no alert
from a real capture is claimed. The analysis quantifies why a threshold change would be the wrong
answer: with a 5 s window, `3` detected votes beside `4` quiet ones fuse to **0.245**, and a 1-second
change produces only **0.16** whether observations arrive per frame or per second. Alerting on a brief
change is therefore a *vote-mix* question (baseline rate and observation cadence), not a gate value.

Closing it costs one explicitly consented capture with the operator in frame, using the schedule and
command in `docs/PHYSICAL_CAMERA_PATH.md` §11 (still ~10 s, then move; the tool prints the rule's own
verdict). Per the current instruction this is deferred **until the P0 submission MCP gate is finished**,
and no unattended capture will be attempted in the meantime.

## 6. Optional 30-45 second video evidence plan

If judges would find it stronger, an *optional* short screen recording of the workstation can show the
demonstration without adding any new claim:

| Segment | Duration | Shows |
|---|---|---|
| 1 | 0-8 s | `hg_camera_bridge --probe` — the camera is discovered by the kernel, metadata only |
| 2 | 8-20 s | `--capture --confirm --frames 120` on a still scene: `MOTION-VERDICT: NO sustained increase` — the no-false-positive side |
| 3 | 20-40 s | the same command with the operator walking across the frame: `MOTION-VERDICT: a sustained increase WAS observed`, with the peak motion ratio on screen |
| 4 | 40-45 s | the printed observation JSON showing `"provenance": "real"` and the closed status vocabulary |

Constraints for any such recording: no QR code, provisioning screen, credential, device identifier,
LAN address or Wi-Fi name may appear; the console output is the subject, not the room; and the
recording must not be described as recognising anything. A recording is **presentation**, not
evidence — the evidence remains the capture logs and the tests.
