# Mi A3 native Linux — build record

Final host-side build record. **No device was flashed, unlocked, erased or modified.**
Everything here was produced on the workstation without root.

Bundle: `/home/swarna-sekhar-dhar/projects/homeguardian-ai/deploy/mi-a3-native-linux/`
Logs:   `/home/swarna-sekhar-dhar/projects/homeguardian-ai/deploy/logs/`

---

## Artifacts and hashes

| Artifact | Size | SHA-256 |
|---|---|---|
| `kernel/Image` | 51,534,336 B | `c3a5589e7a32fb20d2c1df1deac2fa8dfe83cc1d60769285d28d4aa1a5f78627` |
| `kernel/sm6125-xiaomi-laurel-sprout.dtb` | 41,049 B | `66c3509356af238ff18a6fc24eb1bcc5f9ee949bdd487a3d4714826539d68a2f` |
| `rootfs/rootfs-ubuntu2404-aarch64.tar.zst` | 28,158,421 B | `57ec742ec794cb501aedbca915e2730034e566e6b89d2463a1ded94c6419443d` |
| `rootfs/...tar` (uncompressed) | 142,263,808 B | `31f3a9995c595237456c5af24d6e5bb84d736bc4235cfbb9bc52b8f4d45186a6` |
| `service/hg_mcp_server` | 2,702,376 B | `82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8` |
| `service/hg-mcp.service` | — | `b7cd3ddf11ec1b588445b16becfdf811ada167aa993d1566b500b4cc5b6b8c6e` |
| `service/hg-mcp.conf` | — | `68d94b3597277e8d7465675e22785fa07a884ef5a7ad3e919bb3dbfcb24dda9f` |
| `SHA256SUMS` | — | covers every file above |

Bundle total: **92 MB**. Full module tree (1644 modules, 378 MB) remains at
`~/homeguardian-native-build/modules-root`; only the 10 P0 modules are packaged.

## Kernel

```
source    https://codeberg.org/sm61x5-mainline/linux
tag       v7.2.3-r0
commit    5a88d98fa3d282ec4155a91577da86229d6b33d1
release   7.2.3
build     x86_64 native, --platform linux/amd64, aarch64-linux-gnu-gcc 13.3.0
time      4 min (Image + dtbs), 5 min (modules)
type      Linux kernel ARM64 boot executable Image, little-endian, 4K pages
```

## Rootfs

```
base       ubuntu:24.04 aarch64 (emulated — required: apt/dpkg must be the target's binaries)
packages   139, no recommends, no desktop
contents   systemd, openssh-server, iproute2, kmod, dbus, libstdc++6, libgcc-s1
exported   docker export of the container filesystem
entries    4811
```

`docker export` was necessary because apt installs into the container's own filesystem, not a
bind-mounted directory. The flattened export **is** the rootfs.

## P0 modules (all present)

| Module | Purpose |
|---|---|
| `ath10k_snoc.ko` | WCN3990 Wi-Fi |
| `ath10k_core.ko`, `ath10k_pci.ko` | ath10k core / PCI |
| `mac80211.ko`, `cfg80211.ko` | wireless stack |
| `qcom_q6v5_wcss.ko` | Wi-Fi/BT co-processor remoteproc |
| `qcom-spmi-adc-tm5.ko` | **the thermal monitor the board's zones bind to** |
| `qcom-spmi-adc5.ko`, `qcom-vadc-common.ko` | PMIC ADC |
| `qcom-spmi-temp-alarm.ko` | PMIC temperature alarm |

### A naming trap worth recording

An initial check reported the entire thermal stack **ABSENT**. It was wrong: the check grepped
for `qcom_spmi_adc5` (underscores, matching `CONFIG_QCOM_SPMI_ADC5`) while the driver files use
**hyphens** (`qcom-spmi-adc5.ko`). The DT confirms the binding:
`adc-tm@3500 { compatible = "qcom,spmi-adc-tm5"; }`.

**Lesson: verify module names against the actual driver source or `.config`, never against a
guessed spelling.** The build script now checks both conventions and cross-references the live
`.config` so a wrong name cannot hide a gap.

## Verified source properties

**Sustained-read lockup fix — PRESENT.**

```dts
reserved_mem1: memory@46200000 {
    reg = <0x0 0x46200000 0x0 0x4900000>;   /* 73 MiB — the FIXED value */
    no-map;
};
```

The unfixed value was `0x2d00000` (45 MiB), leaving 28 MiB of firmware-owned DRAM in the page
allocator and causing hard lockups roughly once per 59 MiB of buffered read. The MCP engine is
exactly that workload. **Closed.**

**Thermal — present, and now known to be CPU-blind.**

Three zones on the PMIC ADC monitor, each `critical` at 125 °C, **no passive trip**:

```
rf-pa0-thermal  -> pm6125_adc_tm 0   critical @ 125000
quiet-thermal   -> pm6125_adc_tm 1   critical @ 125000
xo-thermal      -> pm6125_adc_tm 2   critical @ 125000
```

No TSENS die sensing in this tree, therefore **no CPU thermal throttling** —
`CONFIG_THERMAL_GOV_STEP_WISE=y` has no passive trips to govern. The sensors watch an RF power
amplifier, a quiet rail and a crystal oscillator, not the CPU cores. The Aug-2026 TSENS series
(tested by its author on this exact board: 15 die zones, 85 °C passive trip) is **not** in
`v7.2.3-r0`.

**For an unattended always-on hub this is a real limitation**, recorded as such.

## Reproducing

```bash
native-linux/packaging/build_kernel_native.sh          # Image + dtbs + modules
# rootfs: see the container command in deploy/logs/rootfs.log
```

Four rules the build script encodes, each from a failed attempt:
pin `--platform`; never `--rm` a build you may debug; `mrproper` any tree previously built for
another architecture; build modules, not just `Image`.

## NOT built / NOT claimed

| Item | State |
|---|---|
| Boot image (`boot.img`, A/B header, `vbmeta`) | **NOT BUILT** — needs `mkbootimg` with the device's exact header |
| Flashable image | **NOT BUILT** |
| Device boot | **NEVER ATTEMPTED** |
| Fastboot stability | **UNPROVEN** — needs a bootloader session |
| `anti` (anti-rollback) value | **UNREAD** — decides whether rollback is possible |
| Unlock mechanism | **UNVERIFIED** — sources conflict |

**This is a kernel, a rootfs and a service definition. It is not a bootable image**, and must
not be described as one until it boots on the physical device.

## Device state — untouched

```
Android 11 (SDK 30)   ro.build.display.id = qssi-user 11 RKQ1.200903.002 V12.0.26.0.RFQMIXM
bootloader            LOCKED (ro.boot.flash.locked=1)
verified boot         green
OEM unlocking         ENABLED by owner (sys.oem_unlock_allowed=1)
```

No flash, unlock, erase, format, downgrade or partition change. Recovery package verified but
never flashed. 941-file user backup intact.
