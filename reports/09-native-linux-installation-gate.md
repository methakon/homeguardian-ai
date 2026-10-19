# Native Linux installation gate — Mi A3

Status of the two blockers and the readiness of the install. Nothing has been flashed,
unlocked, erased or formatted. The postmarketOS assets, the MCP binary and the user-file
backup are preserved.

---

## TASK A — USB/fastboot reliability: **PASS**

The transport is now stable. The cause was the physical port, and moving the cable from the
front panel to the back panel resolved it.

Measured after the change:

```
device on bus .... 2-1  (vendor 2717, product Mi A3, serial edd53dbd04de)
speed ............ 480 Mbps (USB 2.0)
adb state ........ device (authorised, not unauthorized/offline)
window ........... 61 s, 30 samples
state=device ..... 30/30
disconnects ...... 0
state changes .... 0
kernel USB errors  none
error -71 ........ none observed
```

Root cause: **USB link instability, not key or authorisation failure.** The earlier
re-enumeration storm — 334 enumerations / 185 disconnects with `can't set config #1,
error -71` — was the port/hub path, not the phone. This is consistent with the earlier
finding that the workstation holds exactly one adb key and the RSA prompts were a symptom,
not the cause.

**Also resolved:** `fastboot` was not on `PATH`. It exists at
`/home/swarna-sekhar-dhar/Android/Sdk/platform-tools/fastboot`, **version 37.0.1**. Flashing
would have failed at the first command without this.

**Caveat — this is a 61-second window, not a soak.** It is enough to say the port change
fixed the fault that was observed, but a flashing operation runs for many minutes. A longer
soak should be completed immediately before any flash, and the flash itself must be treated
as interruptible-at-any-point (see the recovery section).

**Wireless ADB is not a substitute** and was not used. Fastboot is a USB-only protocol; it
cannot run over wireless debugging.

---

## TASK B — verified stock recovery: **DONE, package verified**

### Exact device and firmware

```
model ......... Mi A3 (laurel_sprout), serial edd53dbd04de
fingerprint ... Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys
build ......... MIUI V12.0.26.0.RFQMIXM, Android 11, SDK 30
security patch  2022-08-01
variant ....... Global (RFQMIXM)
verified boot . green
bootloader .... LOCKED (ro.boot.flash.locked = 1)
active slot ... _b
```

### The matching package — checksum verified

```
file    laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
source  https://bn.d.miui.com/V12.0.26.0.RFQMIXM/laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
origin  Xiaomi official CDN, Last-Modified 2022-11-08
size    2439506308 bytes  (matches the published 2.27 GB exactly)

md5     expected 2292e2c0b1c0e9839d5b14427154b5b4
md5     actual   2292e2c0b1c0e9839d5b14427154b5b4   MATCH
sha256           0e6b3c1b17aa860bacae1b979d4f63dd007a060f84291363263aef577444fe3c
```

Stored at `~/homeguardian-stock-recovery/` with `SHA256SUMS` and `VERIFICATION.md`.

Independent corroboration: the filename ends in `2292e2c0b1`, the leading 10 hex digits of
the file's own MD5 — Xiaomi's convention, and it confirms the file is the one the release
metadata describes rather than a re-upload.

**Download note:** the canonical `bigota.d.miui.com` host served this at ~20–66 KB/s
(ETA ~33 hours). The `bn.d.miui.com` mirror served it at ~4.47 MB/s and supports
byte ranges, so the transfer was resumed rather than restarted. Recorded because the slow
host is the one every guide links to.

### Compatibility — no downgrade required

The package version **is the firmware already on the device**. Restoring it returns the phone
to precisely its current state: same Android version, same security patch, same region. The
Android 9 rollback that the Ubuntu Touch page warns about is **not** involved anywhere in this
plan.

### Exact restore procedure (from the package's own scripts)

The archive ships three flash scripts. Their real behaviour:

| Script | flash cmds | userdata | notes |
|---|---|---|---|
| `flash_all.sh` | 53 | **erased** | full restore |
| `flash_all_except_data_storage.sh` | 51 | **preserved** | preferred for rollback |
| `flash_all_lock.sh` | 53 | erased | **relocks bootloader — do not use** |

Sequence (both slots where applicable): `crclist`, `sparsecrclist`, then `tz`, `xbl`,
`xbl_config`, `rpm`, `abl`; then `modem` (NON-HLOS.bin), `vendor`, `system`, `dtbo`,
`vbmeta`, `boot`; then `userdata` + `metadata`.

Two things that matter:

1. **It rewrites bootloader partitions** (`tz`, `xbl`, `xbl_config`, `abl`). That is why an
   interrupted flash is genuinely dangerous, and why the transport must be stable for the
   whole run, not just 61 seconds.
2. **`system_b` receives `system_other.img`** — a dummy image. This is the documented
   reason a stock-flashed Mi A3 can fail to boot afterwards, fixed by
   `fastboot set_active a`. Our device is currently on slot `_b`, so **this must be part of
   the procedure**, not discovered afterwards.

**`persist` is not touched** by any script. That matters for the Linux plan: Wi-Fi MAC and
sensor calibration live there and survive a restore.

---

## TASK C — native Linux image readiness

Kernel re-verified from source: `codeberg.org/sm61x5-mainline/linux`, tag `v7.2.3-r0`,
commit `5a88d98fa3d282ec4155a91577da86229d6b33d1`, `make kernelversion` = **7.2.3**.

| Requirement | Status | Evidence |
|---|---|---|
| **Sustained-read lockup fix** | **PRESENT ✓** | board DTS `reserved_mem1: memory@46200000 { reg = <0x0 0x46200000 0x0 0x4900000>; no-map; }` — 73 MiB, the *fixed* size. The defective mainline value was 45 MiB. This is the fix for the lockup-per-59-MiB-read bug, and it matters most for a long-running process. |
| **Storage** | present | `MMC_SDHCI_MSM=y`, ext4/f2fs/dm-crypt |
| **Wi-Fi driver** | present | `ATH10K_SNOC=m` (WCN3990) + `QCOM_Q6V5_WCSS=m` |
| **Wi-Fi firmware** | **from the device's own partitions** | `modules-initfs` contains `msm` = msm-firmware-loader, which extracts firmware at boot instead of shipping blobs. **Consequence: `vendor` and `persist` must be preserved by any Linux install, or Wi-Fi/BT will have no firmware.** |
| **Charging** | present | `CHARGER_QCOM_SMB2=m`, PMIC **PM6125** (`pm6125.dtsi`, `rpm-pm6125-regulators`, `pm6125_adc`) |
| **Thermal protection** | **PRESENT ✓** | 3 PMIC-ADC thermal zones on the board DTS — `rf-pa0-thermal`, `quiet-thermal`, `xo-thermal` — via `&pm6125_adc_tm 0/1/2`, each with a **critical trip at 125 °C** |
| **Thermal monitoring** | **PARTIAL ✗** | **TSENS die sensing is NOT in this kernel** — 0 `tsens` references in `sm6125.dtsi`. The Aug-2026 patch adding 15 die zones (37.7–39.6 °C idle, 52.2 °C peak vs 85 °C trip, tested on a Mi A3) is **not yet merged into v7.2.3**. |

**The honest distinction:** the device will have **safety protection** (critical-trip shutdown
on PMIC zones) but **not SoC die-temperature monitoring**. You asked to verify thermal
monitoring, and the answer is that half of it is there. Not a blocker — protection is the
safety-critical half — but it should not be reported as full thermal support.

**MCP service:** already integrated and unchanged — glibc ARM64 binary
`82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8`, systemd unit whose exec
conditions refuse to start without a 600-mode credential file and pin the listener to
loopback, APKBUILD that asserts the binary hash during `prepare()`.

Optional per your instruction, and not on the critical path: touchscreen, panel, microphone,
speaker, cameras.

---

## TASK D — installation gate

**USB/fastboot reliability: PASS** (61-second window; longer soak still wanted)
**Stock recovery package: VERIFIED** (size + MD5 exact match, provenance from Xiaomi's CDN)
**Proposed Linux image:** postmarketOS, kernel `linux-nura-qcom-sm61x5` 7.2.3, systemd,
headless, with our MCP package. **Image not yet built** — `pmbootstrap` has not been run, so
there are **no image hashes to report yet.** That is a real gap: the Linux side is
config-verified, not artifact-verified.

### Remaining risks

1. **Bootloader is LOCKED.** Flashing requires `fastboot flashing unlock`, which **wipes the
   device** and voids verified boot. This is the single largest gate and needs explicit
   approval. Unlocking also makes the phone refuse to boot a non-stock image without it —
   unavoidable for this plan.
2. **Interrupted flash is unrecoverable** — the scripts rewrite `xbl`/`abl`/`tz`. A drop
   mid-flash is a hard brick, not a retry.
3. **Soak duration.** 61 seconds proves the fault is gone; it does not prove eight minutes of
   uninterrupted flashing. A 15–30 minute soak should precede any write.
4. **Wi-Fi firmware depends on preserved `vendor`/`persist`.** A partition layout that
   overwrites those loses Wi-Fi.
5. **No Linux image hashes yet** — the artifact does not exist until `pmbootstrap` runs.
6. **TSENS monitoring absent** (see above).

### Proposed sequence — REQUIRES OWNER APPROVAL, NOT YET AUTHORISED

```
0. 15-30 min USB soak, 0 disconnects required
1. Backup user data already done (941 files, 0 mismatches)
2. Record: fastboot getvar all, current slot, anti-rollback version
3. fastboot flashing unlock            <- WIPES DEVICE, owner approval required
4. (Linux path) flash boot/dtbo/vbmeta from the postmarketOS image
   (recovery path) run flash_all_except_data_storage.sh to return to stock
5. fastboot set_active a               <- required after any stock flash on this A/B device
6. If recovering to stock: expect the device to be on slot a and boot normally
```

**Nothing above is authorised.** Steps 3 onward are destructive and gated on your explicit
approval of this exact sequence.

---

## TASK E — post-install acceptance

**Not performed. Nothing is installed, so there are no device measurements to report.**

The acceptance script is written and committed (`native-linux/acceptance/first_boot_acceptance.sh`)
and covers all nine items you listed: native boot, Wi-Fi, SSH, battery/thermal, systemd
startup, real MCP operation, both read-only tools, reboot/recovery, and power/reliability. It
is bounded, read-only, and reuses the single shared MCP protocol checker rather than
redefining the assertions.

**Every number it will produce will be a real device figure.** No container or emulated result
is being presented as a device measurement anywhere in this report, and the one device figure
that does exist — the 61-second USB window — is labelled with its duration and its limits.

---

## What changed since the last report

- **USB blocker: resolved** by a physical port change, and measured rather than assumed.
- **`fastboot` was missing** from `PATH` — found and located; this would have stopped a flash
  at step one.
- **Recovery package: obtained and checksum-verified**, exact version match, no downgrade.
- **The `set_active a` trap** is now documented from the package's own scripts rather than
  from a forum post, and our device is on slot `_b` so it applies.
- **DRAM carveout fix confirmed present**; **TSENS monitoring confirmed absent** — both
  checked in the actual kernel source rather than assumed from the patch series.
