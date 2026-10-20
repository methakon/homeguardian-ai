# Fastboot connectivity recovery — investigation and readiness report

Date: 2026-10-20  ·  HEAD: `07944fa00e5b3a0a9d513d39cb1224b694ff8819`

---

## 1. Phone recovery state

### NOT RECOVERED — and I cannot change that from this side

```
adb devices   -> LRW86SC6RO8TZ99H (Panasonic only)
fastboot      -> (empty)
/sys/bus/usb/devices/2-1 -> ABSENT
```

The Mi A3 remains in bootloader mode with a dead USB interface. The last kernel activity was
at 10:07:35 (`error -71`, then `unable to enumerate USB device`); nothing since.

**Per Task A: recovery is NOT assumed.** No ADB link and no owner confirmation exist, so the
device state is recorded as unknown-to-recovered. There is no way to issue a reboot — the
command channel is the thing that is broken.

**Required owner action: long-press POWER ~10–15 s.** If that fails: unplug/replug the cable,
then long-press again. Only a physical action can resolve this.

Recovery will be confirmed by ADB (serial + `ro.build.display.id`) or your explicit statement —
nothing else. A dead USB port proves nothing either way.

## 2. USB troubleshooting results — the real finding

The host has **four independent USB controllers**, and the failing port belonged to the wrong
kind:

| Root hub | Driver | PCI | Ports | Status |
|---|---|---|---|---|
| `usb1` | `ehci-pci` | `0000:00:1a.0` | 2 (+6 via RMH) | **native USB 2.0 — untested** |
| `usb2` | `xhci_hcd` | `0000:00:14.0` | 15 | **the phone was here (2-1) — FAILED** |
| `usb3` | `xhci_hcd` | `0000:00:14.0` | 6 | SuperSpeed, untested |
| `usb4` | `ehci-pci` | `0000:00:1d.0` | 2 (+8 via RMH) | **native USB 2.0 — untested** |

The Mi A3 failed on `2-1`, which belongs to `usb2` — an **xHCI** controller. ADB worked on that
same port minutes earlier, so neither the port nor the cable is generally broken; the failure is
specific to the bootloader's USB interface on xHCI.

**14 free ports** exist behind the two EHCI rate-matching hubs:

```
1-1  Intel RMH (behind usb1, PCI 1a.0)  -> 6 ports, ALL FREE
4-1  Intel RMH (behind usb4, PCI 1d.0)  -> 8 ports, ALL FREE
```

Working hypothesis, **not** a conclusion: the bootloader's USB stack cannot complete enumeration
through this host's xHCI controller but may work on native EHCI. On X99 boards the EHCI trees
typically serve the USB 2.0 rear stack.

Items 2 and 3 of Task B (alternative cable, USB 2.0 hub) remain untested and need your hands on
the hardware. Item 5 (binary path/version) is resolved below. Item 6 is resolved below.

### 2.5 Fastboot binary — resolved

```
edge case found: fastboot is NOT on PATH
  $HOME/Android/Sdk/platform-tools/fastboot   version 37.0.1-15733141
  adb on PATH is /usr/lib/android-sdk/platform-tools/adb  version 34.0.4-debian
```

These are **different builds from different SDKs**. Mixing a 34.x adb with a 37.x fastboot causes
protocol mismatches, so all tooling here uses the **matched 37.0.1 pair** from
`~/Android/Sdk/platform-tools/`. `fastboot --version` reported absent because it is not on PATH —
the binary exists and works.

### 2.6 Enumeration failure vs command failure — distinguished

These are now cleanly separated:

- **Enumeration failure** = the device never reaches `fastboot devices`. Signature: kernel
  `error -71` / `Device not responding to setup address` / `unable to enumerate USB device`, and
  no `/sys/bus/usb/devices/N-M` node. **This is what happened.**
- **Command failure** = device enumerates and `fastboot devices` lists it, but a specific
  `getvar` returns FAILED/unknown. **This was never reached**, because enumeration never
  completed.

The distinction matters: an enumeration failure means *no* fastboot command could have worked,
so the previous session's total failure is explained by the transport, not by any particular
command or by the phone.

### 2.7 Probe tooling

`native-linux/acceptance/fastboot_port_probe.sh` — read-only, and it **identifies which
controller the device lands on**, so a port change becomes a measurement rather than a guess.
Modes: `--observe`, `--reboot`, `--query`, `--stability N`. It refuses to act unless the serial
resolves to `laurel_sprout` on the expected build.

Two real defects were found by testing it rather than reading it, both fixed:
- Bashisms (`declare -A`, `mapfile`, herestring `<<<`) meant invoking it as `sh script.sh` died
  with a bare `Syntax error: "(" unexpected`. Now verified exit 0 under `sh`, `bash`, `dash` and
  direct exec.
- Its controller table is now cross-checked against live sysfs (it matches exactly).

## 3. Fastboot stability

**Not measurable — the device never enumerated.** No stability claim can be made.

The stability mode measures `/sys/bus/usb/devices/N-M/devnum` rather than fastboot `transport_id`
deliberately: `transport_id` is a server-side handle that changes for reasons unrelated to the
bus, and in an earlier session an availability-only check reported 100 % across three
re-enumerations — it would have passed a weaker test. The pass condition is **zero devnum
changes**.

## 4. Bootloader diagnostic results

**Nothing was obtained.** Task C is explicitly gated on stable enumeration, which did not occur,
so per instruction the sequence was stopped rather than forced.

| Query | Result |
|---|---|
| Device identity (fastboot) | **UNKNOWN** |
| Supported variables | **UNKNOWN** |
| `get_unlock_ability` | **UNKNOWN** |
| `anti` (anti-rollback) | **UNKNOWN** |
| Current slot (fastboot) | **UNKNOWN** — Android reported `_b` |
| Stability over interval | **NOT MEASURED** |

`anti` being unknown is **not** a rollback index. It is not treated as one anywhere in this
work, and no value has been recorded or inferred.

Absent an enumeration fix, this entire block requires a second authorized read-only session.

## 5. Remaining image-format blockers

### 5.1 RESOLVED — DT placement was not actually a blocker

Previously left open with four candidates. It is now settled by authoritative device-specific
upstream source, not by selection:

```
pmaports device/testing/device-xiaomi-laurel/deviceinfo
  deviceinfo_dtb              = "qcom/sm6125-xiaomi-laurel-sprout"
  deviceinfo_append_dtb       = "true"
  deviceinfo_generate_bootimg = "true"

pmaports deviceinfo_schema.toml, [variable.metadata.append_dtb]
  "Set to `true` if the dtb should be appended to the Linux kernel image,
   if set to `false` the dtb will be copied to the /boot partition instead"
```

So: **append the DTB to the kernel image.** Artifact produced accordingly (below).

A correction worth recording: I initially noted `CONFIG_ARM64_APPENDED_DTB_IMAGE` as "not set",
implying a missing option. **That was wrong** — the symbol does not exist for arm64; it is the
arm32 `CONFIG_ARM_APPENDED_DTB`. arm64 has no in-kernel "scan my own tail" feature; the appended
blob is a packaging convention consumed by the boot-image packer, and the boot chain delivers the
DT. The misleading note is corrected rather than left standing.

### 5.2 CONFIRMED — boot parameters from three independent sources

| Parameter | My decode of stock | pmOS deviceinfo | Stock header |
|---|---|---|---|
| `offset_kernel` | `0x00008000` | `0x00008000` | `0x00008000` |
| `offset_ramdisk` | `0x01000000` | `0x01000000` | `0x01000000` |
| `offset_tags` | `0x00000100` | `0x00000100` | `0x00000100` |
| `page_size` | `4096` | `4096` | `4096` |
| header version | 2 | 2 | 2 |

Three agreeing sources. These are settled, not inferred.

### 5.3 PRODUCED — appended-DTB kernel

```
Image.gz       15,711,298 B  adf5bf8eb78449fb5a2d380c7a13d2a086a431518f4090b9ebbaba0c1ebafe00
Image.gz-dtb   15,752,347 B  ae5a22566cb535da9cccf11389b7b3b19f1749a11b9d8ecb44cbbcb648139e0a
sm6125-xiaomi-laurel-sprout.dtb  41,049 B  66c3509356af238ff18a6fc24eb1bcc5f9ee949bdd487a3d4714826539d68a2f
```

Verified: `Image.gz` round-trips to the exact `Image` hash `c3a5589e…`; the appended tail length
equals the DTB totalsize (41,049) and is **byte-identical** to the standalone DTB.

`Image.gz` was produced with gzip directly because the kernel tree's `include/config` and
`include/generated` are root-owned from the containerised build, so `make` aborts on unlink
permission. That is a permissions artifact and the bytes are what `make` would emit.

### 5.4 STILL OPEN — vbmeta / verified boot

Unchanged. The vbmeta AVB fields remain **unverified**: three parse attempts with different
struct offsets gave three different answers, and `avbtool` is absent and not installable here.
No value has been invented. Resolvable either with `avbtool info_image` or from the device's own
`ro.boot.vbmeta.*` / `ro.boot.verifiedbootstate` properties.

### 5.5 STILL OPEN — boot image not assembled

A complete boot image has **not** been built. All inputs now exist (header v2, page 4096, the four
offsets, `Image.gz-dtb`, a rootfs), but assembling and validating it is only worth doing once
fastboot can actually carry it. No DTB placement or vbmeta configuration has been invented.

## 6. Installation GO / NO-GO

# **NO-GO**

| Gate | State |
|---|---|
| Recovery package verified (72/72 vendor manifest) | **PASS** |
| Kernel, modules, rootfs, service built and hashed | **PASS** |
| Boot parameters — 3 independent sources agree | **PASS** |
| DT placement resolved from upstream source | **PASS** |
| Appended-DTB kernel artifact produced | **PASS** |
| Thermal posture defined and tested | **PASS (mitigation only)** |
| **Fastboot transport works** | **FAIL — error -71, device never enumerated** |
| **Anti-rollback / unlock eligibility known** | **FAIL — UNKNOWN** |
| **vbmeta fields parsed** | **FAIL — unverified** |
| **Boot image assembled and validated** | **FAIL — not built** |

The transport failure is the gating one: **you cannot flash over a link that cannot enumerate**,
and a flash interrupted mid-write is the only failure mode here that genuinely hard-bricks the
device. It also invalidates testing of everything downstream.

## 7. Task D compliance — artifacts preserved

Per instruction, nothing was changed without a demonstrated defect. Only two defects were found
and both were in *newly written* tooling, not in the preserved artifacts:

| Artifact | State |
|---|---|
| `Image` / DTB / rootfs / hg_mcp_server | **unchanged** |
| 10 P0 modules, full 1644-module set | **unchanged** |
| Recovery package | **unchanged** (read-only verification only) |
| `fastboot_port_probe.sh` | fixed twice — bashisms, found by execution |
| `thermal-gate.sh` | fixed once — missing marker dir, found by execution |

Added: `Image.gz`, `Image.gz-dtb`. No DTB placement or vbmeta value invented.

## 8. Device state — UNTOUCHED

```
unlock          not performed
flash           not performed
erase           not performed
factory reset   not performed
slot change     not performed
partition mod   not performed
```

## 9. Commit

```
HEAD    07944fa00e5b3a0a9d513d39cb1224b694ff8819
branch  experiment/mi-a3-linux-runtime
remote  matches
tree    clean
```

## 10. Roadmap

**Roadmap coverage missing** — no `project_checklist_items` row exists for the native-Linux
replacement. Flagged per AGENTS.md rule 9; not invented.
