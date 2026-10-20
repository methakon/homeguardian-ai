# Submission inputs — physical sensor (camera) path

**Supplied to:** the submission lead (Antigravity) for the README, project description and demo.
**Source commit:** `9d252c46f6fca2008e5e0285107002470d451df4`
**Branch:** `experiment/hg-v380-09-secure-camera-integration` — **experimental and isolated; not merged.**
**Status:** factual inputs only. No further capture, no microphone, no threshold change, no merge.

Everything below is copy-paste ready. Numbers are from committed evidence; nothing is rounded up and no
result was produced by tuning anything.

---

## 1. README wording (short paragraph — safe to paste)

> **Physical motion sensing (real hardware).** `hg_camera_bridge` captures from an owner-authorized USB
> UVC camera through the Linux V4L2 kernel API — no external library — reduces each frame to its luma
> plane in place, and reports one bounded motion finding per observation. In a consented 600-frame
> capture, a sustained change in motion in view produced **six consecutive confirmed findings**
> (peak motion **0.1252** changed-fraction against a **0.0103** still-scene ceiling, peak ratio
> **2.70**, peak detector confidence **0.849**), while still-scene captures produced **none**. Frames
> are never stored, transmitted or streamed, and capture requires an explicit `--confirm` operator
> flag. The detector reports *that a sustained change in motion occurred in view* — it does not
> identify people, animals, species or events.

## 2. Project-description wording (Devpost-style, ~90 words)

> The camera path turns real frames into a single bounded scalar — the fraction of luma pixels that
> changed beyond a fixed per-pixel threshold — compares it against a learned baseline, and reports one
> explainable finding. It is deliberately boring by design: *a sustained increase in motion in view*,
> *within the established baseline*, *the camera delivered no frame*, or *insufficient evidence*.
> Measurements come with their provenance (`real` for device frames, `synthetic` for fixtures), and the
> result vocabulary is closed, so a report cannot drift into recognising a person, an animal or an
> event. On real hardware the path separated a still room (~0.010 changed-fraction) from sustained
> movement (peak 0.125, confidence 0.849) with no false alert on any still capture.

## 3. What is actually detected (one sentence for judges)

> **A sustained change in the amount of motion in view, measured from real frames, with an explicit
> confidence and provenance label — and nothing else.**

Explicitly *not* detected: people, animals, species, faces, gestures, falls, intruders, emergencies,
and no prediction of any kind.

## 4. Real measurements vs synthetic tests (kept strictly separate)

| Kind | What it is | Provenance label | Where | Numbers |
|---|---|---|---|---|
| **Real** | Frames captured from the physical USB camera under the operator gate | `real` | `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` §1–§3; `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` | 600/600 frames, 0 stalls, 0 driver drops, 42.6 s; peak motion 0.1252; ratio 2.70; confidence 0.849; 6 confirmed increases; still ceiling 0.0103/0.0102; durable still log: 90 frames, peak motion 0.2664, `sustained_increase=0` |
| **Synthetic** | Deterministic fixtures driving the same detector and fusion rule | `synthetic` | `tests/core/test_camera_activity.cpp` `[camera]`; `tests/core/test_camera_calibration.cpp` `[camera][analysis]` | 22 cases / 217 assertions; 2 cases / 45 assertions |
| **Wiring (no hardware)** | The path's plumbing and refusals: bounds, geometry change, stall semantics, the provenance rule, the capture gate, pipeline integration | — | `tests/core/test_camera_activity.cpp` | included above |

**Rule for the submission:** never present a synthetic number as a measurement. The synthetic suites
exist to prove the *logic* (bounds, refusals, the confirmation rule, uncertainty handling); the capture
logs are the *measurements*. This split is enforced in the code by the provenance label, not by editorial
care.

## 5. Evidence paths and exact SHAs (External Experiment Branch)

> **Important Boundary Note:** No physical camera integration and no V380 camera integration are included in PR #20. The USB camera driver (`V4l2CameraDevice`), bridge tool (`main_camera_bridge`), and capture evidence logs listed below reside exclusively on external experiment branch **`experiment/hg-v380-09-secure-camera-integration`** (HEAD commit `9d252c46f6fca2008e5e0285107002470d451df4`). They are unmerged and not deployed into `release/hackathon-2026-mcp-candidate` (PR #20). The clean judge candidate runtime runs strictly fail-closed (`media_capture_enabled: false`, `physical_camera_unverified`) using synthetic observation fixtures.

| Item | Path / SHA (on `experiment/hg-v380-09-secure-camera-integration`) |
|---|---|
| Judge-facing evidence note | `docs/JUDGE_EVIDENCE_PHYSICAL_SENSOR.md` |
| Implementation notes, calibration analysis, capture procedure | `docs/PHYSICAL_CAMERA_PATH.md` |
| Capture log (run-by-run measurements) | `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` |
| Raw, unedited capture transcript | `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` |
| Capture device (external branch) | `src/backend/linux/V4l2CameraDevice.{h,cpp}` |
| Finding vocabulary + session (external branch) | `src/core/CameraActivitySession.{h,cpp}` |
| Operator-gated tool (external branch) | `src/main_camera_bridge.cpp` |
| Tests (external branch) | `tests/core/test_camera_activity.cpp`, `tests/core/test_camera_calibration.cpp` |
| Docs / evidence commit | **`9d252c46f6fca2008e5e0285107002470d451df4`** |
| Durable-log + evidence commit | `f6afdcf4bdf27d713302faba002d4d38fea200b6` |
| Motion-verified commit | `70a994e63d29ce1274b44ddb6406c640005f924d` |
| Calibration-analysis commit | `7388ae3570f2029ac982a2148102453fefb66c76` |
| First physical path commit | `b21b70a2334dc6c2c7fba95ed0e1798cf57841e5` |

Full suite at the source commit on experiment branch: **7483 assertions / 574 cases, all passing.**

## 6. Provenance honesty (must survive into any published wording)

- Runs 1–5 of the capture log are a **transcription** of each capture's own stdout, recorded in the
  working session. Their raw streams were written to `/tmp` and the system cleaned them, so the
  byte-for-byte originals no longer exist. The log file says so at the top.
- `docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` is **raw and unedited**, written directly to disk by
  the tool's `--log` option — which now exists because of that gap.
- Consequence for the submission: cite the *measurements* freely, and if a reviewer asks for the raw
  stream, the honest answer is "one raw artifact exists in Git; the earlier ones were lost to a
  temporary directory and survive as a labelled transcription."

## 7. Consent and no-raw-media caveats

- Capture requires `--confirm`; no binary, script or test can switch the camera on by itself.
- No frame, image, clip, thumbnail or audio sample is stored, transmitted, streamed or published. The
  repository contains scalars, verdicts and counters only.
- The camera is a device on the owner's own workstation, used with the owner present and consenting.
- Nothing about any other camera, device or network is read, changed or published.

## 8. Suggested 20–30 second video segment (plan only — no filming now)

Assemble from **existing committed evidence**; no new capture is required or authorised. Total ≈ 26 s.

| # | ~Time | Shot | On screen |
|---|---|---|---|
| 1 | 0–6 s | `./build/hg_camera_bridge --help` | The tool's own contract on screen, including the `--confirm` operator gate — no device is touched at all |
| 2 | 6–13 s | `cat docs/evidence/PHYSICAL_CAMERA_STILL_SCENE.log` (committed) | The raw still-scene transcript: `MOTION-VERDICT: NO sustained increase observed`, `sustained_increase=0`, rule `no alert` |
| 3 | 13–21 s | The measurement block from `docs/evidence/PHYSICAL_CAMERA_CAPTURE_LOG.md` §3 (committed) | `peak motion 0.1252 / ratio 2.70 / confidence 0.849`, six consecutive `sustained_increase` windows, `provenance: "real"` |
| 4 | 21–26 s | `./build/homeguardian_tests '[camera]'` | The camera suite passing: the vocabulary, the bounds, the gate and the stall semantics |

If a consented live-motion shot is later authorised, it may be added as an optional shot 3b; until then
this segment is entirely reproducible from committed artifacts, which is a stronger position than a
recording anyway.

Constraints: the console output is the subject — no QR code, provisioning screen, credential, device
identifier, LAN address or Wi-Fi name may appear; the camera's surroundings are not shown; and no
caption may use recognition language.

## 9. Excluded — do not publish these

- **Animal or species identification** — not implemented; no classifier exists in the build.
- **Disaster / calamity prediction** — not implemented, and *unrepresentable*: the fusion frame of
  discernment has no such element.
- **V380 streaming or media** — no supported authenticated interface on those units; media is
  **BLOCKED**, camera 1's ONVIF/RTSP remains OFF, and no proprietary protocol was touched.
- **A real camera-triggered alert** — not demonstrated. See row 4 of §10.
- **Retail model / specs for any camera**, and any identity, person or behaviour claim.

## 10. CLAIM PASS / UNPROVEN table

| # | Claim | Status | Basis |
|---|---|---|---|
| 1 | Real capture from an authorized USB UVC camera via V4L2, no external library | **PASS** | 600/600 frames, 0 stalls, 0 driver drops, 42.6 s, `provenance: "real"` |
| 2 | Bounded scalar-only processing; no frame stored, transmitted or streamed; operator gate required | **PASS** | code paths + `--confirm` gate + no media in the repository |
| 3 | A sustained change in **motion in view** is detected from real frames | **PASS** | six consecutive confirmed windows, peak motion 0.1252 vs 0.0103 still ceiling, ratio 2.70, confidence 0.849 |
| 4 | A **real camera → fusion → delivered alert** end to end | **UNPROVEN** | the fused confidence over the real run's window was never measured; the wiring is proven by test and the gate is reachable (0.849 > 0.5), but no alert from real frames has been observed. Closing it needs one consented operator-in-frame capture |
| 5 | No false alert on a still scene | **PASS** | 120-, 600- and 90-frame still captures: `sustained_increase=0`; a *brief* change peaking at 0.2664 was also refused as non-sustained |
| 6 | Observations reach the existing event / correlation / alert pipeline with no parallel detector or new event model | **PASS** | `Observation::from_event` round-trip, `Application::process_event()`, `DetectionFusionRule` firing on peak-confidence camera evidence (`[camera][integration]`) |
| 7 | Detector confidence reaching the rule's unchanged gate on real evidence | **PASS** | 0.849 measured at ratio 2.70, matching the analytic `|ratio-1|/2` |
| 8 | **Species / animal / person classification** from any signal | **UNPROVEN** | no classifier in the build; the finding vocabulary is asserted free of such terms |
| 9 | **Disaster or calamity prediction** | **UNPROVEN** | not implemented, and unrepresentable in the fusion frame |
| 10 | **V380 camera media** (video, microphone, two-way audio) | **UNPROVEN** | no supported authenticated interface; proprietary access forbidden; media reported BLOCKED |
| 11 | Real-time continuous operation, 24/7 reliability, thermals | **UNPROVEN** | one 42.6-second spot check and short still captures; the workstation was measured as not-always-on |

Tests at this commit: `[camera]` 22 cases / 217 assertions, `[camera][analysis]` 2 / 45, full suite
**7483 assertions / 574 cases, all passing**.

---

Standing by for Antigravity's integration requests. No further capture, no microphone activation, no
threshold change and no merge will be performed without a new authorised reason.
