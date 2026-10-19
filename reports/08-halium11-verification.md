# Halium 11 / Ubuntu Touch lead — direct verification

Verification of the Ubuntu Touch Mi A3 lead, performed against the authoritative sources:
the UBports device/kernel repositories via the GitLab API, and the UBports system-image
server. Nothing was flashed, installed or modified. The postmarketOS work is preserved
untouched as a fallback.

---

## Deliverable 1 — VERIFIED NATIVE UBUNTU AVAILABLE: **NO**

There is **no installable Ubuntu image for the Mi A3 that matches this device's current
Android 11 firmware**.

The port is **real and actively maintained as source**, but **no image is published for it**.
The only published installable image is an **obsolete Ubuntu Touch 16.04 / Android 9** build
that requires the Android 9 downgrade which the device's own documentation says will render
an Android 11 device unusable.

So the answer is not "no" because the work is fake — it is "no" because the work exists as
source and stop short of a published, device-appropriate installation.

---

## Deliverable 2 — exact release and kernel, as published vs as configured

This is where the discrepancy lives, and it resolves cleanly once the two differ:

| | Published / installable | Configured in the Halium 11 source |
|---|---|---|
| Ubuntu release | **16.04 xenial** (EOL) | 24.04-1.x (noble) |
| Android base | **android9** | **android11 / Halium 11** |
| Kernel | 4.14 (Halium 9 era) | **4.14.357-openela**, fork of LineageOS `lineage-23.2` |
| Channel | `16.04/arm64/android9/stable` | none — no channel |
| Last build | `tag=OTA-25`, ubports `20230417-17419` | source commits 2026-09/10 |
| Index generated | **2023-04-23** | n/a |

**The Android 9 text on the device page is stale, but in the direction that does not help
us.** The port *has* moved to Halium 11 in source; the installer's published image has not.
The page's "Halium 11.0" badge describes the source tree. Its install text describes the
only thing you can actually install: the Android 9 image.

That is the discrepancy, and it means the badge is **not** evidence of installation safety.

---

## Deliverable 3 — exact image URL and checksum

An image exists, but **it is the wrong one** for this device. Recorded here so the finding is
checkable, and explicitly **not** recommended for installation.

```
channel  : 16.04/arm64/android9/stable
base URL : https://system-image.ubports.com/16.04/arm64/android9/stable/laurel_sprout/
version  : 10   (tag=OTA-25, ubports=20230417-17419, device=20230416-22700, boot=20220419-16253)

ubuntu rootfs  369259968 B  sha256 fb42e5938de7e3bc040e18e69766dc756284ec34f3bc0dd589f978d2ab47fa56
device          149370652 B  sha256 2a6be02aaa60a2e865273727fbfc2926aa9bdddb165858a3a816a976fc8efc2c
boot             32188068 B  sha256 01d597c81c34dcc721c69ea67d3da80c2c5e6f5a0d0d7cb2daa8cce5b35aaec0
keyring              1556 B  sha256 972d73c58b99fccc6a986951350b9b3b06d470f45bbc6d7f076452f312479bfa
manifest              492 B  sha256 31eb083cea16b77ff8edb8be20218733a3d8adc5ec31b5c6f041f52edede23d7
```

Files are checksummed and signed (GPG signature files present in the pool).

**Why this must not be installed:** it is the Android 9 line. Using it means downgrading a
device that is currently on Android 11 — which the device page itself warns will render the
device unusable. It is also xenial, long out of support, last built April 2023.

**For the Halium 11 / 24.04 configuration there is no image and no checksum**, because no
image is published. It is built locally by the maintainer in a Docker container
(`ut-builder-amd64`) from source.

---

## Deliverable 4 — Android 11 firmware compatibility evidence

**In the source configuration: positive and specific.** The device repo's `deviceinfo` states
an Android 11 base directly:

```
deviceinfo_halium_version="11"
deviceinfo_ubuntu_touch_release="24.04-1.x"
deviceinfo_bootimg_os_version="11.0.0"
deviceinfo_bootimg_os_patch_level="2022-08"
deviceinfo_kernel_source_branch="halium-11"
deviceinfo_kernel_defconfig="vendor/trinket-perf_defconfig vendor/xiaomi-trinket.config
                             vendor/laurel_sprout.config halium.config"
```

A boot image declaring `os_version 11.0.0` with a `halium-11` kernel indicates the port
targets **Android 11 firmware — this device's current state — with no downgrade required.**

**In the published image: negative.** The only installable artifact is on the android9
channel.

So the honest reading: **the Halium 11 port appears to be Android 11-compatible by
configuration, but this has not been turned into an installable artifact**, and I cannot
claim installation compatibility from a `deviceinfo` file. Configuration is evidence of
intent and target, not of a working install.

---

## Deliverable 5 — essential hardware compatibility matrix

From the port's own `README.md` (its claim, not my measurement):

| Requirement | Status per port README | Notes |
|---|---|---|
| Wi-Fi | **working** | plus Bluetooth, hotspot, flight mode |
| Charging | **working (online)**; offline charging **partial** | battery percentage, RTC, shutdown/reboot working |
| Thermal | **not documented** | no thermal claim either way; the port does describe "battery/thermal investigation" in dev notes |
| Recovery | **working** | UBports recovery with adb + fastbootd, OTA updates |
| SSH | **not claimed** | not mentioned; UT normally exposes SSH via the `phablet` account, unverified here |
| Native Linux kernel | **NO** | Android-derived 4.14 LineageOS kernel + HAL blobs, not mainline |
| Camera / touch / display / audio | working | P1 for us anyway |

**Two things that matter more than the tick-list:**

1. **Halium is not native Linux.** Halium runs the Ubuntu rootfs inside an **LXC container**
   on top of an **Android kernel with Android HAL blobs** (`libhybris`). It removes Android
   as the userspace and removes Termux/PRoot — but the kernel remains a vendor-derived 4.14,
   not the mainline stack the postmarketOS option uses. If the goal is genuinely "native
   Linux kernel", Halium does not satisfy it.
2. **Thermal is undocumented.** For an unattended hub this is the safety-critical row, and
   the port makes no claim about it.

---

## Deliverable 6 — Ubuntu versus postmarketOS

Ranked against your criteria. **A** = Ubuntu Touch Halium 11, **B** = Ubuntu 24.04 Minimal on
the mainline kernel, **C** = postmarketOS.

| Criterion | A: UT Halium 11 | B: Ubuntu 24.04 + mainline | C: postmarketOS |
|---|---|---|---|
| Verified Android 11 firmware compat | configured, **not proven by an image** | n/a | n/a (replaces Android, no base needed) |
| Installable artifact | **none published**; only obsolete android9 | **none** | **image build exists** (pmbootstrap) |
| Reproducible native boot image | buildable from source locally | **not demonstrated** | yes, checksum-pinned |
| Wi-Fi | working (port claim) | expected (ath10k in config) | expected (`ATH10K_SNOC`) |
| Charging + thermal safety | charging yes; **thermal undocumented** | charging expected; **thermal in kernel** (`QCOM_TSENS`) | charging expected; **thermal demonstrated on this board** |
| Reliable SSH | not claimed | yes (we supply config) | yes (we supply config) |
| C++ MCP compatibility | glibc-ish via LXC; untested | **built for this exact ABI** | **built for this exact ABI** |
| Recovery readiness | UBports recovery + fastbootd | needs building | fastboot + reflash |
| Installation effort | low **if** an image existed | very high (boot chain from scratch) | medium |
| Maintenance / security | kernel 4.14, **EOL upstream lines** | mainline **7.2.3** | mainline **7.2.3** |
| Native Linux kernel | **no** | yes | yes |

**Ranking: C > A > B.**

- **C wins** on the things you called essential: genuine mainline kernel, demonstrated
  thermal sensing on this exact board, an existing image build, and current security support.
- **A** is closer than expected and its source is healthy, but it has no installable image
  for our firmware, no thermal claim, and is not a native kernel.
- **B** is last **not** because it is a bad idea but because it is **undemonstrated**: it
  would mean authoring a boot chain and device integration that C already provides. It is a
  fallback worth keeping, not a plan to start.

---

## Deliverable 7 — source and build evidence

```
device repo  ubports/porting/community-ports/android11/xiaomi-mi-a3/xiaomi-laurel_sprout
             default branch master @ a6c77af1   last activity 2026-10-19T12:38:14Z
             other branch UT-24.04-1.x-pre-rebase @ 8e9cb3c0 (2026-09-07)
             files: deviceinfo, build.sh, DEVELOPMENT.md, overlay/, ramdisk-recovery-overlay/

kernel repo  ubports/porting/community-ports/android11/xiaomi-mi-a3/kernel-xiaomi-laurel_sprout
             default branch halium-11 @ 3db60dc9   last activity 2026-09-28T08:00:28Z
             other branch master (lineage-16 era) @ da7ee9b8 (2022-01-22)
             base: fork of lineage-23.2, 4.14.357-openela

images       https://system-image.ubports.com/channels.json
             laurel_sprout present in exactly 3 channels, all 16.04/arm64/android9/*
             (stable, rc, devel) — and in none of the 24.04-* or android11 channels
             index generated 2023-04-23, latest tag OTA-25

build        the maintainer builds in a Docker container:
             docker exec ut-builder-amd64 bash -c 'cd /tmp/device-xiaomi-laurel_sprout && ./build.sh -k'
             NOT reproducible from the repo alone — the build tree and tooling live in that
             container, and the README notes everything gitignored is downloaded or generated
             there. This is a real reproducibility gap for anyone else.
```

The port's `DEVELOPMENT.md` documents six kernel fixes required just to boot Ubuntu Touch —
including an `initramfs.c` fix where `skip_initramfs` discarded the Halium ramdisk on this
A/B device and booted Android instead. That is unusually detailed, credible engineering work.

---

## Deliverable 8 — exact pushed commit

Commit SHA is recorded in the covering message for this work; the file itself is the evidence
trail above, and the repositories it cites are external and unmodified.

---

## Deliverable 9 — installation GO / NO-GO

### Ubuntu Touch (option A): **NO-GO**

Not because the port is bad — it is credible, active, and its development notes show real
kernel work. Three independent reasons:

1. **No installable image for this firmware.** The only published image is
   `16.04/arm64/android9`, requiring the Android 9 downgrade that bricks Android 11 devices.
2. **Not a native kernel.** Halium is an LXC container over an Android 4.14 kernel with HAL
   blobs. It does not meet "native Linux kernel".
3. **No thermal claim**, and thermal is the safety-critical property for an unattended hub.

### Native Ubuntu 24.04 + mainline kernel (option B): **NO-GO as a plan to start**

Not demonstrated anywhere, and it would require authoring a boot chain the postmarketOS port
already provides. Kept as a fallback, not a starting point.

**What is missing, specifically:** a working device boot chain (`boot.img`/`vbmeta`/`dtbo`
layout for this A/B device), an initramfs that survives this device's `skip_initramfs`
behaviour, a DTB/DTBO pair for `laurel_sprout`, the `${vendor}`-firmware path that supplies
WCN3990/DSP blobs, and a validated rootfs image. Each is real work; none is done.

### postmarketOS (option C): **recommended**, subject to the same flashing gate

Unchanged from the previous assessment, and now the only option with a genuine mainline
kernel, demonstrated thermal sensing on this board, and an existing image build.

### Blockers that apply to all three (independent, unchanged)

1. **Unreliable flashing transport** — USB drops with `error -71`, repeated re-enumeration.
2. **No verified stock Android 11 restore image held.**

Neither is resolved by choosing a different OS. Both must clear before *any* of A, B or C is
flashed.

---

## What this verification changed

- The device page's **"Halium 11.0" badge is not evidence of installation safety** — your
  suspicion was correct, and the reason is that the badge describes the source tree while
  the installer text describes the published image. They are two different things.
- The Android 9 requirement is **stale relative to the source, not relative to what is
  installable**. Anyone reading the page and installing gets the Android 9 image.
- The Halium 11 port is **real, current and Android 11-targeted in configuration** — it is
  a genuinely promising fallback if a published image appears, and worth revisiting.
- **No Ubuntu image exists** for this device's firmware. Recommending Ubuntu on this evidence
  would mean recommending a downgrade that the vendor documentation says bricks the device.

postmarketOS integration, kernel investigation, the MCP binary and the user-file backup are
all preserved and untouched.
