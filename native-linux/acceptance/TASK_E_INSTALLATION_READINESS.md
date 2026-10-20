# Task E — Installation readiness report

Date: 2026-10-20  ·  Branch: `experiment/mi-a3-linux-runtime`  ·  HEAD: `caa794415599`

---

## 1. Recovery package — FULLY VERIFIED

| Check | Value | Result |
|---|---|---|
| Package | `laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0` | — |
| Size | 2,439,506,308 B | equals published `Content-Length` **PASS** |
| MD5 | `2292e2c0b1c0e9839d5b14427154b5b4` | equals Xiaomi's published value **PASS** |
| SHA-256 | `0e6b3c1b17aa860bacae1b979d4f63dd007a060f84291363263aef577444fe3c` | computed |
| gzip integrity | OK | **PASS** |
| Vendor manifest | 72 verified / 0 mismatched / 0 missing | **PASS** |

**No remaining blocker on the recovery package.** It is genuine, complete and
self-contained, and it carries a full EDL path (`prog_firehose_ddr.elf`, `rawprogram0-5.xml`,
`patch0-5.xml`, GPT binaries).

## 2. Stock boot-image analysis

Full detail in `TASK_A_STOCK_BOOT_ANALYSIS.md`. Headline:

```
boot.img   ANDROID! header v2, page 4096, kernel_addr 0x8000, ramdisk_addr 0x1000000,
           kernel 14,977,930 B gzip, ramdisk 8,279,472 B gzip (19,917,056 expanded),
           tags_addr 0x100, dtb_size 0, total 67,108,864 B
layout     header page 0 | kernel page 1 | ramdisk page 3658   (computed)
cmdline    console=ttyMSM0,115200n8 ... earlycon=msm_geni_serial,0x4a90000
dtbo.img   DT table, 17 entries, 9 with data, all FDT v16
A/B        confirmed from the package's partition.xml; device is on slot _b
```

**Unverified, named not substituted: all vbmeta AVB fields.** Three parse attempts gave three
different answers, so none is trustworthy; `avbtool` is unavailable and not installable here.

## 3. Boot bundle files and hashes

Bundle: `deploy/mi-a3-native-linux/` — **92 MB, 23 files**, `sha256sum -c SHA256SUMS` passes.

```
kernel/Image                              51,534,336 B
  c3a5589e7a32fb20d2c1df1deac2fa8dfe83cc1d60769285d28d4aa1a5f78627
kernel/sm6125-xiaomi-laurel-sprout.dtb        41,049 B
  66c3509356af238ff18a6fc24eb1bcc5f9ee949bdd487a3d4714826539d68a2f
rootfs/rootfs-ubuntu2404-aarch64.tar.zst  28,158,421 B
  57ec742ec794cb501aedbca915e2730034e566e6b89d2463a1ded94c6419443d
service/hg_mcp_server                      2,702,376 B
  82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
```

P0 modules packaged (10): `ath10k_snoc`, `ath10k_core`, `ath10k_pci`, `mac80211`,
`cfg80211`, `qcom_q6v5_wcss`, `qcom-spmi-adc-tm5`, `qcom-spmi-adc5`, `qcom-spmi-temp-alarm`,
`qcom-vadc-common`. Full set built: 1644 modules / 378 MB.

**NOT built:** initramfs (see §6), boot image, flashable image.

## 4. Rootfs and kernel checksums

Rootfs: Ubuntu 24.04 aarch64, 139 packages, 4811 entries, 145 MB unpacked.
`docker export` of the container filesystem — apt installs into the container's own
filesystem, not a bind mount, so the flattened export **is** the rootfs.

Kernel: `linux-nura-qcom-sm61x5` v7.2.3, source `codeberg.org/sm61x5-mainline/linux` tag
`v7.2.3-r0` → commit `5a88d98fa3d282ec4155a91577da86229d6b33d1`, release `7.2.3`.

**Verified source properties:**
- **Sustained-read lockup fix PRESENT** — `reserved_mem1` at `0x4900000` (73 MiB), not the
  buggy `0x2d00000` (45 MiB). Closes the highest-risk defect for a long-running hub.
- **Wi-Fi supported** — mainline `ATH10K_SNOC` (WCN3990) + `QCOM_Q6V5_WCSS`, built as modules.
- **Thermal CPU-blind** — see §6.

## 5. Verified-boot and DTBO assessment

| Question | Finding |
|---|---|
| Is boot.img AVB-signed at the boot layer? | **UNKNOWN** — vbmeta fields unparsed |
| Does the boot chain carry a real key/signature? | **UNKNOWN** — 4096 B file looks unsigned, but that is an inference from size |
| `verifiedbootstate` observed | **green** (on the running device, earlier) |
| Is that consistent with an unsigned vbmeta? | **in tension — unresolved, not guessed** |
| DTBO compatible with mainline DTs? | **UNKNOWN** — dtbo holds Qualcomm downstream DTs |
| Where should mainline's DTB go? | **OPEN** — 4 candidates, none validated |

**This is the single most consequential unknown.** On a locked bootloader, flashing a boot image
whose vbmeta is not correctly signed or whose verification flags are wrong yields a device that
does not boot. It is resolvable, but only with `avbtool` or the device's own bootloader state.

## 6. Thermal limitations and mitigation

Fully documented in `TASK_C_THERMAL_SAFETY.md`. Summary:

- **No CPU die temperature sensor.** Three PMIC zones watch an RF power amplifier, a quiet rail
  and a crystal oscillator.
- **No passive thermal trip.** All critical-only at 125 °C → the only response is a **hard
  shutdown**. **No CPU throttling exists.**
- Mitigation is therefore **workload bounding, not thermal control**: `CPUQuota=25%`, CPU0
  excluded, `MemoryMax=256M`, idle IO class, fail-closed startup gate, stop-time evidence.

The gate and record scripts were **executed**, not just written — four independent cases (cool /
at-gate / above-gate / no-sensors) plus the real-host run. That testing found and fixed a real
defect: the audit trail was silently not being written.

**The thermal problem is NOT resolved.** Unattended deployment is not authorised pending
physical measurement of real hardware thermal behaviour.

## 7. Exact owner actions for the next diagnostic session

**Immediate (unblocks the device):**

1. **Long-press POWER ~10–15 s** — the phone is in bootloader mode with a dead USB interface
   and cannot be commanded back over USB. If that fails, unplug/replug the cable, then retry.
2. Confirm return: `adb devices` shows `edd53dbd04de`, and
   `getprop ro.boot.flash.locked` = `1`.

**Then, to attack the fastboot blocker** (each a hypothesis, none claimed as the cause):

3. Move the cable to a **USB 2.0 port** (bootloader USB stacks frequently fail on xHCI 3.0).
4. Try a **different cable** — data-line quality matters more in the bootloader's bare-metal
   USB than under Android.
5. Insert a **USB 2.0 hub** in between — a well-known workaround for this `error -71` class.

**Then re-run the read-only fastboot session** to obtain what remains unknown:
`fastboot getvar anti`, `fastboot flashing get_unlock_ability`, `fastboot getvar current-slot`,
`fastboot getvar slot-successful`.

**Separately, to resolve the vbmeta unknown (no device needed):**
install `avbtool` and run `avbtool info_image --image vbmeta.img`. Alternatively capture from
Android: `ro.boot.vbmeta.device_state`, `ro.boot.verifiedbootstate`, `ro.boot.veritymode`,
`ro.boot.vbmeta.digest`, `ro.boot.vbmeta.avb_version`, `ro.boot.flash.locked`.

## 8. Installation GO / NO-GO

# **NO-GO**

| Gate | State |
|---|---|
| Recovery package verified | **PASS** |
| Kernel + modules + rootfs + service built and hashed | **PASS** |
| A/B layout and active slot understood | **PASS** |
| Thermal posture defined and tested | **PASS (as mitigation only)** |
| **Fastboot transport works** | **FAIL — measured, error -71** |
| **Rollback proven as a capability** | **FAIL — `anti` unread, rollback index unknown** |
| **Boot image format validated** | **FAIL — not built; DT placement unresolved** |
| **vbmeta / verified-boot handling validated** | **FAIL — fields unparsed** |

Three independent hard blockers. The fastboot one is the most serious because it is physical
and it invalidates the others' testing: **you cannot flash over a link that cannot
enumerate**, and a flash interrupted mid-write is the one failure mode that genuinely
hard-bricks the device.

Note this is a **NO-GO despite authorization being granted** — the owner's fastboot approval
was issued and the session was attempted. The authorization was not the blocker; the hardware
link was.

## 9. Commit

```
HEAD     caa79441559927c2368944fba533b6882f90a15f
branch   experiment/mi-a3-linux-runtime
remote   caa794415599  (matches)
tree     clean
```

Commits in this block:

| SHA | Subject |
|---|---|
| `141e5489cf63` | assemble the artifact bundle; correct a false thermal reading |
| `d38d01bf0061` | stock boot-image analysis + thermal posture + gate scripts |
| `caa794415599` | fix the thermal gate's marker directory |

## 10. Device state — UNTOUCHED

```
flash          not performed
unlock         not performed
erase          not performed
slot change    not performed
vbmeta change  not performed
factory reset  not performed
```

Device: Android 11, build `V12.0.26.0.RFQMIXM`, bootloader LOCKED, verified boot green, slot
`_b`, OEM unlocking allowed. User backup: 941 entries / 0 mismatches.

## 11. Roadmap

**Roadmap coverage missing.** There is no `project_checklist_items` row for the native-Linux
replacement of Android on the Mi A3. Per AGENTS.md rule 9 this is flagged for operator decision
rather than invented — rows are created by operator decision only.
