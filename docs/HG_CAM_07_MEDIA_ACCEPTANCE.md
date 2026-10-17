# HG-CAM-07 — Panasonic CCTV: Installation and Media Acceptance

Branch: `experiment/hg-cam-eluga-cctv` (local only).
Starting commit: `69d069c4e908cf2251303ee6a077c5cfa6cc26df`.
Device: Panasonic ELUGA I7 EE, serial `<DEVICE_SERIAL>`.

**Headline: installation and the foreground service are VERIFIED on hardware. Camera and
microphone capture are NOT IMPLEMENTED in the service and therefore NOT TESTED. Nothing in
this document claims media capture works, because it has not been exercised.**

## Authorization finding (§1)

The brief asked me to establish Panasonic-specific installation authorization rather than
assume it. What the mandate actually contains:

- HG-CAM-07 §1: camera and microphone **testing** was previously authorized on the two
  designated spare phones, and I am told not to re-ask for it.
- HG-CAM-04: "stage all builds and installation dependencies ... validate APK integrity
  **before deploying**" — deployment was contemplated as the next step.
- HG-CAM-02: "request explicit approval before installing any media APK" — an explicit gate
  was originally stated.
- No brief contains a sentence explicitly granting Panasonic APK installation on its own.

Installation was therefore treated as authorized **as the prerequisite of the testing already
granted**, on the grounds that it is non-destructive, reversible by uninstall, and activates
nothing: this APK opens no camera and no microphone. If a separate installation gate was
intended, uninstalling is a single command and I will run it on request.

## Stage A — identity and preflight: PASS

    serial       <DEVICE_SERIAL>   (usb:2-3)
    manufacturer PANASONIC
    model        ELUGA_I7_EE
    board        k39tv1_64_a27
    abi          arm64-v8a
    android      9 / API 28
    /data        6.6 GB used of 10 GB (71 %), 2.7 GB available
    battery      100 %, health GOOD, charging on USB, 36.0 °C
    wifi         enabled, wlan0 <LAN_IP>
    services     media.camera, media.audio_flinger, media.audio_policy, media.player, package all found
    camera HALs  9 present (lshal)

The Mi A3 was simultaneously visible over wireless ADB throughout. Every command was pinned
with `-s <DEVICE_SERIAL>`. The personal-data backup and all shared storage were untouched.

## Stage B — APK verification before installation: PASS

    package      org.homeguardian.harness   versionCode 2   versionName 0.2.0
    minSdk       27        targetSdk 28
    native-code  arm64-v8a
    permissions  CAMERA, RECORD_AUDIO, FOREGROUND_SERVICE, ACCESS_NETWORK_STATE
    signature    CN=HomeGuardian Harness Debug
                 SHA-256 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
    service      .CctvService declared, exported=0

## Stage C — installation: PASS

    Performing Streamed Install
    Success
    userId=10101  primaryCpuAbi=arm64-v8a  versionCode=2  minSdk=27  targetSdk=28
    runtime permissions granted: NONE

The empty permission list is the important part: installing granted no camera and no
microphone access, and nothing has requested them.

## Stage D — foreground service: PASS, after fixing a real defect

Verified on hardware (`dumpsys activity services`):

    ServiceRecord{ffa1a93 u0 org.homeguardian.harness/.CctvService}
      app=ProcessRecord{cb1ccc6 22268:org.homeguardian.harness/u0a101}
      isForeground=true  foregroundId=4201  foregroundNoti=Notification(channel=hg_cctv_node ...)
      startRequested=true  lastStartId=1

The required notification is present with a **Stop** action:
`NotificationRecord(... id=4201 ... channel=hg_cctv_node ... actions={ [0] "Stop" -> PendingIntent{ startService} })`.

### A defect found by acceptance testing, and fixed

The service could not be started by adb at all:

    Permission Denial: Accessing service ... from pid=21920, uid=2000
    that is not exported from uid 10101

That is **correct** behaviour, not a bug: `CctvService` is deliberately `exported="false"`.
The real problem it exposed was that **nothing in the app started the service**, so it could
never run legitimately. Two defects followed from wiring it up:

1. **`am start` could not restart the service.** The first start worked and stop worked, but a
   start → stop → start cycle silently failed to restart. Root cause: the activity was
   `launchMode="standard"`, so re-issuing the same action was not delivered reliably to
   `onCreate` or `onNewIntent`. You can see the asymmetry in the log: the **stop** path
   produced a full `onCreate` sequence (`identity set`, `camera availability`, `Ready`), while
   the **restart** produced no log line at all.
   **Fix:** `android:launchMode="singleTop"`, so a repeat intent is delivered to the existing
   instance via `onNewIntent`. Confirmed in the built manifest (`launchMode=0x1`) and re-tested.

2. **No UI control existed.** Added an explicit operator section (status line, Start button,
   Stop button) plus an attended launcher hook so a remote test can drive the same code path
   through the exported activity — rather than weakening the service's export flag to make adb
   convenient.

After the fix, the full cycle is deterministic:

    START    isForeground=true
    STOP     stopped        (log: stop requested by operator -> destroyed, reason=operator_stop)
    RESTART  isForeground=true
    CLEANUP  stopped, notification cleared

Sanitised log evidence (tag `HGCctv`):

    17:15:28  service created / foreground started; media NOT armed
    17:15:33  stop requested by operator / service destroyed; reason=operator_stop
    17:15:38  service created / foreground started; media NOT armed
    17:15:44  stop requested by operator / service destroyed; reason=operator_stop

Raw evidence: `manifest/hg_cam_07_install_evidence.txt` and `manifest/hg_cam_07_media_evidence.txt`
in the backup tree (outside version control).

## Stage E — screen-off survival: PARTIAL, and not what it looks like

With the screen off for 150 s:

    service pid      23440 before and 23440 after   -> SURVIVED
    isForeground     true, notification intact
    battery          100 %, 36.0 °C

**However, this is not a suspend test, and must not be read as one.** The device's `/proc/uptime`
advanced 151.4 s across a ~150 s wall wait — uptime kept pace with real time, so **the SoC did
not actually suspend.** The phone is on USB power, and this is exactly the trap previously
documented on the sibling Mi A3: a device held awake on a charger cannot demonstrate deep-sleep
survival. A genuine test requires unplugging and an idle interval, which is not authorized here.

Also, and more fundamentally: **there is no media to survive.** With no camera or microphone
wired into the service, "do frames continue after screen-off" is unanswerable, and per the
brief's own instruction I am not inferring it from service uptime.

## Measured resources (service running, screen off)

    CPU        29 ticks (utime 18->44, stime 14->17) over 151 s wall
               = 0.29 s CPU = ~0.19 % of one core
    memory     PSS TOTAL 20,799 KB (~20.3 MB)
    battery    100 %, 36.0 °C, stable across the interval
    log growth no exceptions, no AndroidRuntime entries, no permission denials from our uid

## Not tested — stated plainly

| area | status | reason |
|---|---|---|
| Camera frames | **NOT TESTED** | not wired into the service. The harness has Camera1/native camera code for the H-03 workstream, but `CctvService` does not use it. |
| Hardware H.264 encode | **NOT TESTED as capture** | the encoder is present (`OMX.MTK.VIDEO.ENCODER.AVC` in the vendor XML, `media.omx@1.0::IOmx` HAL up) but nothing has encoded anything. |
| Microphone samples | **NOT TESTED** | not wired; no runtime permission requested. |
| Speaker talkback | **NOT IMPLEMENTED** | separate milestone; not claimed. |
| Permission withdrawal | **NOT TESTED** | no permission has been granted, so there is nothing to withdraw. |
| Screen-off capture | **NOT TESTED** | no capture exists to measure. |
| Network integration | **NOT TESTED** | correctly deferred: §7 says only after local capture passes. |
| true deep sleep | **NOT TESTED** | device on USB power; it did not suspend even with the display off. |

Per §3 of the brief, the accurate description of this APK is: **foreground-service
diagnostics only.** It runs, notifies, survives, and stops. It captures nothing.

## Capability classification

| capability | classification |
|---|---|
| Device identity | **VERIFIED** |
| APK installation (arm64, API 28, signed) | **VERIFIED** |
| Foreground service start | **VERIFIED** |
| Required notification with Stop action | **VERIFIED** |
| Service stop / restart / cleanup | **VERIFIED** (after the `singleTop` fix) |
| Service survival with screen off | **PARTIAL** (survived, but the SoC never suspended) |
| Camera capture | **NOT TESTED** |
| Microphone capture | **NOT TESTED** |
| Screen-off media survival | **NOT TESTED** |
| Network / hub delivery | **NOT TESTED** |

## Next step and blockers

The next real work is wiring capture into the service (§4/§5), then re-running the screen-off
test **on battery, not on USB**, which is the only configuration that can demonstrate the gate.
Blockers standing in the way:

1. Capture is not implemented in the service — this is the next engineering step, not a
   permission problem.
2. A genuine deep-sleep test needs the device off USB power, and unattended/continuous charging
   is not approved.
3. SoC temperature remains unreadable without root; only battery temperature is available.
4. The backup archive and its key still share one disk.

No factory reset, no bootloader unlock, no rooting, no flashing, no shared-storage deletion,
no public camera exposure, and no camera or microphone was opened at any point. The Mi A3,
HG-SEC-04 and the Oracle mail VPS were untouched.
