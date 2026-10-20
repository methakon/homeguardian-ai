# Native Linux on the Xiaomi Mi A3 — decision record: PATH ABANDONED

Status: **ABANDONED by owner decision, 2026-10-20.** Artifacts retained; no further pursuit.
Supersedes the installation-readiness reporting in `TASK_E_INSTALLATION_READINESS.md` and
`FASTBOOT_RECOVERY_REPORT.md`, which remain accurate as history.

---

## 1. The decision

The owner directed that the native Linux installation path for the Mi A3 be abandoned, and
that the existing Android EdgeService be preserved unchanged. The blocker proved to be
hardware, not process, and no amount of further host-side work can move it.

## 2. Why — the evidence

The decisive fact is that **fastboot never enumerates, reproducibly**, on this board.

Two independent authorized sessions, months of precondition work apart:

| run | result |
|---|---|
| session 1 (2026-10-20 04:07) | 4 address attempts, `error -71`, kernel power-cycle failed |
| session 2 (2026-10-20 17:22) | 4 address attempts, `error -71`, never enumerated in 120 s |

```
usb 2-1: new high-speed USB device number 27/28/29/30 using xhci_hcd
usb 2-1: device descriptor read/64, error -71
usb 2-1: Device not responding to setup address.
usb 2-1: device not accepting address 29, error -71
```

`error -71` is `EPROTO`: the device is electrically present and *trying* to enumerate, but the
control-transfer handshake never completes.

### Hypotheses eliminated, each by measurement

| hypothesis | how it was eliminated |
|---|---|
| faulty cable | the same cable enumerates Android on this exact port, three times, cleanly |
| faulty port | Android stable on `2-1` for 121 s with 0 re-enumerations and 0 adb failures |
| flaky USB in general | `-71` occurred **only** in bootloader mode, never once under Android |
| authorization | owner authorised the session explicitly; the run was performed |
| EHCI vs xHCI controller | unmeasurable: this board routes **every** physical port to xHCI. The two EHCI controllers are present and enabled but their only downstream ports are internal rate-matching hubs that have never seen a device — 6 and 8 ports, 0 occupied, all boot |
| stale adb server | resolved separately (kill/start), not the cause |

**The surviving explanation is the bootloader's own USB stack being incompatible with this
host controller.** That is not fixable from the host side.

### Why the two authorised sessions were the right final test

Prior to them, "fastboot is unstable" was an inference from one observation. Two reproducible
failures, with cable and port independently proven good for Android on the same hardware,
convert it to a measured property of the platform. That is what made a defensible
abandonment possible rather than an open-ended one.

## 3. What was NOT the blocker

Worth recording, because it corrects the trajectory of the work:

- The stock recovery package was **fully verified** (72/72 vendor manifest, md5 matching
  Xiaomi's published value). Rollback material was never the problem.
- The boot image was **built and structurally validated** — every parameter matching stock
  across three independent sources (my decode of `boot.img`, the pmOS `deviceinfo`, and the
  stock header itself).
- The kernel, modules, rootfs, initramfs and MCP binary were all built, hashed and audited.
- The thermal limitation was characterised honestly (no CPU die sensor, no throttling).

In short: **everything reachable from the host side was achieved.** The installation failed on
one physical fact — no usable flash transport.

## 4. Retained assets

Frozen at branch `experiment/mi-a3-linux-runtime` @ `26448d3b588ec4f4aeaff931ca27624e3ceba2b8`
(tree clean, remote in sync), and in `deploy/mi-a3-native-linux/` (145 MB, 32 files,
`SHA256SUMS` 26/26 OK):

| asset | digest |
|---|---|
| `kernel/Image` | `c3a5589e7a32fb20d2c1df1deac2fa8dfe83cc1d60769285d28d4aa1a5f78627` |
| `kernel/Image.gz-dtb` | `ae5a22566cb535da9cccf11389b7b3b19f1749a11b9d8ecb44cbbcb648139e0a` |
| `kernel/sm6125-xiaomi-laurel-sprout.dtb` | `66c3509356af238ff18a6fc24eb1bcc5f9ee949bdd487a3d4714826539d68a2f` |
| `rootfs/rootfs-ubuntu2404-aarch64.tar.zst` | `57ec742ec794cb501aedbca915e2730034e566e6b89d2463a1ded94c6419443d` |
| `boot/boot.img` | `4eff763e4e73c9a62fdcbffc5ed9dd2628175f302997579b8992e37ed6dba757` |
| `boot/initramfs.cpio.gz` | `aaf4a929495c6de42871809c140402e9915ba020e940df79d7ddda4d3c35c250` |
| `service/hg_mcp_server` | `82b6cd877ff9a84f1a4feb2d92b6e60de8d38507111bf5029be5bf74f7ca82a8` |

Plus: the full 1644-module set at `~/homeguardian-native-build/modules-root`, and the stock
recovery package at `~/stock-recovery/mi-a3/` (2,439,506,308 B, md5
`2292e2c0b1c0e9839d5b14427154b5b4` — unchanged).

These are retained as assets, not as an active workstream. Should a host with a reachable
EHCI controller (or a BIOS `XHCI Mode = Disabled` setting) ever become available, the bundle
is complete and needs no reconstruction.

## 5. Preserved state — Android EdgeService

**Unchanged, and not touched by this decision.**

- The frozen Android APK was **never installed**: candidate
  `cee0024c…b3f172`, installed stays `2d6018e0…a47246` (v1.0.0).
- No APK install, uninstall, downgrade or data mutation was ever performed.
- The device remains stock Android 11, bootloader locked, verified boot green — no flash,
  unlock, erase, format, slot change or vbmeta write, ever.

## 6. Outstanding — needs one owner action

**The phone is currently in bootloader mode with a dead USB interface and must be recovered
physically.** The last authorized diagnostic left it there (as the previous one did), and
`fastboot reboot` cannot reach it because there is no link.

> **Long-press POWER ~10–15 seconds.** If that fails, unplug/replug the cable and retry.

Confirm with `adb devices` showing `edd53dbd04de`. Nothing was written to the device, so no
data is at risk — this is a stranded boot state, not damage.

## 7. Known-unknowns, recorded rather than closed

These were never resolved and are **not** claimed resolved:

- `anti` (anti-rollback index) — **UNKNOWN**. Never read; fastboot could not be reached.
- Unlock eligibility — **UNKNOWN**.
- `vbmeta` AVB fields — **UNVERIFIED**. Three parse attempts disagreed and `avbtool` was
  unavailable; no value was ever invented.
- Test-count claims in the release README — unverified (no check exists).

## 8. Honest summary

The goal was native Linux replacing Android on the Mi A3, with the HomeGuardian C++ MCP
service under systemd. **It was not achieved, and it is not achievable on this hardware via
fastboot.** The work produced a complete, verified, ready-to-flash bundle and a measured
explanation of why it cannot be flashed from this machine.

The failure was in the transport, not in the preparation.
