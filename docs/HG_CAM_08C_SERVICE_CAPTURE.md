# HG-CAM-08C — Service-owned capture, screen-off, and microphone: RESULT

**Device:** Panasonic ELUGA I7 EE (`LRW86SC6RO8TZ99H`), Android 9 / API 28, arm64-v8a
**Transport:** Wi-Fi ADB `192.168.1.16:5555` (USB-free)
**Base commit:** `6895fe4cbcc8c341fa326792a0591f6bf46690d0`
**Branch:** `experiment/hg-cam-eluga-cctv`

## 1. Headline

Capture ownership moved from the activity into `CctvService`. All three gates pass:

| gate | result |
|---|---|
| Service owns camera + codec lifecycle | **PASS** — foreground-before-arm ordering verified; activity `onPause()` no longer touches capture |
| Screen-on encoded-frame delivery (service-owned) | **PASS** — 536 buffers @ 29.66 fps over ~18 s; 293 @ 29.70 fps |
| **Screen-off capture** | **PASS** — `mWakefulness=Asleep`, counters advanced +741 buffers @ 29.61 fps |
| Microphone (real samples) | **PASS** — 1,194,240 samples @ 48,155 Hz measured, ~99.9% non-zero |

The pipeline was MOVED, not reimplemented: the working Camera2 → MediaCodec configuration,
640x480 H.264, timestamp handling, buffer draining and restart behaviour are unchanged from HG-CAM-08B.

## 2. What changed

`CctvService` (`apk/src/org/homeguardian/harness/CctvService.java`) now owns:

- `Camera2EncoderProbe` in a new **UNBOUNDED** mode (`runMs <= 0`). A CCTV node captures until the
  operator stops it; the bounded 12 s mode is retained for acceptance probes.
- `AudioCaptureProbe` (new) — real `AudioRecord` PCM capture, separately armed.

New actions: `CCTV_ARM` / `CCTV_DISARM` (video only) and `CCTV_MIC_ARM` / `CCTV_MIC_DISARM`
(audio only). `CCTV_START`/`CCTV_STOP` keep owning the whole service lifecycle; `CCTV_START` also
arms video, which is the "promote to foreground, THEN start capture" ordering the brief requires.

`MainActivity` now holds **no** camera, codec or surface reference. It checks permission and sends
an intent. That is what makes `onPause()`/`onStop()` structurally unable to kill screen-off capture.

### Two real defects found and fixed by testing

1. **`onPause()` called `stopEverything()`.** Harmless while capture lived in the activity; fatal
   once the service owned it, because Android pauses the activity the instant the screen goes off.
   The activity's own (Camera1/native) devices still stop there; service-owned capture deliberately
   does not.
2. **The microphone path armed the camera.** `startMic2()` first sent `ACTION_START` to force
   foreground, and `ACTION_START` arming video is correct for the camera path — so a mic-only arm
   opened the camera. Measured: `arming capture` = 2 when it had to be 0. Fixed by restructuring
   `onStartCommand` so foreground promotion is **independent of the requested action**, and each
   sensor is armed only by its own action. Re-measured: mic-only arm gave `arming capture` = 0 and
   `camera2 device OPENED` = 0, with the mic live — then arming video alongside gave both streams
   concurrently.

The second defect was a genuine ordering bug in my own HG-CAM-08C work, caught only because the
brief required the streams be separately controllable and therefore made it testable.

## 3. Stage 2 — screen-on, service-owned

    foreground started (no sensor touched yet)
    arming capture: cam2probe: starting
    camera2 ids=[0, 1]   camera 0 hardwareLevel=LIMITED(LIMITED)
    camera2 device OPENED id=0
    encoder started 640x480
    capture session CONFIGURED with the encoder input surface

    cycle 1:  isForeground=true
    cycle 1 disarm: "disarming capture (operator disarm)"
              service STILL isForeground=true, camera clients []
    cycle 2:  encodedBuffers=536 keyFrames=36 fps=29.66     arm-ignored events: 0

Camera open/close per cycle is clean, and an explicit disarm releases the camera while leaving the
foreground service alive — the two are separately controllable, as required.

## 4. Stage 3 — screen-off capture (the real gate)

Off USB power for the whole test (`AC powered: false`, `USB powered: false`, discharging), so the
SoC was free to suspend — unlike the HG-CAM-07 attempt which ran on a charger.

    before:  mWakefulness=Asleep   mScreenState=OFF   isForeground=true
    150 s later: mWakefulness=Asleep   isForeground=true

Service-side counter trace while the screen was OFF:

    tick #31  encodedBuffers=4577  keyFrames=306  fps=29.61  lastPtsUs=3117218886
    tick #32  encodedBuffers=4725  keyFrames=315  fps=29.61  lastPtsUs=3122218806
    tick #33  encodedBuffers=4874  keyFrames=325  fps=29.61  lastPtsUs=3127252495
    tick #34  encodedBuffers=5022  keyFrames=335  fps=29.61  lastPtsUs=3132252399
    tick #35  encodedBuffers=5170  keyFrames=345  fps=29.61  lastPtsUs=3137252314
    tick #36  encodedBuffers=5318  keyFrames=355  fps=29.61  lastPtsUs=3142252221

**+741 encoded buffers across the screen-off interval, at a sustained 29.61 fps.** `lastPtsUs`
advanced 25.03 s across five 5 s ticks — media time tracked wall-clock time exactly, so frames
were genuinely produced, not merely a counter being incremented.

Screen back on: `mWakefulness=Awake`, capture still running (6,060 buffers), and the disarm action
still worked from the activity.

### What this does NOT prove

`/proc/uptime` advanced 155.8 s over a ~156 s wall interval, so **the SoC did not suspend**. An
active camera session evidently holds the CPU awake — which is the desired product behaviour
(capture keeps running) but means this is a screen-off test, **not** a deep-sleep/suspend test.
Survival across a genuine suspend remains unproven, and the >= 30 minute duration CAM-07 requires
has not been attempted. Both are recorded as gaps rather than glossed.

## 5. Stage 4 — microphone

`AudioCaptureProbe` reads real PCM from the device microphone and counts it. It never writes a
file, never transmits, and stores no audio; samples are read into a scratch buffer and dropped.

    mic STARTED source=MIC 48000Hz ch=1 PCM_16BIT bufferFrames=3840
    mic#1     frames=3840 totalSamples=3840   nonZero=3811
    mic#200   frames=3840 totalSamples=768000 nonZero=3836
    tick #3   rate=48000Hz ch=1 PCM_16BIT readCalls=184 samples=706560
              measuredRate=48256 nonZero=705698 AUDIO
    tick #4   readCalls=247 samples=948480 measuredRate=48188 nonZero=947366 AUDIO
    final      readCalls=311 samples=1194240 measuredRate=48155 nonZero=1192195 AUDIO

- **1,194,240 samples** delivered, measured rate **48,155 Hz** against a declared 48,000 Hz — the
  ~0.3% excess is the reader thread's own timing, and is reported rather than rounded away.
- **`nonZero` ≈ 99.9%** and the state reads `AUDIO`, not `SILENT`: real acoustic signal was
  captured. Had it been silent, the summary would say `SILENT` — the distinction is built in
  precisely so "permission granted" can never masquerade as "microphone works".
- Separately controllable from video: mic-only arm left the camera untouched (§2, defect 2).

Not done: no audio codec layer (AAC/Opus). Raw PCM is counted, not encoded. Speaker talkback
remains a separate milestone and is **not** claimed.

## 6. Resources, permissions, cleanup

Both sensors live at once:

    CPU     30.8% of one core (3.17 s CPU / 10.28 s wall)
    memory  VmRSS 121,416 kB, 24 threads
    battery 99%, 35.0 C (from 100% / 34.0 C after ~4 min of capture)
    errors  0 FATAL / 0 CAMERA_DISABLED / 0 AudioRecord failures / 0 service issues

**Permission revocation** (Stage 4 requirement): revoking `CAMERA` mid-capture produced

    ActivityManager: Killing 8707:org.homeguardian.harness/u0a101 (adj 200): permissions revoked

Android killed the process — its documented behaviour — the camera was released (`Active Camera
Clients: []`), and START_STICKY restarted the service with a null intent, which deliberately arms
nothing. The permission was restored afterwards as test hygiene.

**Clean shutdown:** explicit stop, then `pidof` empty, camera clients `[]`, microphone not in use.
`screen_off_timeout` restored to 120000 ms. Device left off USB power, identity verified
(`ELUGA_I7_EE` / API 28). No footage or audio was recorded, stored or transmitted at any point.

## 7. Honest scope — what is NOT proven

- **SoC suspend survival: NOT PROVEN.** The SoC did not suspend while capturing (§4).
- **30-minute screen-off duration: NOT ATTEMPTED** (CAM-07's gate). Measured 150 s.
- **Microphone screen-off: NOT separately asserted.** Mic was exercised screen-on.
- **Audio encoding: NOT DONE.** Raw PCM only; no AAC/Opus.
- **Hub delivery: NOT DONE** (that is CAM-08). No network path is exercised here.
- **H.264 decode compatibility: STILL NOT TESTED.**
- **Talkback/AEC: NOT IMPLEMENTED** (CAM-09).

## 8. Roadmap

- **CAM-17 re-scoped and marked DONE** per operator decision: acceptance is now "measured
  encoded-frame delivery from at least one camera, with clean resource release". The historical
  Camera1 failure is retained in the row and in `docs/HG_CAM_08_CAPTURE_ACCEPTANCE.md`.
- **CAM-16 stays IN PROGRESS.** Its four conditions are now measured, but the row's spirit is not
  finished: no audio codec, screen-off short of the duration gate, mic screen-off unasserted, no
  hub delivery.
- **CAM-07 advanced to PARTIAL (advanced)** with the real screen-off evidence and the
  non-suspend caveat.

## 9. Correction to earlier claims

HG-CAM-08B stated that "Camera1 could never have been fixed by changing the texture" and that
option 1 "could not work in principle". That was too broad and is corrected here, as the brief
requires:

- **Camera1 is NOT universally incapable of producing frames.** Android supports preview callbacks
  and Camera1 can deliver frames through `onPreviewFrame` given a consumer that is actually
  drained. The failure documented in this project belongs to **the particular implementation used**
  — an undrained `new SurfaceTexture(0)` — not to the Camera1 API as a whole.
- What **is** structural, and is the reason a different API was needed for this design: Camera1's
  `setPreviewTexture` accepts only a `SurfaceTexture`, never a `Surface`, so Camera1 cannot target
  a MediaCodec input surface directly. That is a limitation of the encoder-sink goal, not of
  Camera1's ability to deliver frames at all.
