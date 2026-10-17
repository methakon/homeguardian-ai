# HG-CAM-08 — Real Camera and Microphone Acquisition: Result

Branch: `experiment/hg-cam-eluga-cctv`. Continued from `8f854a6cb2bfbbfb9351d973329dad91b478ca40`.
Device: Panasonic ELUGA I7 EE, serial `LRW86SC6RO8TZ99H`, MT6739, arm64-v8a, Android 9 / API 28.

**Headline: Stage 1 (camera acquisition) is BLOCKED. The camera opens and negotiates correctly,
but ZERO frames are ever delivered to the preview callback, measured across three configurations.
Stages 2–5 were therefore not attempted. No media capture has been demonstrated on this device.**

## What was built for this milestone

The capture implementation already existed (`Camera1Bridge` + the native delivery gate) but had
**never been exercised on hardware** — H-03 listed physical Camera1 capture as BLOCKED/awaiting
approval. So this milestone is its first real test, not a re-run.

Added: attended launcher actions so the operator or a scripted test can drive the *same* code
paths the UI buttons use, without tapping the screen and without weakening any gate:

    ...CONSENT_GRANT / ...CONSENT_WITHDRAW
    ...CAMERA_START / ...CAMERA_STOP
    ...MIC_START / ...MIC_STOP

Consent, OS permission, and the native delivery gate are all still enforced — the actions call
the identical methods the buttons call. No gate was bypassed to make testing convenient.

Runtime permissions were granted with `pm grant` (the documented mechanism). The app keeps its
ordinary runtime-request path for a human operator; the on-screen dialog needs an unlocked
device, which is not available for a remote test.

## Measured result

Every run reaches the same point and then delivers nothing:

    18:21:57  consent recorded (camera=yes, mic=yes)
    18:22:00  camera capture started (id=0 backend=camera1)
    18:22:00  camera preview size: target=320x240 chosen=320x240 from 6 supported
              (bounds min=16 max=307200 pixels)
    18:22:00  camera1: capturing 320x240 @ 15fps (id=0, delivery-gated)
    18:22:20  camera stopped: frames=0 dropped=0        <-- THE MEASUREMENT

    camera availability: legacy(Camera1)=2
    ro.camera.api1.max_preview_size = 960x540

Three configurations were tried, all with frames=0:

| # | configuration | observation |
|---|---|---|
| 1 | dummy `new SurfaceTexture(0)` (the original code) | camera connects, vendor HAL reports buffers released unused, client closes ~1 s later, **frames=0** |
| 2 | configuration 1 with the screen held awake (`stayon`, 600 s timeout) | identical — so this was **not** an `onPause` artefact, **frames=0** |
| 3 | no preview surface at all (`startPreview()` with no texture) | camera reports capturing, still **frames=0** |

### The vendor-level evidence

    MtkCam/streambuf: [markUserStatus] s0:d0:App:YV12:0|HW_TEXTURE:0:0x7e3c23e580
                      released w/o using -> ERROR buffer - user status:0x8:0x8
    MtkCam/pipeline:  [getBufferLockedImp] [requestNo:0 frameNo:0 streamId:0] mNonReleasedNum==0
    Camera2ClientBase: Closed Camera 0. Client was: org.homeguardian.harness

The MediaTek pipeline configured a **HW_TEXTURE** stream (despite `setPreviewFormat(NV21)`) and
released its buffers unused, with zero non-released buffers available. No `onPreviewFrame` fired,
and — importantly — **the delivery gate never denied anything**: the bridge's "delivery gate denied
a frame; stopping capture" path did not appear in any run. So the failure is below our code, at
the vendor HAL/legacy-shim boundary, not in the consent or accounting logic.

### Immediate diagnosis

This device exposes only two cameras and its legacy Camera1 shim appears to run over a camera2/
MTK "device@3.4" HAL (`mtkcam-dev3: open camera3 device (device@3.4/internal/0)`). That stack
expects a real consumer surface. A callback-only preview — which is exactly what a headless
capture node wants and what the H-03 design assumed — does not appear to be supported here.

**This is a genuine engineering blocker, reported rather than worked around.** I did not attempt to
defeat any privacy control, and no rooting or OS modification was considered.

## Concrete next steps (not attempted here)

1. **Attach a real consumer**: a `SurfaceView`/`TextureView` or a `SurfaceTexture` backed by a
   live GL context, rather than a detached texture. This is the most likely fix and is what the
   MTK stack is asking for.
2. **Or use the camera2 NDK path** (`NdkCameraDevice`) with an `ImageReader`-style target. The
   device is HAL3-capable, so this is viable; it was simply never enumerated successfully before.
3. **Or use MediaCodec as the sink** — configure an encoder input surface directly as the preview
   target, which both satisfies the HAL's surface requirement and is the shape Stage 3 needs
   anyway. This is probably the strongest option: it turns the blocker into the next milestone.

## Stages not reached

Because Stage 1's acceptance gate ("at least one supported camera produces actual frames") was
not met, the remaining stages were **not attempted**, and are recorded as blocked rather than
skipped:

| stage | status |
|---|---|
| 1 — camera acquisition | **BLOCKED** — camera opens, 0 frames delivered |
| 2 — microphone acquisition | NOT ATTEMPTED (depends on Stage 1 gate) |
| 3 — H.264 encoding | NOT ATTEMPTED (no frames to encode) |
| 4 — service lifecycle / consent | HG-CAM-07 fixes retained and still verified |
| 5 — screen-off operation | NOT ATTEMPTED; additionally cannot be tested until frames exist |

## Device state and hygiene

- App installed, camera and microphone runtime permissions granted for the test, service stopped.
- The service/activity was left stopped; no capture is running.
- Display settings changed for the test were restored: `screen_off_timeout` 120000 ms, `stayon`
  returned to its previous state. Both were verified restored in the run output.
- Permissions can be revoked with `pm revoke` on request; nothing was left capturing.
- No footage was recorded, stored or transmitted — no frame was ever produced.
- No factory reset, unlock, rooting, flashing, or shared-storage deletion. Mi A3, HG-SEC-04 and
  the Oracle mail VPS untouched.

## Capability classification

| capability | classification |
|---|---|
| Attended action harness for capture control | **VERIFIED** (drives the real code paths) |
| Consent recording | **VERIFIED** (`consent recorded (camera=yes, mic=yes)`) |
| Runtime permissions granted | **VERIFIED** (`CAMERA: granted=true`, `RECORD_AUDIO: granted=true`) |
| Camera open + preview negotiation | **VERIFIED** (id=0, 320x240 @ 15fps, backend camera1) |
| **Camera frame delivery** | **BLOCKED — 0 frames, measured three ways** |
| Microphone samples | **NOT TESTED** |
| H.264 encode | **NOT TESTED** |
| Screen-off media | **NOT TESTED** |
| Network integration | **NOT TESTED** |

A successful `startPreview()` and a "capturing" status string are **not** frame delivery. This
document does not claim any capture capability.
