# Boot artifacts — architecture audit

Why this exists: a wrong-architecture binary inside an initramfs fails silently at boot with no
useful message. Every binary was therefore checked rather than assumed.

## initramfs.cpio.gz (extracted and inspected, 64 entries)

```
bin/busybox   ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV),
              static-pie linked, stripped
              dynamic deps: 0        <- essential: an initramfs has no libc
lib/modules/7.2.3/...  all 10 modules: ELF 64-bit LSB relocatable, ARM aarch64
init          #!/bin/busybox sh   mode 755      <- non-executable /init = silent boot hang
x86-64 ELF found anywhere: 0
```

## Kernel payload

```
Image.gz-dtb : 15,752,347 B
  decompressed kernel : 51,534,336 B   (identical to the built Image)
  ARM\x64 magic at 0x38 : b'ARMd'      -> CONFIRMED arm64 Image, not a stray format
  code0 @ 0x00 : 0xfa405a4d
```

## Result

CLEAN — every executable is aarch64, the busybox is static, and the kernel payload is a genuine
arm64 Image of the expected size.

This audit does NOT establish that the image boots. It has never been flashed or booted.
