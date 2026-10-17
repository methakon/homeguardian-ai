# HG-CAM-06 — Panasonic Cleanup: Result, Evidence and One Discrepancy

Branch: `experiment/hg-cam-eluga-cctv`. Device: Panasonic ELUGA I7, serial `<DEVICE_SERIAL>`.

Executed against the real device after USB reconnection. Every stage re-verified identity
immediately before acting; the device was never left ambiguous, and the Mi A3 was never
touched even though it was simultaneously visible over wireless ADB.

## Stage A — identity: VERIFIED

    serial       <DEVICE_SERIAL>   (usb:2-3, transport_id 1532)
    lsusb        0e8d:201c MediaTek Inc. ELUGA_I7_EE
    manufacturer PANASONIC
    model        ELUGA_I7_EE
    board        k39tv1_64_a27
    abi          arm64-v8a
    android      9 (API 28)

Two phones were attached for the whole session (the Panasonic on USB, the Mi A3 over
Wi-Fi at `<LAN_IP_MI_A3>:40815`). Every command was pinned with `-s <DEVICE_SERIAL>`.

## Stage B — account status: A DISCREPANCY, REPORTED NOT HIDDEN

The Google account **is gone — verified absent**: it was 1 of 4 accounts before removal and
is now 0.

**But all three non-Google accounts are gone too.** `dumpsys account` now reports
`Accounts: 0` with no `Account {name=...}` entries at all:

    type                       before   after
    com.google                    1        0
    com.facebook.auth.login       1        0     <- was to be PRESERVED
    com.facebook.messenger        1        0     <- was to be PRESERVED
    com.whatsapp                  1        0     <- was to be PRESERVED
    TOTAL                         4        0

HG-CAM-04 instructed "do not remove other application or mail accounts", and HG-CAM-06
Stage B asked specifically to confirm those three survived. They did not.

**Recorded as a discrepancy between the owner's report and the device. No attempt was made
to restore, re-add or bypass anything**, per Stage B. A plausible mechanism (not claimed as
fact, since it cannot be confirmed after the event): WhatsApp, Facebook and Messenger all
offer Google sign-in, so removing the Google account can cascade to accounts created through
it. The owner may equally have removed them deliberately. This is recorded as an open
question rather than explained away.

Operational impact: none for a dedicated CCTV node — those accounts were not needed. The
cost is limited to app-private data, which was already authorised as permanently lost.

## Stage C — inventory: COMPLETED

Six third-party packages remained at inventory time (22 had already been removed before this
session reached the device — package count was 168 at backup time and 146 at Stage C):

    com.eterno           15.4 MB   Dailyhunt
    com.facebook.katana  62.2 MB   Facebook
    com.facebook.orca    55.8 MB   Messenger
    com.jio.myjio        80.9 MB   MyJio
    com.whatsapp         53.0 MB   WhatsApp
    in.startv.hotstar    22.6 MB   Disney+ Hotstar

No SIM was present (`gsm.operator.alpha` empty), so MyJio carried no carrier-provisioning
role and was not load-bearing. APK size was recorded for information only and was **not** the
reason for any removal.

## Stage D — removal: 6 of 6, each independently verified

Removed one at a time through `scripts/panasonic_pkg_remove.sh`, which re-verifies identity
before every package, refuses anything that is not a third-party package, and records the
exit status, resulting package state, `/data` delta and service health per package:

| package | result | exit | installed after | /data freed | services |
|---|---|---|---|---|---|
| `com.whatsapp` | Success | 0 | 0 | 110,800 KB | PASS |
| `com.facebook.katana` | Success | 0 | 0 | 345,968 KB | PASS |
| `com.facebook.orca` | Success | 0 | 0 | 339,484 KB | PASS |
| `com.eterno` | Success | 0 | 0 | 62,692 KB | PASS |
| `in.startv.hotstar` | Success | 0 | 0 | 143,880 KB | PASS |
| `com.jio.myjio` | Success | 0 | 0 | 196,084 KB | PASS |
| **total** | **6/6** | | | **1,198,908 KB (~1.14 GiB)** | |

No wildcard, no list-driven batch, no generic debloating. Raw evidence:
`manifest/package_removal_evidence.txt` in the backup tree (outside version control).

## Stage E — final health check: PASS

| check | result |
|---|---|
| responsive, identity unchanged | PASS — API 28, `k39tv1_64_a27`, arm64-v8a |
| no reboot during cleanup | PASS — `boot_count=168` unchanged, uptime continuous |
| ADB | PASS — transport `device` |
| Wi-Fi | PASS — `wifi_on=1`, "Wi-Fi is enabled", IP `<LAN_IP>` |
| camera frameworks | PASS — `media.camera` present; MTK `camera.ccap` / `camera.lomoeffect` HALs up |
| audio frameworks | PASS — `media.audio_flinger`, `media.audio_policy` present |
| video codecs | PASS — `media.omx@1.0::IOmx` and `IOmxStore` up; `OMX.MTK.VIDEO.ENCODER.AVC` still present; 16 MediaCodec entries in the vendor XML |
| DRM / crypto | PASS — `drm@1.0/1.1` clearkey + widevine, `cas@1.0` |
| keystore / security | PASS — `keymaster@3.0::IKeymasterDevice` up; keystore process running |
| package installer | PASS — `package` service present, packageinstaller present |
| shared-storage backup intact | PASS — `DCIM` **490 files, exactly the pre-cleanup baseline**; `WhatsApp` 429 M, `PhoneRecord` 24 M, all top-level dirs present |
| **no capture initiated** | confirmed — no camera or microphone was opened at any point |

**A probe correction worth recording.** `service check media.codec` and
`service check keystore` both report `not found` on this device. That is **not** a failure:
on Android 9 both are HIDL/HAL-backed, so `service check` is the wrong instrument and would
have produced a false FAIL. Verified correctly through `lshal`, `dumpsys media.player` and
the vendor codec XML, all of which confirm they are functional.

**A second measurement trap, confirmed rather than assumed.** `du` on `/sdcard` is
unreliable over FUSE here: it reports `DCIM` as 106 MB when the directory actually holds
2.83 GB. That reading is *unchanged* from before the cleanup, and an independent count of
`find /sdcard/DCIM -type f` returns **490 — exactly the baseline** — so no shared storage was
lost. Reading the `du` figure alone would have suggested the photos had been deleted.

## Storage outcome

    /data used     8.5 GB (90 %)  ->  6.6 GB (71 %)
    /data available  975 MB       ->  2.7 GB
    third-party packages   6      ->  0
    accounts               4      ->  0

Part of that gain (~700 MB) came from the 22 packages removed before this session reached
the device; 1.14 GiB is attributable to the six removals recorded above.

## Verified versus owner-reported

| item | status |
|---|---|
| Device identity, services, codecs, storage, health | **VERIFIED on hardware** |
| Six package removals with exit status and `/data` deltas | **VERIFIED** |
| Shared storage intact (490-file DCIM count) | **VERIFIED** |
| Google account removed | **VERIFIED absent** |
| Three non-Google accounts intact | **FAILED — all four accounts are gone (§Stage B)** |
| Camera/microphone operation | **NOT TESTED** — no capture initiated |
| Screen-off capture survival | **NOT TESTED** — still the gate for the CCTV workstream |

## Remaining risks

| item | status |
|---|---|
| Non-Google accounts unexpectedly gone | open; no action taken; no data recoverable either way |
| Screen-off capture survival | untested — the gate for CCTV |
| SoC temperature unreadable without root | permanent; only battery temp available, rising 34 → 37 °C while on USB at 100 % |
| Backup archive and key share one disk | open — needs the owner's off-machine second copy |
| Battery at 100 % on USB | attended, short operations only |

Nothing destructive was performed beyond the authorised package removals: no factory reset,
no bootloader unlock, no flashing, no repartitioning, no shared-storage deletion, no capture.
The Mi A3, HG-SEC-04 and the Oracle mail VPS were untouched.
