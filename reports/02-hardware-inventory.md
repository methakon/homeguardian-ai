# reports/02-hardware-inventory.md

Read-only inventory of the Xiaomi Mi A3 (`laurel_sprout`, Android 11 / API 30), collected via
`getprop`, `dumpsys`, `/proc` and `/sys` on branch `experiment/mi-a3-linux-runtime`.

**Collection was partial.** The USB link dropped mid-command repeatedly; values below were obtained
with retry-per-command (up to 5 attempts each). Anything not obtained is marked *not collected*
rather than guessed. No partitions dumped, no private user files read, no credentials collected.

---

## 1. SoC, CPU and memory — collected

```
manufacturer ....... Xiaomi
model .............. Mi A3
codename ........... laurel_sprout
board .............. trinket            (Qualcomm SM6125, Snapdragon 665 class)
hardware ........... qcom
ABI ................ arm64-v8a
CPU cores .......... 8
MemTotal ........... 5,738,336 kB  (~5.47 GiB)
Android ............ 11        API 30
build fingerprint .. Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys
```

`ro.soc.manufacturer` / `ro.soc.model` are **empty** on this build (those properties arrived in later
Android versions); `ro.board.platform=trinket` plus `ro.hardware=qcom` identify it. Snapdragon 665 is
a 4×Cortex-A73 + 4×Cortex-A53 part with Adreno 610.

**Relevance to the mission:** 8 cores and ~5.5 GiB is ample for a PRoot Ubuntu plus the MCP engine —
the engine itself measures ~7 MB RSS (workstation). The constraint will be persistence, not capacity.

## 2. Storage — collected

```
/data/user/0 ... 108G total, 21G used, 87G available (20% used)
```

Ubuntu 24.04 minimal under PRoot-Distro is expected to consume on the order of a few hundred MB;
87 GB free is not a constraint. Termux keeps all state in application-private storage, so no shared
phone storage is bound.

## 3. Power, thermal and battery — collected

```
battery level ...... 98%
status ............. 2 (charging)
temperature ........ 36.0 °C
thermal zones ...... 62 entries under /sys/class/thermal
USB powered ........ yes
```

62 thermal zones is normal for Qualcomm platforms. Charging while evaluating is the right condition:
an emulated PRoot workload is CPU-heavy and will consume battery quickly otherwise.

## 4. Display and input — collected

```
screen resolution .. 720x1560
density ............ 320
display state ...... mWakefulness=Dozing   (screen was off during collection)
input devices ...... /sys/class/input: event0..event5 (6 event nodes), input0..input5
```

The six input event nodes are consistent with touchscreen, the power key, the volume rocker,
plus motion and proximity/light sensors. Individual node names were **not** resolved (the second
half of that query returned empty on the flapping link), so physical-key mapping is **not verified**
and is listed as uncollected.

## 5. Connectivity — collected

```
Wi-Fi .............. enabled, interface wlan0
Bluetooth .......... not collected (ro.bluetooth.name returned empty)
USB state .......... sys.usb.state = adb
USB fault .......... can't set config #1, error -71; 334 enumerations / 185 disconnects
```

## 6. Camera and microphone — NOT COLLECTED

```
dumpsys media.camera ......... returned 0 matches for the probe pattern
sensor service handles ....... 3 matched (partial enumeration)
```

Camera and audio HAL enumeration did **not** produce usable output. `dumpsys media.camera` is not a
reliable surface on this MIUI build, and the transport flapped during the sensor query. **No claim is
made about camera or microphone availability from this inventory.** What *is* independently known
from earlier sessions on this device family: the Android app layer holds granted `CAMERA` and
`RECORD_AUDIO` permissions on the Panasonic node, and the Edge Service on the Mi A3 does not use them.

## 7. The distinction the mission asks for: Android HAL ≠ native Linux driver

This is the single most important note in this file, and it governs Phases 3–4:

**Everything enumerated above is an ANDROID abstraction.** `dumpsys`, `getprop`, `/sys/class/thermal`
and the sensor service expose devices through Android's HAL and Binder layers. Presence in those
layers says **nothing** about whether a native Linux userspace can drive the same hardware.

Concretely, for the Ubuntu-under-PRoot target:

| Device | Visible to Android | Reachable from PRoot Ubuntu? | Why |
|---|---|---|---|
| CPU / RAM | Yes | **Yes** | Syscalls pass through; proot only intercepts paths |
| Storage (app-private) | Yes | **Yes** | Ordinary file I/O |
| Wi-Fi (sockets) | Yes | **Yes** | Networking is a kernel facility, not a HAL |
| Thermal zones | Yes | **Partly** | `/sys` may be readable; access is SELinux-gated |
| Screen / touch | Yes | **No** | Needs Android's SurfaceFlinger/input stack |
| Power & volume keys | Yes | **No** | Delivered as Android input events |
| Accelerometer / gyro / light / proximity | Yes | **No** | Sensor HAL over Binder; no `/dev/iio` exposure to apps |
| Camera | Yes | **No** | Camera2 HAL; no V4L2 node usable by an app |
| Microphone | Yes | **No** | Audio HAL; no ALSA device usable by an app |
| Bluetooth | Yes | **No** | Android Bluetooth stack, not raw HCI to apps |

**Consequence:** PRoot Ubuntu can host the **C++ MCP engine** — which is pure CPU, memory, files and
sockets — and nothing else on this list. It cannot host sensor capture. This is why a small Android
component remains necessary for camera and microphone, and it is why the rootless-PRoot plan is
sound for the *control plane* but not for the *sensor plane*.

**Root access changes none of this.** The mission prohibits root, and even with it, the sensor and
camera HALs are Android services rather than plain device nodes — root would not turn them into
Linux drivers.

## 8. Uncollected — and why

| Item | Reason |
|---|---|
| Bluetooth capability | property returned empty; transport flapped |
| Individual input-node names / key mapping | partial query returned empty |
| Camera and audio HAL enumeration | `dumpsys media.camera` yielded no usable output |
| Full sensor list (accelerometer, gyro, light, proximity) | only 3 handles matched before the link dropped |
| Partition / bootloader / recovery state | **deliberately not collected** — prohibited and unnecessary |

Re-running this inventory would be worthwhile **over wireless ADB**, where the transport does not
drop between commands. The gaps are transport artefacts, not device absences.

## 9. Safety

No partitions dumped, no bootloader or firmware inspection, no recovery access, no private user files
read, no credentials collected. Stock Android 11 untouched, Edge Service v1.0.0 and its data intact,
v1.0.3 APK and rollback artifacts preserved with verified hashes, Panasonic handset undisturbed.
