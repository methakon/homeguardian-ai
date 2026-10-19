# Kernel build record — Mi A3 (laurel_sprout)

Produced on the workstation without root and without pmbootstrap.

## Source (pinned)

```
repo    https://codeberg.org/sm61x5-mainline/linux
tag     v7.2.3-r0
commit  5a88d98fa3d282ec4155a91577da86229d6b33d1
tree    arch/arm64/boot/dts/qcom/sm6125-xiaomi-laurel-sprout.dts
```

## Toolchain

```
build host        x86_64 (native — NOT emulated)
container         ubuntu:24.04, --platform linux/amd64
cross compiler    aarch64-linux-gnu-gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
kernel release    7.2.3
```

## Artifacts

| Artifact | Size | SHA-256 |
|---|---|---|
| `arch/arm64/boot/Image` | 51,534,336 B | `c3a5589e7a32fb20d2c1df1deac2fa8dfe83cc1d60769285d28d4aa1a5f78627` |
| `arch/arm64/boot/dts/qcom/sm6125-xiaomi-laurel-sprout.dtb` | 41,049 B | `66c3509356af238ff18a6fc24eb1bcc5f9ee949bdd487a3d4714826539d68a2f` |

```
Image type: Linux kernel ARM64 boot executable Image, little-endian, 4K pages
build time: 4 min (Image + dtbs, native)
```

The DTB hash is identical whether built natively or under emulation — the device tree is a
deterministic data artifact, so that cross-check is meaningful.

## Verified in-source properties

**Sustained-read lockup fix — PRESENT.** The board DTS reserves firmware-owned DRAM at the
corrected size:

```dts
reserved_mem1: memory@46200000 {
    reg = <0x0 0x46200000 0x0 0x4900000>;   /* 73 MiB — the FIXED value */
    no-map;
};
```

The unfixed value was `0x2d00000` (45 MiB), which left 28 MiB of firmware-owned DRAM in the
page allocator and caused hard lockups roughly once per 59 MiB of buffered read. Our MCP
engine is exactly that workload, so this was the highest-risk item. **Closed.**

**Thermal — present but CPU-blind.** The board defines three zones, all on the PMIC ADC
thermal monitor, all with a `critical` trip at 125 °C and no passive trip:

```
rf-pa0-thermal   -> pm6125_adc_tm 0   critical @ 125000
quiet-thermal    -> pm6125_adc_tm 1   critical @ 125000
xo-thermal       -> pm6125_adc_tm 2   critical @ 125000
```

There is **no TSENS die temperature** in this tree (`sm6125.dtsi` defines none), therefore
**no CPU thermal throttling** — `CONFIG_THERMAL_GOV_STEP_WISE=y` has no passive trips to
govern. The sensors watch an RF power amplifier, a "quiet" rail and a crystal oscillator, not
the CPU.

The Aug-2026 TSENS patch series (`patchew.org/linux/20260821030325.1823829-1-admin@mswin.me`),
whose author tested it on this exact board and measured 15 die zones with an 85 °C passive
trip, is **not** in `v7.2.3-r0`.

## Rules this build encodes

Four, each learned from a failed attempt:

1. **Pin `--platform linux/amd64`.** Without it a cached arm64 image is reused and the entire
   build runs under qemu at ~10–20× slower. Measured: 1439 objects in 22 min versus 4475
   objects in 4 min.
2. **Never `--rm` a build you might need to debug.** The first failure deleted its own logs.
3. **`mrproper` any tree previously built for another architecture.** An arm64 build compiles
   the kernel's *host* tools as aarch64 ELF; on x86-64 they cannot execute, and even
   `make defconfig` fails with `aarch64-binfmt-P: Could not open '/lib/ld-linux-aarch64.so.1'`.
4. **Build modules, not just Image.** Wi-Fi (`ath10k_snoc`), the PMIC ADC and the temperature
   alarm are all `=m`. An Image alone has no Wi-Fi and no thermal sensing.

## Not claimed

This is a **kernel**, not a bootable image. There is no root filesystem, no boot image, no
verified flash layout, and it has never been booted on the device. Per the standing rule it
must not be described as device-bootable until it boots on the physical phone.
