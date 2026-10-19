# reports/04-device-cleanup-audit.md

Dedicated-device cleanup audit for the Xiaomi Mi A3 (`laurel_sprout`, Android 11 / API 30), answering
the six required deliverables. **No uninstallation, data wipe, bulk removal or factory reset was
performed** — this is audit and plan only, as Phase 4 requires.

Branch `experiment/mi-a3-linux-runtime`. Read-only collection: package **names** only, no app
databases, no credentials, no private data.

---

## 1. Essential versus unnecessary application categories

Collected with `pm list packages` (names only):

```
third-party (user-installed) ...... 20
disabled by user .................. 3   (com.android.nfc, com.google.android.ims, com.google.ar.lens)
system ............................ 245
```

**ESSENTIAL — leave untouched** (245 system packages). Android system, hardware interfaces,
networking, permission and security services. Per your instruction, no system component is to be
removed to make the device smaller, and nothing here is proposed for removal.

**REQUIRED — keep**

| Package | Why |
|---|---|
| `org.homeguardian.edgeservice` | the project's own Edge Service; its data is a documented dependency |
| `com.amazon.dee.app` | Alexa — HomeGuardian has an Alexa bridge (`hg_alexa_bridge` in the C++ tree), so this is a documented project dependency, not personal software |
| `com.wireguard.android` | matches the VPN/remote-MCP workstream; keep unless the owner says the VPN plan is abandoned |
| *(Termux)* | **not installed** — required for Phase D and must be added from F-Droid after any reset |

**OPTIONAL — development convenience, no runtime dependency**

| Package | Note |
|---|---|
| `com.openai.chatgpt` | useful for dev, not a HomeGuardian dependency |

**UNNECESSARY — no HomeGuardian dependency** (16 packages)

```
com.eterno                                    com.google.android.music
com.google.android.apps.magazines             com.google.android.contactkeys
com.google.android.apps.nbu.paisa.user         com.google.android.keep
com.google.android.apps.tachyon                com.google.android.videos
com.google.android.calculator                  com.mi.global.shop
com.google.android.safetycore                  com.radio.pocketfm
com.google.android.verifier                    com.truecaller
com.vigafun.funfinity.foodhunt                 com.whatsapp
```

Two notes of care: `com.google.android.safetycore` and `com.google.android.verifier` look like Google
infrastructure rather than personal apps, and `com.whatsapp` is personal data with no project
relevance but obvious owner value. **I do not propose removing any of them.**

## 2. User-file backup scope and verification

**Scope** (shared storage, metadata-only sizing — no file contents read to measure):

| Folder | Size | Files |
|---|---|---|
| DCIM | 1,598.8 MB | 99 |
| Pictures | 37.3 MB | 819 |
| Movies | 0.6 MB | 19 |
| Music | 0.0 MB | 1 |
| Documents | 0.0 MB | 0 |
| Download | 0.8 MB | 3 |
| **Total** | **~1.6 GB** | **941** |

`HomeGuardian` / `homeguardian` folders: **not present**. At `/sdcard` root there are small
`hg_dbg.xml`, `hg_ui.xml`, `d.*/s.*/w.*.xml` files — UI-dump artefacts from earlier sessions, not
owner recordings; I have not treated them as user files.

**Status: IN PROGRESS at the time of writing — not yet VERIFIED.** Copy is running to
`~/.homeguardian-userfiles-backup/mi-a3-<timestamp>` (mode 700, outside the repository, no cloud
destination). Partial progress at last check: DCIM 26 of 99 files, 184 MB. The run performs:
`adb pull -a` (preserves timestamps) → `sha256sum` manifest over every copied file → file counts per
folder. **A sample restore into a temporary workstation directory has not yet been executed**, so per
your instruction this must be reported as **PARTIAL**, not BACKUP VERIFIED, until copy integrity and
restore testing both succeed.

This is explicitly **not** a full phone recovery backup: no system partitions, firmware, bootloader,
recovery, app-private directories, contacts, messages, accounts, credentials, ADB or signing keys, or
SIM/device identifiers. Android's own shared-storage tree leaves out app-private data by design.

**Note on the 819-file `Pictures` folder versus 99 in `DCIM`:** Pictures is mostly small images
(screenshots and thumbnails), so the byte weight sits almost entirely in DCIM. A staged copy —
Pictures/Movies/Music/Documents/Download first, DCIM last — is the safer ordering on an unstable
transport, since a failure late in DCIM costs the least.

## 3. Factory-reset risks and prerequisites

**The dominant risk is Factory Reset Protection (FRP).** After a factory reset, Android 11 requires
the **previously-synced Google account** to activate the device. If that account is unavailable or its
credentials are lost, the device becomes **unusable** — a far worse outcome than any bloat it removes.
This is the single reason a reset must not be treated as routine.

| # | Prerequisite | Status |
|---|---|---|
| 1 | User-file backup completed and verified | **PARTIAL** — copy running, restore test not done |
| 2 | Owner can unlock and operate the device | assumed yes; **not verified in this audit** |
| 3 | Google account for post-reset activation available | **UNVERIFIED — hard blocker if not** |
| 4 | Trusted method to install Termux afterwards | available (F-Droid; APK can be side-loaded) |
| 5 | Wi-Fi connectivity and network access | yes — `wlan0` enabled, on the trusted home network |
| 6 | HomeGuardian app data loss understood | **understood, but it is a real loss**: `org.homeguardian.edgeservice` v1.0.0 and its provisioned credential/identity would be **destroyed**. The stored device identity at the coordinator would be orphaned and a new one would have to be provisioned |
| 7 | Ubuntu ARM64 deployment instructions and binary prepared | instructions yes; **glibc binary still building** |
| 8 | Battery sufficiently charged | yes — 98%, charging, 36.0 °C |
| 9 | Recovery does not depend on unreliable USB | **no** — recovery currently *does* depend on the flaky USB link. This is the second real blocker |

Two further risks worth naming: the reset **removes the working Edge Service**, which is the only
currently-functioning MCP surface on the device; and the USB fault (`error -71`) is **hardware**, so a
reset will not fix it, and post-reset setup would face the same unreliable link.

## 4. Minimal post-reset Android configuration

If a reset ever happens, the minimum viable set is: finish setup **without** signing into any optional
account, connect Wi-Fi, enable Developer options, install **Termux from F-Droid** (never the Play
build), optionally `com.wireguard.android` if the VPN plan resumes and `com.amazon.dee.app` only if the
Alexa bridge is being demonstrated — then follow report 01's Termux/PRoot procedure. Nothing else is
required for the HomeGuardian Linux runtime.

## 5. Ubuntu 24.04 ARM64 installation procedure

Already written and unchanged: `reports/01-connectivity-readiness.md` §4 — operator-executed, no USB,
no PC, CLI only, `proot-distro list` before install to confirm the image name and ARM64 architecture,
no desktop, no VNC, no Kali packages.

## 6. Verdict: **RESET NOT NECESSARY**

Recommended: **skip the factory reset.**

The reasoning, plainly:

* **It buys almost nothing.** The 16 unnecessary apps cost no project capability. Storage is 87 GB
  free of 108 GB; the engine needs ~7 MB of RAM and a few hundred MB of disk for PRoot Ubuntu. Bloat
  is not a constraint on this project.
* **It risks a great deal.** FRP can render the device unusable if the Google account is unavailable;
  the working Edge Service, its provisioned credential and its coordinator device identity are
  **destroyed**, and neither is restorable from a user-files backup.
* **It does not fix the actual blocker.** The USB fault is physical. Post-reset setup would face the
  same `error -71` link, with no working MCP surface in the meantime.
* **It costs time against a deadline.** The hackathon path needs the Linux engine working, not a
  cleaner app drawer.

Reconsider only if the owner's goal is a genuinely dedicated appliance *and* requirement 3 above
(Google account availability) is confirmed, *and* the user-file backup is reported VERIFIED with a
successful restore test. If those hold, a reset becomes reasonable rather than risky — but it is not
necessary for the project to succeed, and I would not spend the remaining time on it.

## 7. Preserved (unchanged)

Stock Android 11 installed; Edge Service v1.0.0 and its data untouched; signing identity unchanged;
all three APK/rollback artifacts intact with verified hashes. No uninstall, wipe, bulk removal,
factory reset, root, bootloader unlock, partition write or firmware change. No Oracle or WireGuard
change. No user files or backups published anywhere.
