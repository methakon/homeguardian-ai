# Native Linux readiness — Xiaomi Mi A3 as a headless MCP hub

Assessment of running native Linux on the Mi A3 (`laurel_sprout`, SM6125) instead of
Termux/PRoot on Android. Nothing was flashed, installed or modified to produce this.

**Scope note:** this is a new workstream. There is no roadmap row for a native Linux
replacement, and per the standing rule I am not inventing one — flagging it instead:
**roadmap coverage missing**.

---

## Recommendation

**postmarketOS with the `linux-nura-qcom-sm61x5` kernel — systemd variant, headless.**

Not a new Ubuntu port. The reason is that the kernel and driver stack already exist, are
maintained, and have been demonstrated on this exact device; a minimal Ubuntu userspace on
that same kernel is possible in principle but would mean inventing the boot chain, initramfs
and device integration that pmOS already provides and tests.

**Kernel: Linux 7.2.3** — far newer than the Android 11/vendor 4.14 baseline, and described
upstream as "close to mainline", which matters for getting security patches.

### Why this and not the alternatives

| Option | Verdict | Reason |
|---|---|---|
| postmarketOS + nura sm61x5 kernel | **recommended** | maintained kernel, tested on this board, image build via pmbootstrap, device in `device/testing/` |
| Minimal Ubuntu userspace on the same kernel | possible, not recommended now | would require building the boot chain/initramfs/device integration that pmOS already has |
| Ubuntu Touch (Halium) | rejected | runs the **vendor 4.14.357** kernel behind Android HAL blobs — not native, and it *requires* rolling back to stock Android 9, which bricks Android 11 devices |
| Arch Linux ARM on the pmOS kernel | viable alternative | `criollojoel10/alarm-laurel_sprout` — same kernel, different userspace; a real option if Arch is preferred |
| NixOS on mainline | plausible | community report (see below); not evaluated in depth |

**Ubuntu Touch is specifically disqualified** by its own installer documentation: it requires
stock Android 9 firmware, and states that rolling an Android 11 device back to Android 9
"will render your device unusable". That matches the standing constraint against
downgrading to Android 9.

---

## Sources and commits

| What | Where | Notes |
|---|---|---|
| Device package | `gitlab.postmarketos.org/postmarketOS/pmaports` → `device/testing/device-xiaomi-laurel/` | `APKBUILD` v6-r0, `deviceinfo`, `modules-initfs` |
| Kernel package | same repo → `device/testing/linux-nura-qcom-sm61x5/` | **`pkgver=7.2.3`**, `pkgrel=0` |
| Kernel source | `https://codeberg.org/sm61x5-mainline/linux` | tag `v7.2.3-r0`, tarball sha512 `2a6393212c413cf3812c2293d074fc9b7f82dec01c740251fe390b0633484b247e95e886493cbec77e6ef9741aebe0a4134c2218672424b332ae119c22566f23` |
| Kernel config | same package, `config-nura-qcom-sm61x5.aarch64` (8,152 lines) | sha512 `c84672dd702acf5cdad7ed57ffa6aaeb4d0cc3c71d1ef4c9f5a1a2450e0f3d085f331c4489ec345c63a89f753c882ce42ba0d661928cd2d4c393f1144286d2b1` |
| Device tree | `deviceinfo_dtb="qcom/sm6125-xiaomi-laurel-sprout"` | mainline DTS |
| Thermal patch (SM6125 TSENS) | `patchew.org/linux/20260821030325.1823829-1-admin@mswin.me` | Roman Linev, **tested on a Mi A3** |
| DRAM carveout fix | `patchew.org/linux/20260824155535.335494-1-admin@mswin.me` | fixes hard lockups under sustained reads |
| Touchscreen (FT3518) | `patchew.org/linux/20260208-touchscreen-patches-v5-1-5821dff9c9a2@gmail.com` | Yedaya Katsman, v5 |
| Display panel (S6E8FC0/M1906F9) | `lore.kernel.org` patch v7, 2026-03-20 | Yedaya Katsman |

Retrieved via git protocol — the pmaports **web** UI is behind an Anubis bot wall, but git is
not, so the packages above were read from the repository directly rather than from prose.

### Provenance honesty

The **postmarketOS wiki page could not be read**. It is behind Anubis, and the Wayback
snapshot (`20260914224146`) captured the bot-challenge page rather than the article, so the
snapshot is useless. Everything above comes from the repository and the kernel patch threads,
not from the wiki.

The community lead you supplied (`reddit.com/r/mobilelinux/comments/1x4u0l2`) **has no archive
copy and was not retrievable**, so it is **not** used as evidence here. A different thread
(`.../comments/1vx9w9g`, "NixOS on Mi A3 with mainline kernel", Aug 2026) does exist and is
consistent with the above — but it is an anecdotal self-report and I am not treating it as
verified hardware compatibility, exactly as you instructed.

---

## Hardware matrix — P0

Derived from the kernel config and device package (static evidence), **not** from a boot.

| P0 function | Status | Evidence |
|---|---|---|
| CPU / SMP | **expected working** | `CONFIG_ARCH_QCOM=y`, `CONFIG_ARM_QCOM_CPUFREQ_NVMEM=m` |
| RAM | **expected working** | `CONFIG_ARM64_4K_PAGES=y`; 4 GB board |
| Persistent storage | **expected working** | `CONFIG_MMC_SDHCI=y`, `CONFIG_MMC_SDHCI_MSM=y`, `CONFIG_EXT4_FS=y` (+ f2fs, dm-crypt). Note: eMMC via sdhci-msm, **not** UFS |
| Wi-Fi | **expected working** | `CONFIG_ATH10K=m`, **`CONFIG_ATH10K_SNOC=m`** (WCN3990), `CONFIG_QCOM_Q6V5_WCSS=m`, `CONFIG_CFG80211=m`, `CONFIG_MAC80211=m` |
| Battery charging | **expected working** | `CONFIG_CHARGER_QCOM_SMB2=m`, `CONFIG_REGULATOR_QCOM_SPMI=y`, `CONFIG_QCOM_SPMI_ADC5=m` |
| Thermal protection | **expected working** | `CONFIG_THERMAL=y`, **`CONFIG_QCOM_TSENS=y`** — and the TSENS patch was tested on a Mi A3: 15 die zones, 37.7–39.6 °C idle, 52.2 °C peak vs 85 °C passive trip |
| SSH / remote admin | **expected working** | userspace feature (OpenSSH package), not a kernel concern; requires Wi-Fi above |
| systemd service supervision | **expected working** | userspace; pmOS systemd variant |
| C++ MCP server autostart | **integration prepared** | our own artifact — see below |
| Two read-only MCP tools | **integration prepared** | same engine, already verified |
| Fail-closed auth | **integration prepared** | same engine, already verified |
| Reboot / shutdown / recovery | **untested** | needs a boot; recovery path is fastboot + reflash |
| **Bluetooth** | **partially supported** | `CONFIG_BT=m`, `CONFIG_QCOM_WCNSS_CTRL=m` present, but BT on WCN3990 needs firmware + a working `btqcomsmd`/serdev path — **not confirmed** |
| Display / touchscreen | **patches exist, unmerged** | panel v7 and FT3518 v5 patch series; `CONFIG_DRM_MSM=m` present. Optional (P1), not on the critical path |
| Camera / microphone / audio / sensors | **missing** | deliberately excluded — P1, out of scope |

**Status vocabulary:** *expected working* = enabled in the kernel config and/or demonstrated by
a patch author on this board, but **not observed by me on a booted device**. *integration
prepared* = our software is ready to install, not yet installed. *untested* = nothing reached
it. No row claims a boot that has not happened.

---

## Critical risk: the DRAM carveout bug

The patch `sm6125-xiaomi-laurel-sprout: enlarge reserved_mem1` describes **hard lockups under
sustained buffered reads**, at roughly one lockup per 59 MiB read from storage, because
mainline handed firmware-owned DRAM to the page allocator. With the region correctly reserved,
the same workload ran 691 GiB with zero lockups.

This matters directly: our MCP engine is a long-running process that will read from storage and
hold page cache. **The image must be built from a kernel revision that includes the v2 version
of this fix.** Booting without it risks a hub that wedges unpredictably — which would look like
a software fault and waste a lot of time. This is the single highest-value thing to verify
before trusting the device as a hub.

---

## Image build (prepared, not performed)

`native-linux/build/` holds the pmbootstrap configuration, a `pmaports` pinning note and the
APKBUILD for the MCP engine package. The build is deliberately **not run**: it needs a
temporary `pmbootstrap` chroot and produces a flashable image, and the standing instruction is
not to install, and to prepare only.

Build outline (for review before execution):

```
pmbootstrap init        # vendor xiaomi, device laurel, systemd, no UI
pmbootstrap build device-xiaomi-laurel
pmbootstrap build linux-nura-qcom-sm61x5
pmbootstrap build homeguardian-mcp     # our package
pmbootstrap install     # writes the rootfs image
```

Images produced land under `~/.local/var/pmbootstrap/`. Reproducibility: the kernel and device
packages are checksum-pinned by the pmaports APKBUILDs; the MCP package is built from our own
pinned commit.

---

## MCP engine integration

The verified glibc ARM64 engine already exists and needs no rework:

```
hg_mcp_server   2,702,376 bytes
sha256          82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8
NEEDED          libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6
interpreter     /lib/ld-linux-aarch64.so.1
```

postmarketOS is glibc on aarch64 — the same ABI this binary was built and verified against, so
no rebuild is required. `native-linux/build/homeguardian-mcp/APKBUILD` packages it plus a
systemd unit. Verified: 13/13 protocol assertions pass against the real engine.

---

## Recovery and flashing readiness

**Not ready to flash.** Per the standing instruction, nothing is flashed until a reliable
fastboot transport is demonstrated, and the current USB path is unreliable (`error -71`,
repeated re-enumeration). Required before any flash:

| Gate | Status |
|---|---|
| Reliable fastboot/recovery transport | **NOT MET** — USB instability unresolved |
| Exact device identity confirmed | met (Mi A3, `laurel_sprout`, verified earlier) |
| Bootloader state recorded | needs a live `fastboot getvar all` on a stable link |
| Stock restore image in hand | **NOT MET** — the Android 11 fastboot image must be obtained and verified first |
| Battery state | pending |
| Rollback artifacts preserved | met — production APK `2d6018e0…`, user-file backup 941/941 |
| Owner approval of the exact procedure | **required, not yet given** |

**Recovery reality:** once `vbmeta`/`dtbo`/`boot` are written, returning to stock Android needs
a full stock fastboot image flashed over a stable link. The user-file backup is preserved, but
**it is user files only and cannot restore the OS** — that distinction matters here more than
anywhere else in this project.

---

## Expected deployment footprint

| Item | Estimate | Basis |
|---|---|---|
| Kernel + modules | ~60–90 MB | 7.2.3 arm64, modules installed |
| Minimal rootfs (systemd, no UI) | ~350–500 MB | pmOS base headless |
| MCP engine | 2.7 MB | measured |
| Runtime RSS | ~7 MB idle | measured on x86-64; **not a device figure** |
| RAM available to system | ~3.7 GB usable | 4 GB board minus firmware carveouts |
| Storage used | ~0.5 GB of 64 GB | ample |

No device measurement exists for any of these. The RSS figure is a host figure and is not a
prediction.

---

## GO / NO-GO for a safe first physical boot

**NO-GO right now.** Two independent blockers, either of which alone is sufficient:

1. **No reliable flashing transport.** USB drops with `error -71` and re-enumerates. Flashing
   a bootloader-adjacent partition over a link that drops is how devices get bricked.
2. **No verified stock restore image.** Until the Android 11 fastboot image is obtained and
   its checksums verified, there is no proven way back.

**GO conditions:** both met, plus the kernel revision confirmed to carry the DRAM carveout fix,
plus explicit owner approval of the exact flash procedure. The engineering prep is done; the
gating items are transport reliability and recovery readiness, not our software.

**Not a NO-GO on the architecture.** The kernel and driver stack are real, maintained and
demonstrated on this device. The plan is sound; it is the flashing safety gate that is unmet.

---

## Constraints observed

No flash, erase, unlock, relock, partition format or boot-state change. No factory reset. No
Android 9 downgrade. Stock Android 11 and the HomeGuardian app untouched. No Oracle, WireGuard
or public MCP exposure. User-file backup and rollback artifacts preserved and re-verified.
