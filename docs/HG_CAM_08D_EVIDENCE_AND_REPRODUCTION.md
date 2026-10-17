# HG-CAM-08D — Evidence summary, reproduction steps, and sanitization record

Companion to `docs/HG_CAM_08D_SUSTAINED_ACCEPTANCE.md`. This file exists so a reviewer can
reproduce and re-verify the claims without trusting the prose in that document.

- **Branch:** `experiment/hg-cam-08d-evidence`
- **Base:** `cad758f` (fetchable from `origin/feature/h03-android-wifi-node`)
- **Device class:** Panasonic ELUGA I7 EE, MediaTek MT6739, arm64-v8a, Android 9 / API 28
- **No main merge.** No production deployment. No public media endpoint.

## 1. Sanitization record

Every device/user identifier was replaced before publication. Substitutions applied to the
changed and new files only:

| real value | placeholder |
|---|---|
| device serial | `<DEVICE_SERIAL>` |
| device Wi-Fi MAC | `<DEVICE_MAC>` |
| development host LAN address | `<LAN_IP>` |
| second test device (unrelated) LAN address | `<LAN_IP_MI_A3>` |
| local build path prefix | `<HOME>` |

**Scope note.** Three files in this tree — `docs/RELEASE_CANDIDATE_V21.md`, `docs/STATUS.md`,
`docs/FRICTION_LOG.md` — contain a local path. They are **inherited from the base commit and are
not part of this milestone's diff**; they were already published on the base branch. They are out
of scope here and are flagged rather than silently edited.

**Consequence for reproduction.** The `scripts/panasonic_*.sh` tools carry a load-bearing identity
guard whose expected serial is now a placeholder. They will refuse to run until you substitute your
own device's serial. That is the intended behaviour, and it is a behaviour change relative to the
locally tested bytes: the guard logic is unchanged, only the literal was redacted. The published
scripts are therefore *not* byte-identical to the tested artifacts. Every other claim in this
document rests on the raw log in `docs/evidence/`, which was captured from the tested build.

**Not published:** no household audio, no video frames, no keystore, no bearer token, no account
data. No frame or sample was ever written to disk during any test.

## 2. What was tested, and the exact evidence

### 2.1 The 30-minute screen-off window

Raw per-sample log: `docs/evidence/hg_cam_08d_stage2_30min.log` (30 samples, `t=0s` .. `t=1756s`).
Sampler: `scripts/hg_cam_08d_stage2_screenoff.sh`.

Preconditions, recorded in the log header:

    power: ACpowered:false USBpowered:false        <- off charger, so suspend was possible
    wakefulness=mWakefulness=Asleep
    screen=mScreenState=OFF
    isForeground=isForeground=true

Window endpoints (video and microphone, screen off, display locked):

| point | encoded H.264 buffers | keyframes | microphone samples | level | temp |
|---|---|---|---|---|---|
| `t=0s` | 1,018 | 68 | 1,006,080 | 95 | 34.0 C |
| `t=1756s` (last in-window sample) | 53,077 | 3,539 | 85,424,640 | 90 | 35.0 C |
| end-of-window readout | 54,857 | 3,658 | 88,312,320 | 90 | 34.0 C |

Note the deliberate distinction: the in-window samples stop at `t=1756s` because the loop's next
iteration would exceed 1800 s; the end-of-window readout is taken afterwards and is slightly
higher. Both are reported rather than presenting one as the other.

### 2.2 Per-interval continuity — the dropped-frame question

Derived from the same log by differencing consecutive samples:

    intervals measured : 29
    video fps          : min 29.2   max 32.1
    microphone         : min 47,276 Hz  max 51,242 Hz
    stalled intervals  : 0

Every interval delivered roughly 1,780 buffers and 2.88M samples. The fps spread is sampling
jitter, not instability: consecutive samples are 60-61 s apart, so a 61 s interval necessarily
yields proportionally more buffers. A stall, a camera restart or a codec hiccup would appear as a
*low* interval; none is present.

Load and resource behaviour across the window:

    CPU      28.3-29.5% of ONE core, steady
    VmRSS    116,748 - 125,780 kB (stable across 30 min -> no leak)
    thermal  33.0 - 35.0 C, never rising
    battery  95% -> 90%  (5% for 30 minutes of concurrent video + audio)

Restart accounting, which is the strongest single line in the run:

    arming_video=1  arming_mic=1  camera_reopens=1  disarms=0
    errors: fatal=0  camera_denied=0  aac_fail=0  probe_fail=0

The camera was opened **once** and ran for the full window without a reopen, a release or a
restart.

### 2.3 Idle release after the window

    camera_clients = []
    idle CPU       = 0.10% of one core (2 ticks over 20 s)
    idle VmRSS     = 121,076 kB
    battery        = 90%, health GOOD, 34.0 C

## 3. The failed measurement script, preserved deliberately

`scripts/hg_cam_08d_stage2_screenoff_FAILED.sh` is the **first** version, kept as its own artifact
(the corrected one is `scripts/hg_cam_08d_stage2_screenoff.sh`; the two differ).

Failure, as measured:

    t=0s pid=11248 isForeground=true rss=117932kB cpu=?% up=5918.44 batt=97 320 \
         encodedBuffers=1018 keyFrames=68 samples=1006080 fatal=0
    /tmp/hgcam8d_stage2.sh: line 56: T: unbound variable

Two defects, both documented in the header of the failed file:

1. **Unbound variable (fatal).** The CPU branch tested `$T`, but the assigned variable was
   `PREV_T`. Iteration 1 short-circuited at `[ -n "$PREV_CPU" ]` and never evaluated `$T`, so it
   printed a good sample; iteration 2 evaluated it and `set -u` aborted the script.
2. **Swapped battery parse.** Positional parsing of `level:`/`temperature:` returned the
   temperature where the level was expected, so the two safety-abort thresholds were comparing the
   wrong numbers. It did not fire here, but a genuine low-battery condition would not have aborted.

**Why this matters to the verdict.** A sampler that prints one sample looks exactly like a working
sampler. The corrected run is therefore not accepted until it has emitted **both `t=0` and `t=60`**.
Both are present in the published log.

## 4. Permission withdrawal — process death, not a graceful stop

    pm revoke <pkg> android.permission.CAMERA        (both sensors live)
    ActivityManager: Killing 13392:org.homeguardian.harness/u0a101 (adj 200): permissions revoked
    after: camera clients []
           new pid 13535 (START_STICKY restart)
           restart camera client: none -> a null-intent restart arms nothing

This is the check the issue asks to be verified as **process death rather than a graceful
microphone stop**, and the evidence supports that reading in a specific way: the microphone logged
**0** `stopped and released` lines. Had the calm path run, that line would be present. The absence
of our own teardown logging is precisely what distinguishes a kill from a disarm.

**Method correction, kept here on purpose.** The first check of "did the restart re-arm the
camera" counted `arming capture` log lines and read `1` — but that one line was the *setup* arm,
not the restart. `Active Camera Clients: []` after the restart is the authoritative evidence. A
count that includes the setup action is not a test of the restart.

## 5. Codec proofs

### 5.1 H.264 encoder -> decoder round-trip

    encoded=285 buffers / 2,380,712 bytes   key=19
    DECODED=283 frames
    csd=SPS/PPS-present
    enc=video/avc 640x480      decOut=none (see limitation)
    COMPATIBLE=true

The decoder was configured from the **encoder's own** output format (CSD-0/CSD-1 = SPS/PPS) and
run with a **null surface**, so frames were decoded in memory and never rendered, written or
transmitted. The 2-buffer shortfall is expected, not a defect: the decoder is created on
`INFO_OUTPUT_FORMAT_CHANGED`, so the earliest buffers are consumed to trigger it. Reporting this as
a gap rather than rounding to a 1:1 match.

**Limitation:** the decoder's negotiated output format string was not captured (`decOut=none`), so
decodability is proven by frame count, not by a recorded output colour format.

### 5.2 Audio: hardware Opus is absent, software Opus is NOT blocked

Read from the device:

    audio ENCODERS present : OMX.google.aac.encoder (audio/mp4a-latm), AMR-NB, AMR-WB
    Opus                   : OMX.google.opus.decoder exists -- DECODER only, no encoder
    G.711                  : alaw/mlaw are DECODER entries only

WebRTC's mandatory audio codecs are Opus and G.711 PCMU/PCMA, so the device cannot natively produce
what WebRTC prefers. The working encoder path was measured, not assumed:

    mime=audio/mp4a-latm(AAC-LC) 64kbps
    pcmInFrames=710,400  encodedFrames=694  encodedBytes=118,394
    ~21.3 ms/frame, timestamps advancing

**Software Opus feasibility — tested, not assumed.** The absence of a hardware/vendor Opus encoder
does not prevent software encoding, and this was verified by actually cross-compiling it:

    source      : opus-1.4.tar.gz (1,063,408 bytes, sha256 c9b32b4253be5ae6...)
    toolchain   : the project's own NDK r26d, android.toolchain.cmake
    target      : ANDROID_ABI=arm64-v8a, ANDROID_PLATFORM=android-28
    result      : libopus.so  ->  ELF 64-bit LSB shared object, ARM aarch64
    size        : 1,779,456 bytes (~1.7 MB)
    exports     : opus_encode, opus_encoder_create, opus_encoder_ctl,
                  opus_repacketizer_create, opus_repacketizer_cat  (7 opus_encode* symbols)

So a software Opus encoder is available to this device. **Scope of this proof:** it is a BUILD and
symbol proof — correct architecture, correct API level, real encoder entry points from the official
upstream source. It is **not** an on-device encode proof: no Opus frame has been produced on the
handset yet. Cost is ~1.7 MB added to the APK plus CPU.

**Not claimed:** that Opus is integrated. The APK still uses AAC-LC. The recommended next step is
to wire libopus behind the same "separately armed microphone" control and re-measure encoded frames
and CPU the way AAC-LC was measured.

## 6. Reproduction steps

Everything below runs over the development transport; substitute your own device serial.

1. **Build the APK** (arm64-v8a, API 28):

       HG_ABI=arm64-v8a HG_API=28 bash apk/build_apk.sh

2. **Install and grant the runtime permissions** for the attended test:

       adb -s <DEVICE_SERIAL> install -r <built>.apk
       adb -s <DEVICE_SERIAL> shell pm grant org.homeguardian.harness android.permission.CAMERA
       adb -s <DEVICE_SERIAL> shell pm grant org.homeguardian.harness android.permission.RECORD_AUDIO

3. **Verify identity before acting.** The scripts refuse on mismatch; if you run commands by hand,
   confirm model / board / ABI / SDK first. Never fall back to another attached device.

4. **Camera-only, screen on** (expect a non-zero buffer count and a stable fps):

       adb -s <DEVICE_SERIAL> shell am start -n org.homeguardian.harness/.MainActivity \
           -a org.homeguardian.harness.CAM2_START
       # read the service's own counters:
       adb -s <DEVICE_SERIAL> logcat -s HGCctv

5. **Microphone-only**; confirm the camera is NOT opened, which is the separate-control check:

       adb -s <DEVICE_SERIAL> shell am start -n org.homeguardian.harness/.MainActivity \
           -a org.homeguardian.harness.MIC2_START

6. **H.264 round-trip:** action `org.homeguardian.harness.DECODE_CHECK`, then read tag `HGDecode`.
   Expect `DECODED` > 0 and `COMPATIBLE=true`.

7. **MediaSink self-test:** action `org.homeguardian.harness.SINK_SELFTEST`, then read tag `HGSink`.
   Expect 7/7 PASS with `accepted=1 rejected=2`. If it reports SKIPPED-INVALID, the sink never
   bound — that is a FAIL, not a pass.

8. **The 30-minute window:** `bash scripts/hg_cam_08d_stage2_screenoff.sh`. Before trusting it,
   confirm the log shows **both** `t=0s` and `t=60s`.

9. **Screen-off precondition check:** `mWakefulness=Asleep` AND `mScreenState=OFF` AND
   `dumpsys battery` showing AC/USB/wireless all `false`. Without all three, a screen-off run says
   nothing about screen-off operation.

## 7. Verification summary — verified / prototype / blocked

| claim | status |
|---|---|
| Service-owned concurrent camera + microphone, screen off, 30 min continuous | **VERIFIED** |
| Independent arming and independent release of the two sensors | **VERIFIED** |
| Camera opened once, zero restarts, zero errors across the window | **VERIFIED** |
| Resource behaviour (CPU / RSS / thermal / battery) | **VERIFIED** |
| Idle release after disarm | **VERIFIED** |
| H.264 stream is decodable (encoder→decoder round-trip) | **VERIFIED** |
| Permission revocation kills the process and the restart arms nothing | **VERIFIED** |
| AAC-LC encoding of real microphone PCM | **VERIFIED** |
| Software Opus buildable for arm64/API 28 with this NDK | **VERIFIED (build only)** |
| Opus actually encoding on-device | **NOT DONE** — prototype |
| MediaSink loopback, token-gated, fail-closed | **VERIFIED** |
| Authenticated cross-network media delivery | **NOT DONE** — no hub transport exists |
| A/V multiplexing and lip-sync | **NOT DONE** — two independent streams |
| Two-way talkback + echo cancellation | **NOT DONE** (CAM-09) |
| Real Alexa Smart Home camera integration | **NOT DONE** — see the integration contract |
| SoC suspend survival | **NOT PROVEN** — capture keeps the SoC awake |

## 8. Limitations a reviewer should weigh

- **One window, one configuration.** A single 30-minute run at 640x480 / 48 kHz mono. Not repeated,
  not at other resolutions.
- **No suspend evidence.** `/proc/uptime` advanced with wall clock throughout: an active capture
  session keeps the SoC awake. This is expressly not the acceptance condition, but nothing in this
  evidence should be read as deep-sleep survival.
- **`MediaSink` is loopback and token-gated only.** It is **not** authenticated cross-network
  streaming and **not** Alexa device integration. No TLS, no hub, no discovery.
- **The published scripts are not byte-identical to the tested ones** (identity-guard literal
  redacted). Logic unchanged; see §1.
- **No A/V sync.** Video and audio timestamps are independent; no container, no muxing.
- **Decoder output format not recorded** (§5.1).
