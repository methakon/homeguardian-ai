# HG-CAM-02 / HG-CAM-03 — Path B: Alternative-OS Feasibility for the Panasonic ELUGA I7

Target device, verified by ADB serial `<DEVICE_SERIAL>`:

| property | value |
|---|---|
| model | `ELUGA_I7_EE` (Panasonic) |
| board | `k39tv1_64_a27` |
| SoC | MediaTek MT6739, 4x Cortex-A53, arm64-v8a |
| OS | Android 9 / API 28, patch level 2021-04-05 |
| fingerprint | `Panasonic/ELUGA_I7_EE_Z/ELUGA_I7_EE:9/PPR1.180610.011/1617357334:user/release-keys` |
| partitions | **A-only** (`ro.boot.slot_suffix` empty, A/B = false) |
| Treble | `ro.treble.enabled=true`, vndk 28 |
| boot state | `ro.boot.flash.locked=1`, `verifiedbootstate=green`, `veritymode=enforcing` |
| unlock | `ro.oem_unlock_supported=1` but **`sys.oem_unlock_allowed=0`** |
| build | `user/release-keys`, `ro.secure=1`, `ro.debuggable=0` |
| storage | 10 GB `/data`, 8.5 GB used (90 %) |
| RAM | 1,904,764 kB total, ~464 MB swap in use |

## Question asked

> Determine whether a maintained, model-specific replacement ROM exists with working
> camera, Wi-Fi, microphone, speaker and hardware encoding.

**Answer: no. No maintained, model-specific replacement ROM exists for this device, and
there is no public path to build one.** The reasoning, and exactly what was checked, follows.

## B-1. Device-specific ROMs — verified negative

Checked through the GitHub API rather than by scraping vendor pages, because the
LineageOS and TWRP device lists are JavaScript-driven and return ~1 KB of text to a
non-browser client — a zero hit there would have proven nothing.

| check | method | result |
|---|---|---|
| LineageOS official devices | `gh search repos lineage_device_panasonic` | **NO RESULTS** — LineageOS has never carried a Panasonic device |
| LineageOS wiki list | rendered page, counted `Panasonic`/`Eluga` | 0 occurrences |
| /e/OS device selector | rendered page, **44,327 chars loaded** (so the list really was present, not a JS stub), counted `Panasonic`/`Eluga` | 0 occurrences — no Panasonic brand at all |
| TrebleDroid tested-GSI wiki | rendered page, counted `Panasonic`/`Eluga` | 0 occurrences; the only `k39` row is `Coolpad C558 / k39tv1`, a **32-bit `arm`** device — the same MediaTek platform name, a different phone entirely |
| `android_device_panasonic_*` | `gh search repos` | matches exist, listed below |
| `ELUGA_I7` | `gh search repos` | **exactly one** hit, listed below |
| the kernel tree the device tree expects | `gh search repos "kernel panasonic k39tv1"` | **no results** — the `kernel/panasonic/ELUGA_I7_EE` source it points at was never published |

Every Panasonic device tree that exists:

| repo | last push | note |
|---|---|---|
| `twrpdtgen/android_device_panasonic_ELUGA_I7_EE` | 2024-12-05 | **this exact model** — see B-2 |
| `twrpdtgen/android_device_panasonic_Eluga_Ray_810` | 2023-03-31 | different model, generator output |
| `AndroidBlobs/device_panasonic_ELUGA_Z1` | 2023-06-24 | different model |
| `twrpdtgen/android_device_panasonic_P85Nxt`, `..._FZ-S1` | 2023 / 2026 | different models, Toughpad-class |
| `PyYoshi/android_device_panasonic_ponyo`, `hiikezoe/..._p01d` | 2013 | different model, decade old |
| P55 Novo trees (several forks) | 2017-2023 | different model |

So: no LineageOS, no /e/OS, no PixelExperience, ArrowOS or crDroid build exists for
`ELUGA_I7_EE`, official or widely-forked unofficial. Every Panasonic tree on GitHub
belongs either to a different model or is generator output.

## B-2. The one artifact that exists is not a ROM

`twrpdtgen/android_device_panasonic_ELUGA_I7_EE` is real and was pushed 2024-12-05
(1 star, no description, `isTemplate: false`). It is **output of the TWRP Device Tree
Generator** — a script that reads a stock recovery/firmware image and emits a skeleton
device tree. It is not a ROM, has no kernel, no vendor blobs and no build recipe, and
its own tooling documents the output as a starting point that a porter must then fix by
hand. Treating it as "an existing port" would be wrong. It is at best evidence that
somebody else pointed the generator at this model once.

I read its `BoardConfig.mk` rather than relying on the README. It confirms the model
(`TARGET_BOOTLOADER_BOARD_NAME := k39tv1_64_a27`, `TARGET_ARCH := arm64` with an `arm`
2nd arch), and it declares:

    TARGET_KERNEL_SOURCE := kernel/panasonic/ELUGA_I7_EE

That is the decisive line. The tree **points at a kernel source tree that does not
exist publicly** — searching for it returns no results. And the repository contents are
scaffolding only: `Android.bp`, `Android.mk`, `AndroidProducts.mk`, `BoardConfig.mk`,
`README.md`, `device.mk`, `extract-files.sh`, `omni_ELUGA_I7_EE.mk`, an empty `prebuilt/`,
`recovery.fstab`, `setup-makefiles.sh`, `vendorsetup.sh`. No kernel, no HAL, no device
source. It cannot build anything as it stands.

## B-3. Custom ROMs are blocked by the kernel, not by interest

A custom ROM needs kernel source built against this board's peripherals. None is public:

- Panasonic's OSS distribution pages (`ospo.panasonic.com/oss/...`,
  `panasonic.jp/.../eluga/opensource.html`) publish **licence texts and named
  components**, not a device kernel source drop for this model.
- The only mainline work for any Panasonic handset that surfaced is
  **ELUGA U2 (MSM8916, 2015, Qualcomm)** — a different vendor's SoC from five years
  earlier, and therefore irrelevant here.
- An XDA thread from a developer asking how to build a device tree for the
  **Eluga Z1 Pro** states plainly that *no kernel source was released* — the same
  pattern, an adjacent model in the same lineup.

No kernel source, no vendor blobs, no ROM. This is the hard blocker; the rest are
secondary.

## B-4. Even unlocking the bootloader is not available as shipped

The device reports `ro.oem_unlock_supported=1`, which is the tempting signal, but
`sys.oem_unlock_allowed=0` — the **OEM unlocking toggle is not permitted**, so
`fastboot flashing unlock` is not an available path on this unit as it stands.

The remaining route is `mtkclient`, which exploits the MediaTek BROM/preloader to
patch the `seccfg` partition (`python mtk da seccfg unlock`). Its own documentation
lists four conditions that all apply here:

1. **OEM-unlock dependency** — "certain devices require enabling 'OEM Unlocking' in
   Developer Options before MTKClient can unlock the bootloader. This setting is
   inaccessible on locked or bricked devices." Our toggle is not permitted.
2. **Data loss** — unlocking wipes userdata.
3. **Brick risk** — "incorrect commands or interruptions can soft-brick the device",
   and the fix is firmware restoration.
4. MTKClient's source carries preloader security patches specifically labelled
   `"mt6739 c30"` — the MT6739 is targeted, but that also means it is a generation
   where the vendor patched back.

None of this is forbidden, but it is a destructive, warranty-voiding, brick-risk path
whose first step may be blocked outright.

## B-5. GSI (Treble) — architecturally possible, empirically unverified

`ro.treble.enabled=true` with vndk 28 means the partitioning *permits* a Generic System
Image. That is the only reason a GSI is even worth mentioning. It is **not** a
recommendation:

- No indexed success report exists of any GSI booting on `ELUGA_I7_EE`. Nobody has
  published this working.
- A-only with no A/B slots means no seamless rollback; a bad flash means recovery via
  firmware tooling.
- The camera is `android.info.supportedHardwareLevel = LIMITED` and the HAL is
  HAL3-style over MTK vendor blobs. Camera is the single most vendor-dependent
  subsystem on MediaTek, and a GSI ships a generic camera stack. **A GSI that boots
  is not a GSI with a working camera**, and the camera is the entire point of a CCTV
  node.
- Verified boot is `green` with `veritymode=enforcing`; disabling verity is a further
  destructive step.

A GSI would trade a working, known camera for an unverified stack whose most likely
failure mode is exactly the feature we need. That is a bad trade for a security camera.

## B-6. Firmware availability and realistic restoration options

| route | availability | verdict |
|---|---|---|
| Manufacturer / service-centre restore | the only authoritative path | reliable, not self-service |
| Third-party stock ROM (needrom, ELUGA I7 EE, `EB-90S54E7Ev3006`/`v2009`) | listed, **unverified**, device is on `v3024` | usable only as a last resort, never as a first step — an unverified image on a security camera is a supply-chain risk |
| Custom recovery (TWRP) | no official build; only generator output | none |
| GSI | unverified, camera at risk | not recommended |
| Linux port | no kernel source, MTK vendor blobs, MT6739 not in mainline | **not feasible** |

## B-7. Risk comparison, and the recommendation

**Replacement OS / Path B**

- Must first unlock a bootloader the firmware does not permit, via a BROM exploit, with
  data loss and brick risk, on a device whose only recovery image is an unverified
  third-party download.
- Then find there is no ROM to install, and that building one needs kernel source that
  does not exist.
- Best case short of that: an unverified GSI with a strong chance of a broken camera.
- Loses the 2021-04-05 security patch level, or freezes it.
- Keeps the phone's data at risk the whole time.

**Clean stock Android 9 + dedicated CCTV foreground service / Path A**

- Verified feasible on documented grounds: Android 9 names the **foreground service**
  as the fix for its camera/microphone background restrictions, and targeting API 28
  avoids the Android 11+ while-in-use `foregroundServiceType` gates entirely.
- Hardware is sufficient and measured: `OMX.MTK.VIDEO.ENCODER.AVC` H.264 up to
  1920x1080, measured 70-155 fps at 720p; PCM-16 audio input to 48 kHz; AOSP acoustic
  echo canceller present (`libaudiopreprocessing.so`); 2.4 GHz Wi-Fi at 65 Mbps.
- Destroys nothing and is reversible with a normal app uninstall.
- Real constraints, stated honestly: 975 MB free on `/data` argues for **streaming
  rather than storing** on-device; ~464 MB of swap in use and a `LIMITED` camera HAL
  mean the app must be modest; thermal telemetry is root-only; 2.4 GHz-only Wi-Fi is
  adequate but not generous.

**Recommendation: Path A. Do not pursue a replacement OS on this device.** The
replacement-OS route is not merely risky, it is *blocked* — there is no ROM to install
and no way to build one. Path A needs no destructive step at all, and the only
capability Path B would add in theory (a different Android version) is not worth
trading a verified camera for an unverified one.

## B-8. Sources that look like evidence and are not

Flagged explicitly so nobody re-derives a conclusion from them:

- **Content farms.** Pages titled "How to install AOSP/GSI on Panasonic Eluga I7
  Enterprise Edition" are templated articles: no build, no test, no boot report. A
  GSI *install guide* is not evidence that a GSI *boots* on this model. Non-evidence.
- **Repair-tool listings.** ChimeraTool and similar list this exact model. That is
  box/SIM-unlock and FRP tooling coverage — it says nothing about custom-ROM or recovery
  support, and must not be read as such.
- **Platform-name coincidence.** `k39tv1` is a MediaTek platform identifier, not a device
  identifier. Finding `k39tv1` in another device's row (the Coolpad C558) does not make
  that device's results transferable — ours is arm64, that one is 32-bit `arm`.

## B-9. What is inferred rather than verified

Being precise about the boundary, because these are the subsystems a port would live or
die on and I do **not** have primary evidence for them on this board:

- **Camera HAL and per-sensor configuration** — *inferred*. The generic MT6739 Android 9
  vendor model puts camera behind a vendor HAL plus per-sensor blobs and configuration.
  What this *board* actually uses is not confirmed from any primary source. What I did
  verify is device-side and narrower: the camera reports
  `android.info.supportedHardwareLevel = LIMITED` with a HAL3-style interface.
- **Wi-Fi driver and firmware** — *inferred*. MT6739 vendor builds ship the driver blob
  and firmware in `/vendor`; the exact part on this board is not confirmed. Verified to
  the extent that the radio is 2.4 GHz only, which is a measurement, not a driver fact.
- **Audio HAL** — *inferred*. Same reasoning; the AOSP echo canceller being present is a
  verified device fact, but the HAL variant is not.

None of this weakens the conclusion, and that is the point: B-3 and B-4 fail before
camera, Wi-Fi or audio even come into scope. There is no kernel to build and no permitted
unlock, so driver-level uncertainty never becomes the deciding factor.

## Confidence and what remains unverified

**Confidence: high** on "no device-specific ROM exists" and "no kernel source is
published" — both rest on API-level searches with explicit zero results, not on page
scraping.

**Confidence: medium** on the bootloader-unlock analysis — mtkclient behaviour varies by
preloader revision, and this specific unit was never attempted. It is worth noting the
recommendation does not depend on that analysis: even a *successful* unlock leads to a
dead end in B-3.

**Not verified, and not claimed:** no firmware image was downloaded, no GSI was tested,
and no unlock was attempted. The `twrpdtgen` tree was not cloned or built. Nobody has
published a GSI booting on this model, and this report does not claim otherwise.
