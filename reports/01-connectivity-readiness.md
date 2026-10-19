# reports/01-connectivity-readiness.md

**Status: BLOCKED** — wireless ADB is not running, and USB remains physically unstable.
Phase 1 finding: the Mi A3 is reachable in bursts but cannot be relied on as a transport.

Collected 2026-10-19 on branch `experiment/mi-a3-linux-runtime`. Read-only: no installation, no
partition, bootloader, firmware, recovery or unrelated system-setting changes. The Panasonic CCTV
handset was not touched.

---

## 1. Device identification — PASS

Verified by four independent attributes, not by a single string:

```
manufacturer ....... Xiaomi
model .............. Mi A3
device codename .... laurel_sprout
Android ............ 11
API ................ 30
build fingerprint .. Xiaomi/laurel_sprout/laurel_sprout:11/RKQ1.200903.002/V12.0.26.0.RFQMIXM:user/release-keys
board .............. trinket        (SM6125)
hardware ........... qcom
ABI ................ arm64-v8a
```

A second handset is attached (`0e8d` MediaTek, model `ELUGA_I7_EE`) — the Panasonic ELUGA CCTV node.
It is **distinct** from the target by vendor ID, product string, codename and Android version, and it
is excluded from every command by explicit transport selection. No command in this phase was issued
without `-s <mi-a3-transport>`.

## 2. Wireless debugging — BLOCKED

```
adb_wifi_enabled (global setting) ... 1        (enabled as a setting)
mDNS-advertised adb services ........ none
Wi-Fi ............................... enabled  (wlan0)
wireless debugging port on device ... none observed
```

The setting reads enabled, but **wireless debugging is not actually running**: no mDNS advertisement
and no listening debug port. Android 11 starts the service only through the on-device UI toggle; a
settings write alone does not launch it. This is the same result as the earlier evaluation.

**Owner steps required (about a minute):**

1. Settings → System → **Developer options** → **Wireless debugging** → toggle **ON**
   (if Developer options is hidden: Settings → About phone → tap **Build number** 7×)
2. Tap **"Pair device with pairing code"**
3. Provide, here in chat only — never in GitHub, a log, or committed text:
   * the **6-digit pairing code**
   * the **pairing port**
4. Confirm the IP shown is the phone's Wi-Fi address on the trusted home network

I will then use authenticated `adb pair` (Android 11 pairing), then `adb connect` to the separate
debug port, then observe **five minutes** of stable wireless transport before any installation or
deployment action. Legacy `adb tcpip` is explicitly not used.

## 3. USB — UNSTABLE, and not used as a prerequisite

The mission instructs that unstable USB must not gate Linux evaluation. Measured today:

```
kernel ......... usb 2-8: can't set config #1, error -71
churn .......... 334 enumerations / 185 disconnects on the target port
mid-command .... transport dropped between two consecutive read-only queries
                (identity queries succeeded; the very next batch returned empty)
```

`error -71` is a USB configuration failure — the device announces itself and the host cannot bring it
up. Recovery required retry-per-command (up to 5 attempts) to collect even a partial inventory. The
transport disappeared and reappeared repeatedly within minutes.

**Assessment:** the physical link is the fault, not the software. A different cable and a direct
workstation port remain the real fix, but per the mission I am **not** making USB a prerequisite —
the operator-executed Termux procedure below is the intended path instead.

## 4. Operator-executed Termux procedure (no USB required)

Because wireless ADB is blocked and USB is unstable, Phase 3 is prepared as a **manual, on-device
procedure** the owner runs directly on the phone, exactly as the mission directs. It needs no PC, no
USB and no ADB at any point.

```
# 1. Install Termux — from F-DROID, not the Play Store (the Play build is deprecated
#    and cannot install packages).  https://f-droid.org/packages/com.termux/

# 2. In Termux:
pkg update && pkg upgrade -y
pkg install -y proot-distro

# 3. Confirm the tool and the available image name BEFORE installing anything:
proot-distro list

# 4. Install Ubuntu 24.04 arm64 (CLI only — no desktop, no VNC, no Kali tools):
proot-distro install ubuntu:24.04

# 5. Enter it:
proot-distro login ubuntu
```

Inside Ubuntu, verify architecture and provenance before trusting the rootfs:

```
uname -m                       # expect: aarch64
cat /etc/os-release            # expect: Ubuntu 24.04 LTS
```

Then report back the output of `uname -m`, `cat /etc/os-release`, `df -h /` and `proot-distro list`.

**Deliberately not included:** no desktop environment, no VNC server, no Kali tool collections, no
binds of shared phone storage, no root, no partition changes. All state stays in Termux's
application-private directory.

## 5. Blocked / unknown

| Item | Status |
|---|---|
| Wireless ADB pairing | **BLOCKED** — needs the on-device UI toggle + pairing code from the owner |
| Five-minute wireless stability window | **BLOCKED** — cannot start without a wireless transport |
| Termux installation | **BLOCKED** — Termux is not installed; requires an owner-side APK install |
| Ubuntu PRoot rootfs | **BLOCKED** — depends on Termux |
| USB-dependent automation | **NOT ATTEMPTED BY DESIGN** — mission prohibits gating on it |
| Stable device investigation (Phase 2 depth) | **PARTIAL** — collected via retries; camera/sensor enumeration incomplete |

## 6. Safety upheld

No partition, bootloader, firmware or recovery modification. No Android system setting changed except
the previously-written `adb_wifi_enabled=1`, which did not start the service and is left for the UI
toggle to complete. Stock Android 11 remains installed; the Edge Service v1.0.0 and its private data
are untouched; the v1.0.3 APK and all rollback artifacts are intact with verified hashes; the
Panasonic handset was not disturbed. No pairing codes, ADB keys or raw device serials are recorded
here or anywhere in this repository.
