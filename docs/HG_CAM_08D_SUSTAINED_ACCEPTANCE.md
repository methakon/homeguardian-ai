# HG-CAM-08D — Sustained screen-off video + audio, codec and delivery: RESULT

**Device:** Panasonic ELUGA I7 EE (`<DEVICE_SERIAL>`), Android 9 / API 28, arm64-v8a
**Transport:** Wi-Fi ADB `<LAN_IP>:5555` (USB-free)
**Base commit:** `748d11c26c83a0baa0d13a71097c052f6ff5e98a`
**Branch:** `experiment/hg-cam-eluga-cctv`

## 1. Headline

| gate | result |
|---|---|
| Concurrent screen-off video + audio | **PASS** |
| 30-minute battery-powered screen-off acceptance | **PASS** — zero stalls, zero restarts |
| H.264 encoder→decoder compatibility | **PASS** — 283 frames decoded from 285 encoded buffers |
| Audio encoding path | **PASS with a caveat** — AAC-LC works; no Opus/G.711 ENCODER exists |
| Local delivery interface (fail-closed, loopback-only) | **PASS** — 7/7 security cases |

## 2. Stage 1 — concurrent screen-off operation

Armed video and microphone independently through the authorized activity, verified both with the
screen on, then turned the display off and locked.

Cross-interference: **none.** `arming capture` = 1, `arming mic` = 1, `camera OPENED` = 1 — arming
the microphone did not reopen or disturb the camera, and vice versa.

With `mWakefulness=Asleep` / `mScreenState=OFF`, `isForeground=true`, notification present:

    tick #33  video encodedBuffers=4873  fps=29.61   mic readCalls=1914 samples=7349760  AUDIO
    tick #36  video encodedBuffers=5318  fps=29.61   mic readCalls=2102 samples=8071680  AUDIO

Independent release (the part that actually proves separate ownership):

    disarm VIDEO only  -> camera clients [], mic STILL running (readCalls 2478 -> 2603, samples climbing)
    disarm MIC only    -> "mic stopped and released", final 2,661 readCalls / 10,216,320 samples / 212,923 ms

### Permission withdrawal while BOTH sensors are live

    pm revoke ... CAMERA
    ActivityManager: Killing 13392:org.homeguardian.harness/u0a101 (adj 200): permissions revoked
    after: camera clients []          new pid 13535 (START_STICKY restart)
           restart camera client     none -> a null-intent restart arms nothing (fail-safe)

Android kills the process when a runtime permission is revoked, so **the resources are released by
process death, not by our stop path** — the microphone logged 0 "stopped and released" lines for
exactly that reason. That is expected behaviour and not a leak, but it is worth stating plainly
rather than counting release lines and claiming the disarm path ran.

Note on method: the first check used the count of `arming capture` log lines as a proxy for "did the
restart re-arm the camera", and it read 1 — which was my *own* initial arm, not a restart. The
authoritative evidence is `Active Camera Clients: []` after the restart. A count that includes the
setup action is not a test of the restart.

## 3. Stage 2 — the 30-minute acceptance

Window 20:33:08 → 21:03:08 IST (1800 s), **off USB power** (`AC/USB/wireless` all false), display
off and locked for the entire window.

    VIDEO   1,018 -> 54,857 encoded H.264 buffers (+53,839)   keyframes 68 -> 3,658
    MIC     1,006,080 -> 88,312,320 samples
    SERVICE isForeground=true throughout; arming_video=1 arming_mic=1 camera_reopens=1 disarms=0
    ERRORS  fatal=0  camera_denied=0  aac_fail=0  probe_fail=0

**Continuity — the dropped-frame question, answered per interval** (29 sampled intervals):

    interval fps: min 29.2 / max 32.1        interval audio: 47,276 - 51,242 Hz
    ZERO stalled intervals; every interval delivered ~1,780 buffers and ~2.88M samples

The fps spread is sampling jitter, not instability: consecutive samples are 60-61 s apart, so a
61 s interval yields proportionally more buffers. No interval is low, which is what a stall,
a camera restart or a codec hiccup would look like.

`camera_reopens=1` with `disarms=0` is the strongest single line: the camera opened **once** and ran
continuously for 30 minutes without ever being reopened, released or restarted.

Resources across the window:

    CPU      steady 28.3-29.5% of ONE core (0.2 core for concurrent 640x480 H.264 + 48 kHz PCM)
    MEMORY   VmRSS 116,748 - 125,780 kB, stable across 30 min -> no leak
    THERMAL  33.0 - 35.0 C throughout, never rising
    BATTERY  95% -> 90%  (5% for 30 minutes of concurrent video + audio)

No safety abort triggered (thresholds were battery < 15% and temperature > 45 C).

After the window: screen on → `mWakefulness=Awake`, still capturing (55,005 buffers), and the
disarm action still worked.

**Idle release after disarming both:**

    camera_clients = []
    idle CPU  0.10% of one core (2 ticks over 20 s)
    idle RSS  121,076 kB      battery 90%, health GOOD, 34.0 C

## 4. Stage 3 — codec compatibility

### H.264 encoder→decoder round-trip: COMPATIBLE

    h264roundtrip STARTED encoded=285B/2380712bytes key=19 DECODED=283
                   csd=SPS/PPS-present enc=video/avc 640x480 elapsedMs=10000
    COMPATIBLE=true

285 encoded buffers → **283 decoded frames**. The decoder was configured from the encoder's own
output format (CSD-0/CSD-1 = SPS/PPS) and ran with a **null surface**, so frames were decoded in
memory and never rendered, written or transmitted.

The 2-buffer shortfall is expected and not a defect: the decoder is created on
`INFO_OUTPUT_FORMAT_CHANGED`, so buffers emitted before that point are consumed to trigger it. This
is reported rather than rounded to 1:1.

### Audio encoding path: AAC works, but the WebRTC codec is missing

Read from the device, not assumed:

    audio ENCODERS present: OMX.google.aac.encoder (audio/mp4a-latm), AMR-NB, AMR-WB
    NO Opus encoder  -- OMX.google.opus.decoder exists, DECODER only
    NO G.711 encoder -- g711 alaw/mlaw are DECODER entries only

This matters because **WebRTC's mandatory audio codecs are Opus and G.711 PCMU/PCMA**. This device
cannot natively produce the codec WebRTC prefers. Confirmed by driving real microphone PCM through
the encoder that *does* exist:

    voice-encode mime=audio/mp4a-latm(AAC-LC) 64kbps
      pcmInFrames=710400  encodedFrames=694  encodedBytes=118394
      aac#1 bytes=2  aac#3 bytes=240 ptsUs=21333  aac#50 bytes=154 ptsUs=1023999

710,400 PCM frames → **694 AAC-LC frames**, 118,394 bytes ≈ 64 kbps, ~21.3 ms per frame,
timestamps advancing. So raw PCM is demonstrably not the delivery format, and a real encoder
output exists — but it is AAC, not Opus.

Paths for the eventual bidirectional media architecture, in preference order:

1. **Bundle a software Opus encoder** (libopus). This is the normal approach for a phone-to-browser
   path and is the only option that produces WebRTC's mandatory codec natively here.
2. **AAC here + transcode at a gateway.** Works today with the existing encoder; costs CPU in
   HomeGuardian and adds latency.
3. **G.711 PCMU in software.** Trivial companding, ~64 kbps narrowband, and it *is* a WebRTC
   mandatory codec, so it interoperates without negotiation tricks. Lowest quality, lowest cost.

Note `OMX.google.aac.encoder` is a software codec, so its cost is already inside the 28.7% CPU
figure above.

## 5. Stage 4 — local delivery interface

`apk/src/org/homeguardian/harness/MediaSink.java` — a small, testable boundary, deliberately **not**
a streamer. Encoded frames only, length-prefixed (`[4B pts][4B flags][4B size][payload]`), nothing
decoded, transcoded or rendered.

Self-test result — 7/7 PASS, and provably non-vacuous (`accepted=1 rejected=2 frames=1 bytes=64`):

    1 no-token-refuses-start   PASS   absent token REFUSES TO START the sink (fail-closed)
    2 starts-with-token        PASS   binds 127.0.0.1:35967
    3 loopback-only            PASS   127.0.0.1 explicitly, never 0.0.0.0
    4 no-token-client-rejected PASS   "handshake: bad token length 0" -> client REJECTED
    5 wrong-token-rejected     PASS   reply=1 -> REFUSED (correct)
    6 right-token-accepted     PASS   reply=0, "client ACCEPTED (token verified)"
    7 frame-delivered          PASS   record size=64

The test credential is deleted at the end of the run. The interface does **not** replace or bypass
the independent physical-device authentication gate, and there is no TLS, reverse proxy, Cloudflare,
VPS routing or public exposure anywhere in this change. Remote dashboard video and talkback remain
subsequent milestones and are not claimed.

### Two platform constraints found while building this

- **`INTERNET` is required to bind any socket, including a loopback-only one.** Without it,
  `bind()` fails with `SocketException: socket failed: EACCES (Permission denied)` — not a port or
  firewall problem. The permission has been added with its scope documented, and **flagged for
  operator review** because it widens this app's declared surface: it had been deliberately
  network-free.
- **`NetworkOnMainThreadException`.** `onStartCommand` runs on the main thread, so the self-test's
  client probes could not connect from there. The accept loop was unaffected (already on its own
  thread), which made this initially look like a binding fault. The test now runs off the main
  thread.

### Three defects found in my own test harness, not the sink

Worth recording because each one would have produced a false claim:

1. **Vacuous passes.** Cases 4 and 5 initially reported PASS while the server had never bound —
   the probe simply failed to connect and "no connection" was scored as "correctly refused". Fixed
   with a precondition gate (non-zero bound port, else SKIPPED-INVALID) and by **never swallowing
   the connect exception**.
2. **The probe closed before reading the record**, making the server's write fail with
   `Connection reset` and failing case 7 for a reason that had nothing to do with the sink.
3. **The 30-minute script died after its first sample**: it referenced `$T` while having declared
   `PREV_T`, and `set -u` aborted on the second iteration. The first attempt's failure is why the
   loop now prints `t=0` *and* `t=60` before the run is trusted.

## 6. Honest scope — what is NOT proven

- **SoC suspend survival: still NOT PROVEN.** `/proc/uptime` advanced with wall clock throughout;
  an active capture session keeps the SoC awake. Per HG-CAM-08D this is explicitly **not** the
  acceptance condition — but it remains untested, and nothing here should be read as deep-sleep
  evidence.
- **Remote/networked media delivery: NOT DONE.** The sink delivers to a **loopback** client only.
  No hub, no TLS, no dashboard.
- **Two-way audio / talkback / AEC: NOT IMPLEMENTED** (CAM-09).
- **Opus: NOT AVAILABLE** without bundling a software encoder. AAC-LC only.
- **Decoder output format string: not captured** — the round-trip counts decoded frames and proves
  decodability, but the decoder's negotiated output colour format was not recorded.
- **Video/audio are not muxed or synchronized.** They are two independent streams with independent
  timestamps; no container, no A/V sync was attempted.
- **Sustained test ran once.** One 30-minute window at 640x480/48 kHz; other resolutions, and
  repeated or longer runs, are unmeasured.

## 7. Roadmap

- **CAM-16 remains IN PROGRESS**, as instructed. What is now measured: service-owned concurrent
  video + audio, screen-off continuity over 30 minutes, an encoder→decoder round-trip and a real
  audio encoder path. What still blocks closure: no hub/media-path delivery (loopback interface
  only), no Opus, no A/V muxing or sync, no talkback.
- **CAM-17 stays DONE** under its approved revised criteria (measured encoded-frame delivery with
  clean resource release) — unchanged and re-confirmed by this milestone.
- **CAM-07's >= 30-minute screen-off gate is now MET** on duration and continuity; its remaining
  caveat is unchanged (the SoC does not suspend while capturing, so suspend survival is untested).

## 8. Evidence and hygiene

- Raw 30-minute sampler log: `devices/backup/deviceprofile/panasonic-eluga-i7/manifest/hg_cam_08d_stage2_30min.txt`
  (gitignored, chmod 600) — per-sample CPU, RSS, battery, temperature, and media counters.
- No footage or audio was recorded, stored or transmitted at any point. Encoded buffers were
  released undecoded, decoded frames were never rendered, and PCM was counted and dropped.
- Device left clean: app stopped, `Active Camera Clients: []`, `screen_off_timeout` restored to
  120000 ms, off USB power, 90% battery, health GOOD, 33.0 C, identity verified.
- No changes to Mi A3, Oracle, Cloudflare, V380 devices, the security branch or mail infrastructure.
