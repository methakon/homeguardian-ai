# Native Linux unblocking — status

Covers TASKs 1–4. Nothing was flashed, unlocked, erased, formatted or downgraded. All device
interaction was read-only. The user-files backup and rollback artifacts are preserved and
re-verified.

---

## 1. USB and fastboot stability measurements

### What the owner's cable move fixed

The back-panel port moved the device from `usb:2-8` (could not be claimed, `error -71`) to
`usb:2-1`, which enumerates cleanly with the ADB interface
(`2717:ff08`, `product="Mi A3"`, `serial=edd53dbd04de`).

### The decisive distinction TASK 1 asked for

The earlier measurement tracked adb's `transport_id`. **That is a server-assigned handle**, so
it changes for reasons other than the bus. A new probe therefore records four signals in
authority order:

| Signal | Meaning | Authority |
|---|---|---|
| `/sys/bus/usb/devices/2-1/devnum` | kernel's USB device number; changes **only** on genuine re-enumeration | **decisive** |
| kernel "new USB device number" log line | kernel announcing a new device | strong |
| adb server PID | if it changes, transport churn is explained by the server alone | strong |
| adb `transport_id` | server handle | weakest |

### Results

**Probe window (22 samples):** `usb_devnum` constant at **47**, adb server PID constant,
`transport` constant, `state=device` throughout, `kernel_newdev=0` throughout.
**A fully stable window — no bus re-enumeration.**

**Outside the probe window:** an adb invocation printed
`* daemon not running; starting now at tcp:5037`, and the server PID had changed from the one
the probe observed (`621863` → `697146`).

**Interpretation.** The adb server dies on its own. An adb-server restart reassigns transport
ids and reproduces the churn **without any bus event** — so the previously reported
"3 re-enumerations" were most likely server-side, not USB-side. That is precisely the
distinction TASK 1 required, and it changes the remedy entirely: restart discipline and server
stability, not cable or port electrical faults.

**Stated as indicative, not conclusive.** The probe window caught a stable period, so the
correlation was not observed directly. A longer run that catches an instability event while
sampling `devnum` in the same window is still required to close it.

### Earlier 297 s availability measurement (still valid)

```
samples 59 | state=device 59 | GONE 0 | up-time 100% | transport_id changes: 3
```

### Fastboot

`fastboot 37.0.1` is present at `~/Android/Sdk/platform-tools/fastboot` but **not on PATH** —
a bare `fastboot` fails and would be misdiagnosed as "not installed".

**Fastboot-mode stability is NOT demonstrated and cannot be from Android.** It requires
entering the bootloader, which is a state change needing authorisation. Per the standing rule,
ADB stability is not a substitute — so this row stays **unproven** regardless of how clean the
ADB numbers look.

### Verdict: **BLOCKED** for flashing

---

## 2. Verified recovery package status: **IDENTIFIED, NOT YET VERIFIED**

### Device (read from the device)

```
model Mi A3   codename laurel_sprout   Android 11 / SDK 30
build RKQ1.200903.002   version V12.0.26.0.RFQMIXM   region Global
security patch 2022-08-01
fingerprint Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys
stock      ro.secure=1  ro.adb.secure=1  ro.build.type=user  ro.build.tags=release-keys
```

### Package — official origin, build-exact match

```
file   laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
origin https://cdnorg.d.miui.com/V12.0.26.0.RFQMIXM/<file>      (Xiaomi CDN)
       https://bigota.d.miui.com/V12.0.26.0.RFQMIXM/<file>      (alias, 100x slower)
HTTP   200   Content-Length 2439506308   Accept-Ranges: bytes
md5 (published) 2292e2c0b1c0e9839d5b14427154b5b4
```

Build `V12.0.26.0.RFQMIXM` equals the device's own fingerprint — correct package, not a
neighbouring release. Region Global matches.

**Download-source finding:** `bigota` served **33 KB/s**; `cdnorg` served **3.43 MB/s**. Same
file, same vendor, 100× difference. A naive `bigota` download would take ~20 hours and looks
like a hang.

### Status: NOT verified

The download is in progress and **has not completed**. Therefore:

- file completeness: **unconfirmed**
- expected vs actual checksum: **unconfirmed** — I will not report a hash I have not computed
- archive integrity: **unconfirmed**

The published md5 is from Xiaomi's release channel and is the value to compare against once the
file lands.

### Partition layout and anti-rollback

**A/B (seamless update) device.** Confirmed from the device-tree/packaging evidence and from
`fastboot getvar` output recorded by other owners of this model:

```
product:laurel_sprout   kernel:uefi   slot-count:2   has-slot:boot:yes
has-slot:system:yes     has-slot:modem:yes
max-download-size:804261888
vbmeta present (A/B)    dtbo present (AVB2)
```

Consequences for any restore:

- Must be **slot-aware**. Flashing `boot` to the wrong slot silently boots the other one.
- **vbmeta must be handled**; AVB rejects a modified boot chain.
- **Anti-rollback is enforced.** Reported by the device as `ro.boot.vbmeta.rollbackindex`, and
  by fastboot as `rollback-index`. Flashing a boot chain with a *lower* index is refused by the
  bootloader regardless of vbmeta settings. This is the mechanism behind the well-documented
  rule that these devices cannot be rolled back from Android 11 to Android 9.
- Use **"clean all"**, never **"clean all and lock"**. Relocking over a mismatched boot chain is
  a documented brick path.
- `fastboot` must be **≥ 31.0.2**; older releases dropped legacy A/B support and try
  `boot__a`/`boot__b`. Ours is 37.0.1, which is fine.

### Documented brick scenario — read this before authorising anything

A documented Mi A3 case exists where the device ended up **stuck in fastboot with a locked
bootloader and unbootable slots**, and could not recover because
`fastboot flashing get_unlock_ability` returned **0** — so `fastboot flashing unlock` was
refused, no slot change was permitted, and no image could be flashed.

Our device currently reports `sys.oem_unlock_allowed = 0`, the same precondition. The
difference is that ours still boots normally. **This is the concrete failure this gate exists
to prevent**, and it is why the transport and rollback gates must clear before anything else.

### Can the package actually restore this device? **Unproven**

Possessing an archive is not proven ability to restore. What is demonstrated: the package
matches this build and region, is on the vendor's own CDN, and is checksummed by the vendor.
What is not: that a local flash succeeds on this host over this transport, and that the result
boots. Both require the transport gate to clear first.

---

## 3. Bootloader readiness — owner interaction required

### Current state (read-only)

```
ro.boot.flash.locked        1        <- locked
ro.boot.verifiedbootstate   green
ro.boot.vbmeta.device_state locked
sys.oem_unlock_allowed      0        <- OEM unlocking is OFF
ro.oem_unlock_supported     1        <- device supports it
development_settings_enabled 1       (developer options already on)
adb_enabled                  1
```

### The toggle that must be flipped (owner, on the phone)

**Settings → System → Developer options → OEM unlocking → ON**

Per AOSP the bootloader **refuses** `fastboot flashing unlock` unless `get_unlock_ability` is 1.
Until this is on, unlocking cannot even be attempted — independent of USB quality.

### Device-specific unlock mechanism: **UNVERIFIED — sources conflict**

| Source | Claim |
|---|---|
| /e/OS install docs | Mi Unlock **app required**; Windows 7+; Mi Account with phone number; SIM installed; link device under "Mi Unlock status"; **may quote up to 30 days**; one device per account per 30 days |
| XDA guides (several) | **standard** `fastboot flashing unlock` then `fastboot flashing unlock_critical`; no Mi Unlock app |
| androidinfotech | "you don't have to contact Xiaomi for the Bootloader unlock process" |

**I am not resolving this by assertion.** The Mi A3 is an Android One device, which makes the
standard AOSP path plausible, but /e/OS — which maintains an install path for this exact device
— documents the Mi Unlock app. **Treat both the account requirement and any waiting period as
unverified for this device** until observed.

This is cheap to resolve read-only: `fastboot flashing get_unlock_ability` in bootloader mode
reports the value without changing anything. That must be the first real step, and it needs
bootloader-mode authorisation.

### Two unlock commands are required

`fastboot flashing unlock` unlocks standard partitions; `fastboot flashing unlock_critical`
unlocks the critical ones (vbmeta/bootloader-adjacent). Both are needed for a full OS
replacement — and **both wipe**, or the second still does.

### What unlocking costs

AOSP is explicit that the unlock transition **performs a factory data reset**. So replacing
Android means **losing all on-device data**. The 941-file backup covers user files only — it
cannot restore apps, settings, accounts or app data. That is the real cost and it should be
weighed as such.

Also recommended before unlocking: sign out of any Mi/Google account and remove screen locks,
or Factory Reset Protection will demand those credentials after the wipe.

### Not done, not requested

No toggle flipped, no unlock, no factory reset. Backup preserved and re-verified:
**941 entries, 0 mismatches**.

---

## 4. Native Linux image-build status: **PREPARED, NOT BUILT**

| Item | State |
|---|---|
| Kernel | `linux-nura-qcom-sm61x5` 7.2.3, close-to-mainline, sha512-pinned |
| Device tree | `qcom/sm6125-xiaomi-laurel-sprout` |
| **Sustained-read fix** | **must** be present — without it the device hard-locks ~once per 59 MiB of buffered read, which is exactly our workload |
| Wi-Fi | `ATH10K_SNOC` (WCN3990) + `QCOM_Q6V5_WCSS`; blobs from the device's own `${vendor}` partition |
| Charging | `CHARGER_QCOM_SMB2`, `REGULATOR_QCOM_SPI` |
| Thermal | `QCOM_TSENS` — demonstrated on this board (15 zones, 37.5–52.2 °C vs 85 °C trip) |
| MCP binary | the verified glibc ARM64 engine, packaged as `homeguardian-mcp` |
| Service | systemd unit, 600-mode credential gate, loopback-pinned |
| Admin | sshd policy: keys only, no root login, no password auth |
| Acceptance | `native-linux/acceptance/first_boot_acceptance.sh`, bounded, reuses the shared MCP checker |
| Build manifest | `native-linux/build_native_image.sh` + packaging tree |
| Optional | microphone, speaker, cameras, touchscreen — excluded from the critical path |

**The image is not built.** `build_native_image.sh` is prepared and reviewed but deliberately
unrun. It needs a pmbootstrap chroot, network, and it produces a flashable image.

## 5. Exact image checksum

**There is none. The image does not exist yet.**

What does exist and is hash-verified is the MCP binary that will be packaged into it:

```
hg_mcp_server  Linux ARM64 / glibc
size    2,702,376 bytes
sha256  82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
NEEDED  libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
```

I will not present a checksum for an image that has not been built.

## 6. Installation and rollback procedure

**Preconditions — all must hold before step 1:**

```
[ ] transport re-measured with a known-good data cable in a rear port, no hub,
    for >= 30 min with ZERO re-enumerations, WITH devnum sampled (not just adb state)
[ ] fastboot-mode stability independently demonstrated in the bootloader
[ ] stock package downloaded, size == 2439506308, md5 == published, gzip -t OK
[ ] extracted images inventoried and compared against the device's partition set
[ ] user-file backup re-verified (941/941)
[ ] owner has signed out of accounts and removed screen locks
[ ] owner has enabled OEM unlocking
[ ] owner accepts: unlock FACTORY RESETS the device
```

```
1. Unlock        fastboot flashing unlock            <- WIPES THE DEVICE
                 fastboot flashing unlock_critical   <- WIPES, unlocks critical parts
2. Capture       fastboot getvar all                 -> record every variable
                 fastboot getvar current-slot / rollback-index
                 fastboot oem device-info
3. Pre-flash     verify battery > 50%, cable in rear port, no hub
4. Flash         slot-aware, correct vbmeta handling for the prepared image
5. First boot    native-linux/acceptance/first_boot_acceptance.sh

ROLLBACK  re-flash the verified stock package, "clean all" (NEVER "clean all and lock"),
          slot A, then verify with fastboot getvar all against the step-2 capture.
```

**Do not reflash blindly on a bad boot.** Check the slot first; A/B devices commonly recover by
switching back to the previously-working slot — but only while both slots are intact.

## 7. GO / NO-GO: **NO-GO**

Four unmet gates, any one sufficient to stop:

1. **Transport not proven flash-safe.** ADB availability is good and the churn now looks
   server-side, but fastboot-mode stability has never been demonstrated, and ADB stability is
   explicitly not a substitute.
2. **Rollback artifact unverified.** Downloaded but not complete, not hashed, not integrity-
   tested. There is no proven way back.
3. **`sys.oem_unlock_allowed = 0`.** Unlock would be refused outright.
4. **Bootloader unlock mechanism unverified**, and unlocking costs a factory reset.

**The engineering is ready.** The blockers are transport proof, rollback proof, and an owner
toggle — not the architecture.

## 8. Exact pushed commit

```
81e18210650ed75507bb5c8c077988d1b08e2e3c
```

---

## Untouched, as instructed

No flash, unlock, erase, format or downgrade. Stock Android 11 running normally. PR #20
untouched and the hackathon release not delayed. Oracle, mail, V380 and the Panasonic ELUGA
untouched. The 941-file backup and rollback artifacts preserved and re-verified this session.
