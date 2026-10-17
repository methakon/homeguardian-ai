# HG-CAM-08 — Real Camera and Microphone Acquisition: Result

Branch: `experiment/hg-cam-eluga-cctv`. Started from `8f854a6cb2bfbbfb9351d973329dad91b478ca40`;
camera blocker resolved in HG-CAM-08B from `6db8fa08785f7bfda9ca18179a9fc072ae960c40`.
Device: Panasonic ELUGA I7 EE, serial `<DEVICE_SERIAL>`, MT6739, arm64-v8a, Android 9 / API 28.

**Headline (updated by HG-CAM-08B): Stage 1 camera acquisition is SOLVED — real H.264 frames are
delivered from the physical camera and measured (350/344/345/344 encoded buffers at 29.69 fps).
Stages 2 (microphone), the H.264 decode-compatibility check and screen-off operation are NOT yet
done; screen-off specifically requires the capture to move into the foreground service (§7).**

Sections 1–6 below are the ORIGINAL HG-CAM-08 blocker report, kept verbatim as the record of how
the fault was localised. Section 7 is the HG-CAM-08B resolution.

---

## 1. What was built for this milestone

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

## 2. Measured result — the zero-frame blocker

Every run reached the same point and then delivered nothing:

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

## 3. The vendor-level evidence

    MtkCam/streambuf: [markUserStatus] s0:d0:App:YV12:0|HW_TEXTURE:0:0x7e3c23e580
                      released w/o using -> ERROR buffer - user status:0x8:0x8
    MtkCam/pipeline:  [getBufferLockedImp] [requestNo:0 frameNo:0 streamId:0] mNonReleasedNum==0
    Camera2ClientBase: Closed Camera 0. Client was: org.homeguardian.harness

The MediaTek pipeline configured a **HW_TEXTURE** stream (despite `setPreviewFormat(NV21)`) and
released its buffers unused, with zero non-released buffers available. No `onPreviewFrame` fired,
and — importantly — **the delivery gate never denied anything**: the bridge's "delivery gate denied
a frame; stopping capture" path did not appear in any run. So the failure is below our code, at
the vendor HAL/legacy-shim boundary, not in the consent or accounting logic.

## 4. Immediate diagnosis (as written at the time)

This device exposes only two cameras and its legacy Camera1 shim appears to run over a camera2/
MTK "device@3.4" HAL (`mtkcam-dev3: open camera3 device (device@3.4/internal/0)`). That stack
expects a real consumer surface. A callback-only preview — which is exactly what a headless
capture node wants and what the H-03 design assumed — does not appear to be supported here.

**This is a genuine engineering blocker, reported rather than worked around.** I did not attempt to
defeat any privacy control, and no rooting or OS modification was considered.

## 5. Concrete next steps (as proposed at the time)

1. **Attach a real consumer**: a `SurfaceView`/`TextureView` or a `SurfaceTexture` backed by a
   live GL context, rather than a detached texture. This is the most likely fix and is what the
   MTK stack is asking for.
2. **Or use the camera2 NDK path** (`NdkCameraDevice`) with an `ImageReader`-style target. The
   device is HAL3-capable, so this is viable; it was simply never enumerated successfully before.
3. **Or use MediaCodec as the sink** — configure an encoder input surface directly as the preview
   target, which both satisfies the HAL's surface requirement and is the shape Stage 3 needs
   anyway. This is probably the strongest option: it turns the blocker into the next milestone.

**Option 3 is the one that worked** (see §7). Option 1 was rejected for this design: Camera1's
`setPreviewTexture` accepts only a `SurfaceTexture`, never a `Surface`, so Camera1 cannot target an
encoder sink. **Correction (HG-CAM-08C):** that is a limit of the encoder-sink goal, NOT a claim
that Camera1 cannot produce frames at all — Android supports preview callbacks and Camera1 can
deliver frames via `onPreviewFrame` when the consumer is actually drained. The failure documented
here belongs to the particular undrained-`SurfaceTexture` implementation, not to the Camera1 API.
Option 2 was unnecessary: the Java Camera2 API enumerates both cameras on this device.

## 6. Stages as reported at the time

| stage | status then |
|---|---|
| 1 — camera acquisition | **BLOCKED** — camera opens, 0 frames delivered |
| 2 — microphone acquisition | NOT ATTEMPTED (depends on Stage 1 gate) |
| 3 — H.264 encoding | NOT ATTEMPTED (no frames to encode) |
| 4 — service lifecycle / consent | HG-CAM-07 fixes retained and still verified |
| 5 — screen-off operation | NOT ATTEMPTED; additionally cannot be tested until frames exist |

---

# 7. HG-CAM-08B — RESOLUTION

## 7.1 Result

Real H.264 frames are now delivered from the physical camera, encoded by the vendor hardware
encoder, and measured four times over Wi-Fi ADB with USB verified absent.

| run | encoded buffers | key frames | measured fps | bytes | window |
|---|---|---|---|---|---|
| 1 | **350** | 24 | **29.69** | 2,905,718 | 12,000 ms |
| 2 | **344** | 23 | **29.69** | 2,853,343 | 12,000 ms |
| 3 | **345** | 23 | **29.69** | 2,863,518 | 12,000 ms |
| 4 | **344** | 23 | **29.69** | 2,858,370 | 12,001 ms |

Output format `video/avc w=640 h=480`; `presentationTimeUs` advanced 11.55–11.76 s across each
12 s window — genuine per-frame timestamps, not a repeated constant.

The Stage 1 acceptance gate ("at least one supported camera produces actual frames in an attended,
bounded test and releases resources cleanly") is **MET**.

## 7.2 Why the old path delivered zero frames — confirmed

The H-03 design attached a bare `new SurfaceTexture(0)` purely to satisfy `setPreviewTexture` and
never called `updateTexImage()`. `released w/o using` together with `mNonReleasedNum==0` is the
signature of a buffer queue nobody consumes: the producer runs out of free buffers and stalls.
The gate-denial absence from §3 is what proves the fault was below our code.

**Scope of this finding (corrected in HG-CAM-08C):** it indicts THAT undrained-`SurfaceTexture`
implementation, not Camera1 in general. Camera1 preview callbacks are a supported Android
mechanism and do deliver frames when the consumer is drained.

## 7.3 The fix

`apk/src/org/homeguardian/harness/Camera2EncoderProbe.java` — Camera2 → MediaCodec input surface,
driven only through the exported `MainActivity` (`CAM2_START` / `CAM2_STOP`).

A MediaCodec input surface is consumed by the codec itself, so no app-side drain exists and the
stall cannot occur — and the output shape is what a CCTV node needs anyway. Buffers are released
with `releaseOutputBuffer(idx, false)`: never rendered, never written to disk, never transmitted.

Enumeration and capability, read from the device rather than assumed:

    camera2 ids=[0, 1]
    camera 0 hardwareLevel=LIMITED
    encoder-compatible sizes (13): 3264x2448 2560x1920 1920x1440 1920x1088 1920x1080 1280x720
                                   960x720 960x540 720x480 640x480 352x288 320x240 176x144

Sizing uses `SCALER_STREAM_CONFIGURATION_MAP.getOutputSizes(MediaCodec.class)` so the camera size
is one the encoder actually accepts, capped at 1280x720 to bound encode work on this 4x Cortex-A53
part. 640x480 was selected. `OMX.MTK.VIDEO.ENCODER.AVC` accepts 128x96..1920x1080.

**Camera2 is available here even though the NDK camera2 API enumerates zero cameras on the
MT6580 clone.** `dumpsys media.camera` shows `API2 camera characteristics` and
`android.scaler.availableStreamConfigurations` (188 entries for the back camera); the Java
Camera2 API enumerates both cameras fine — option 2 of §5 was never needed.

Independent confirmation that real frames moved through the vendor pipeline:

    MtkCam/P1NodeUtility: [inspect] [Cam::0] <AcqBuf+++> StreamNum(5)
        StreamId(4294967298) FrameNum(376) RequestNum(376)

## 7.4 Resources and lifecycle

During active capture (8.65 s sample window):

    CPU        2.68 s CPU / 8.65 s wall = 31.0% of ONE core
    memory     VmRSS 127,992 kB (~125 MB), VmSize 3.99 GB virtual, 22 threads, native heap 9,820 kB
    battery    100%, health GOOD, 34.0 C
    errors     0 FATAL / 0 CameraAccessException / 0 codec errors

Camera open/close pairs, proving the camera is released and never left open:

    19:11:16 CONNECT  ->  19:11:28 DISCONNECT
    19:11:33 CONNECT  ->  19:11:45 DISCONNECT

### Defect found and fixed during this milestone

The bounded run never cleared its `running` flag when the window elapsed, so the **next** `start()`
was silently refused as `already running` — the same silent-refusal class as the `singleTop` defect
from HG-CAM-07. It was invisible until two runs were attempted back to back.

Fixed by auto-releasing the camera when the window completes. Re-verified with **no stop issued
between runs**: run B followed run A directly, produced 344 buffers, and logged **0** `already
running` refusals.

## 7.5 Next blocker — screen-off capture requires the service, not the activity

Android 9 restricts camera access to the **foreground**. With the screen off the activity is
paused and `openCamera` fails with a real, specific error:

    android.hardware.camera2.CameraAccessException: CAMERA_DISABLED (1):
    validateClientPermissionsLocked:921: Caller "org.homeguardian.harness"
    (PID 10101, UID 5708) cannot open camera "0" from background

The identical code succeeds the moment the device is awake with the activity resumed. Consequences:

- Every measurement above is a **screen-on** result and claims nothing about screen-off capture.
- Screen-off capture must run in the **foreground service** (`CctvService`) — which HG-CAM-07 also
  concluded independently, for a different reason.
- A `mWakefulness=Awake` / `mResumedActivity` check is a mandatory precondition for any capture
  test; a locked device refuses the camera and must not be misread as a codec or camera fault.

## 7.6 Honest scope — what HG-CAM-08B does NOT prove

- **Screen-off capture: NOT TESTED.** Needs the capture moved into `CctvService` (§7.5).
- **Microphone acquisition: NOT TESTED.** Next milestone (Stage 2).
- **H.264 decode compatibility: NOT TESTED.** Output is `video/avc` with sane per-frame timestamps
  and key-frame flags, but no decoder round-trip has been run.
- **Sustained capture: NOT TESTED.** Only 12 s bounded windows; thermal behaviour beyond that on an
  aging device is unmeasured.
- **`presentationTimeUs` is informational** — measured fps is derived from its spacing, not from a
  wall clock, and it supports no latency claim.

## 7.7 Capability classification (updated)

| capability | classification |
|---|---|
| Attended action harness for capture control | **VERIFIED** (drives the real code paths) |
| Consent recording | **VERIFIED** (`consent recorded (camera=yes, mic=yes)`) |
| Runtime permissions granted | **VERIFIED** (`CAMERA: granted=true`, `RECORD_AUDIO: granted=true`) |
| Camera open + preview negotiation | **VERIFIED** (Camera2 `ids=[0,1]`, camera 0, LIMITED) |
| **Camera frame delivery (screen on)** | **VERIFIED — 350/344/345/344 encoded buffers @ 29.69 fps** |
| Hardware H.264 encode | **VERIFIED** (`OMX.MTK.VIDEO.ENCODER.AVC`, `video/avc 640x480`) |
| Camera resource release | **VERIFIED** (clean CONNECT/DISCONNECT pairs; restart deterministic) |
| **Camera frame delivery (screen off)** | **NOT TESTED — blocked on moving capture into the service** |
| Microphone samples | **NOT TESTED** |
| H.264 decode compatibility | **NOT TESTED** |
| Network integration | **NOT TESTED** |

A successful `startPreview()` and a "capturing" status string are **not** frame delivery. This
document claims capture only where the encoded-buffer counts above are cited, and only for the
screen-on, attended, bounded case.

## 7.8 Device state and hygiene

- App `org.homeguardian.harness` v0.2.0 installed over Wi-Fi ADB; camera and microphone runtime
  permissions granted for the test, then the probe stopped; no capture left running.
- Display settings changed for the test were restored (`screen_off_timeout` 120000 ms, `stayon`
  returned to its previous state), both verified restored in the run output.
- Permissions can be revoked with `pm revoke` on request.
- **No footage was recorded, stored or transmitted** — encoded buffers were released undecoded
  (`releaseOutputBuffer(idx, false)`); the evidence is counters, timestamps and sizes only.
- No factory reset, unlock, rooting, flashing or shared-storage deletion. Mi A3, HG-SEC-04 and the
  Oracle mail VPS untouched.

## 7.9 Next steps

1. Move camera capture into `CctvService` and re-test with the screen off, using media timestamps
   to distinguish "service alive" from "frames actually arriving".
2. Microphone acquisition (Stage 2): request permission visibly, measure real sample delivery,
   rate, channel count and format, keep it separately controllable from video.
3. Bounded H.264 decode-compatibility check on the encoded buffers.
