# Installation gate — Mi A3 native Linux

Gate report for TASKs A–D. Nothing was flashed, unlocked, erased or formatted. All device
interaction was read-only (`getprop`, `dumpsys`, `ls`).

---

## TASK A — USB/fastboot reliability: **BLOCKED**

### What the owner's port change fixed

Moving the cable from the front panel to the back panel changed the device topology from
`usb:2-8` to `usb:2-1`, and the transport became usable:

```
before  : 2-8   error -71, repeated enumeration, adb could not claim it
after   : 2-1   2717:ff08  product="Mi A3"  serial=edd53dbd04de  speed=480
                interface 1.0 class=ff subclass=42 (ADB)
```

A **stale adb server** was also masking it: it had cached the old bus state and reported only
the Panasonic. After `adb kill-server && adb start-server`, both devices appear:

```
LRW86SC6RO8TZ99H   device usb:2-2   model:ELUGA_I7_EE    transport_id:2
edd53dbd04de       device usb:2-1   model:Mi_A3           transport_id:7
```

So two causes, two fixes: **back-panel port** (physical) and **adb server restart** (stale cache).
Worth recording because a stale server looks identical to a dead cable.

### Measurement — 297 s window, sampled every 5 s

```
samples          : 59
state=device     : 59   (100%)
state=GONE       : 0
transport_ids    : 1 -> 4 -> 5 -> 7
```

### The decisive detail

Availability was 100% — but the **transport ID changed three times**:

```
t=0..98s     transport 1    stable 98s
t=103s       1 -> 4         re-enumeration
t=108s       4 -> 5         re-enumeration (5s later)
t=174s       5 -> 7         re-enumeration
t=174..297s  transport 7    stable 123s
```

Each recovery completed inside one 5 s sample, so a state-only check reports a clean 100% and
**misses the instability entirely**. Had I trusted the up-time figure, I would have reported a
PASS on a transport that re-enumerates roughly once every 100 s.

### Verdict: **BLOCKED** for flashing

- **For long-running adb work:** fine. Recovery is fast and the device stays usable — this is
  materially better than the `error -71` behaviour previously recorded.
- **For flashing:** **not acceptable.** A flash is a single uninterruptible operation, and
  fastboot writing across a re-enumeration is exactly the failure mode that bricks a device.
  Three re-enumerations per five minutes is far outside any safe margin.

### Tooling

```
adb      1.0.41        /usr/bin/adb            (on PATH)
fastboot 37.0.1        ~/Android/Sdk/platform-tools/fastboot   (NOT on PATH — call by full path)
```

`fastboot` must be invoked by absolute path; a bare `fastboot` fails, which would be
misdiagnosed as "no fastboot installed".

### Cabling discipline for any future attempt

Known-good **data** cable (not charge-only), **directly into a rear I/O port**, no hub, no
front-panel header, no adapter. The front-panel path is the one that produced `error -71`.

---

## TASK B — stock Android 11 recovery package

### Exact device and firmware (read from the device)

```
model          Mi A3                 device      laurel_sprout
build          RKQ1.200903.002       version     V12.0.26.0.RFQMIXM
release        11  (SDK 30)          security    2022-08-01
fingerprint    Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys
variant        Global (RFQMIXM)
stock state    ro.secure=1  ro.adb.secure=1  ro.build.type=user  ro.build.tags=release-keys
               (unrooted, unmodified)
```

### Matching stock package — verified present on Xiaomi's own servers

```
file            laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
url             https://bigota.d.miui.com/V12.0.26.0.RFQMIXM/<file>
mirror          https://cdnorg.d.miui.com/V12.0.26.0.RFQMIXM/<file>
http            200
Content-Length  2439506308      (2.44 GB)
Content-Type    application/x-gtar-compressed
Last-Modified   Tue, 08 Nov 2022 03:15:41 GMT
md5 (published) 2292e2c0b1c0e9839d5b14427154b5b4
```

Build number `V12.0.26.0.RFQMIXM` **matches the installed fingerprint exactly** — this is the
correct restoration package for the exact firmware on the device, not a neighbouring release.

Recovery (as opposed to fastboot) package, same build:
`miui_LAURELSPROUTGlobal_V12.0.26.0.RFQMIXM_88b2e6c4fb_11.0.zip`, md5
`88b2e6c4fba84155121e4eb684c623ba`.

**Not yet downloaded.** Verification so far is of availability, not of file integrity — the
checksum must be confirmed against the downloaded artifact before any flash, and I have not
downloaded 2.44 GB or claimed its hash.

### Recovery procedure — no Android 9 downgrade

This restores **Android 11**, so it does not involve the Android 9 downgrade that bricks
Android 11 devices. That was the specific hazard to avoid, and this path avoids it.

### Preserved artifacts (verified this session)

```
user-file backup   941 entries, 941 re-hashed, 0 mismatches  (unchanged)
production APK     2d6018e0afa4c7250b621efcb4194dc3d964c4895e18cc043738644d03a47246
MCP engine         82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
```

---

## TASK C — image readiness

Device-specific kernel and boot configuration are already resolved and committed
(`reports/07-native-linux-readiness.md`):

| Item | Status |
|---|---|
| Kernel | `linux-nura-qcom-sm61x5` 7.2.3, close-to-mainline, pinned sha512 |
| Device tree | `qcom/sm6125-xiaomi-laurel-sprout` |
| **Sustained-read fix** | patch series reserving firmware-owned DRAM — **must** be present; without it the device hard-locks roughly once per 59 MiB of buffered read, and our MCP engine is exactly that workload |
| Wi-Fi firmware path | `ATH10K_SNOC` (WCN3990) + `QCOM_Q6V5_WCSS`; blobs come from the device's own `${vendor}` partition |
| Charging | `CHARGER_QCOM_SMB2`, `REGULATOR_QCOM_SPI` |
| Thermal | `QCOM_TSENS`, demonstrated on this board (15 zones, 37.5–52.2 °C vs 85 °C trip) |
| MCP service | packaged (`homeguardian-mcp`) + systemd unit with a 600-mode credential gate, loopback-pinned |
| Optional | microphone, speaker, cameras, touchscreen — all excluded from the critical path |

---

## TASK D — installation gate

### 1. USB/fastboot reliability: **BLOCKED**

100% adb availability, but 3 transport re-enumerations in 297 s. Not flash-safe.

### 2. Stock recovery package

**Identified and verified as available**, build-exact match, on Xiaomi's official servers,
with published md5 and `Content-Length`. **Not downloaded**; integrity unconfirmed until it is.

### 3. Proposed Linux image and hashes

The **built** artifact that exists and is hash-verified:

```
hg_mcp_server (Linux ARM64/glibc)
series  2,702,376 bytes
sha256  82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
NEEDED  libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
```

The **OS image has not been built** and therefore has no hash. `native-linux/build_native_image.sh`
is prepared and reviewed but deliberately unrun. I will not present a hash for an image that
does not exist.

### 4. Device and firmware compatibility: **verified**

Exact model, codename, bootloader state and firmware build all read from the device and
cross-checked against the stock package. Kernel and device tree match the board.

### 5. Essential hardware readiness

Kernel-side: storage, Wi-Fi, charging, thermal — all enabled and researched. Thermal is
additionally demonstrated on this board by the patch author. Nothing on the critical path is
missing. Display/touch/audio/camera remain deliberately optional.

### 6. Remaining risks

1. **Transport instability** — the blocking one. Untouched by any OS choice.
2. **`sys.oem_unlock_allowed = 0`** — see below. A hard gate.
3. **Bootloader unlock wipes the device.** AOSP is explicit: unlock "performs a factory data
   reset". Any custom OS therefore means **losing all on-device data**, which the 941-file
   backup mitigates but does not eliminate (it is user files only — it cannot restore apps,
   settings or accounts).
4. **Xiaomi unlock policy.** Xiaomi normally requires the Mi Unlock Tool with a Mi Account
   bound for 7+ days and a 7–30 day wait. The Mi A3 is an Android One device and may permit
   standard `fastboot flashing unlock`, but **I have not verified which applies** and will not
   guess.
5. **Kernel revision** must carry the sustained-read fix.
6. **Build reproducibility gap** — the pmaports build fetches spdlog/nlohmann_json at configure
   time, so an image build needs network and a writable tree.

### 7. Blocking owner action — one toggle

```
sys.oem_unlock_allowed = 0        <- OEM unlocking is OFF
ro.oem_unlock_supported = 1       <- but the device supports it
ro.boot.flash.locked = 1          <- currently locked
```

Per AOSP, the bootloader **refuses** `fastboot flashing unlock` unless `get_unlock_ability`
is 1. Until the owner enables **Settings → System → Developer options → OEM unlocking** on the
phone, unlocking cannot be attempted at all — regardless of USB quality.

### 8. Proposed sequence requiring explicit authorisation

Not authorised, not started. Each step needs your approval, and step 1 alone is a
point of no return for on-device data.

```
0. Preconditions (ALL must hold)
   - transport re-measured with a known-good cable in a rear port, no hub,
     and ZERO re-enumerations over >= 30 minutes
   - stock ROM downloaded and its sha256 recorded and confirmed against the
     published md5 -> this is the rollback artifact
   - user-file backup re-verified (941/941)
   - owner enables OEM unlocking on the device
   - owner accepts that unlock FACTORY RESETS the device

1. Unlock bootloader            <- WIPES THE DEVICE. Irreversible in effect.
   fastboot flashing unlock  (owner confirms on-device with the volume/power keys)

2. Read-only state capture
   fastboot getvar all   -> record every variable for rollback comparison

3. Pre-flash sanity
   fastboot getvar product / current-slot / unlocked / battery-voltage

4. Flash the prepared image (boot / vbmeta / dtbo as required by the device layout)

5. First boot + run native-linux/acceptance/first_boot_acceptance.sh

ROLLBACK: re-flash the verified stock package with the full fastboot ROM.
```

**I am not requesting authorisation to proceed yet.** The gate is not satisfied: the transport
is demonstrably unstable, and the rollback artifact is identified but not downloaded or
checksum-verified. Authorising a flash now would mean flashing without a proven way back —
which is precisely what this gate exists to prevent.

---

## TASK E — post-install acceptance

**Not performed, not performable.** Nothing has been installed. No device measurement of any
kind exists: no boot, no Wi-Fi association, no SSH, no battery or thermal reading, no systemd
status, no MCP call on real hardware.

Every MCP figure reported in this project so far is a **container or workstation** result.
Under your standing instruction I am not presenting any of them as phone measurements. Task E
begins only after an authorised installation, using
`native-linux/acceptance/first_boot_acceptance.sh`.

---

## Untouched, as instructed

No unlock, erase, flash, partition change or downgrade. Stock Android 11 intact and running.
Hackathon release branch not touched. Oracle, mail, V380 and the Panasonic ELUGA not touched.
The 941-file backup and the rollback artifacts are preserved and re-verified.
