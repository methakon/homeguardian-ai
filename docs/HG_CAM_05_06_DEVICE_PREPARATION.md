# HG-CAM-05 / HG-CAM-06 — Device Preparation: Status, Blocker and Authorization Corrections

Branch: `experiment/hg-cam-eluga-cctv` (isolated Panasonic worktree).
Device: Panasonic ELUGA I7, serial `LRW86SC6RO8TZ99H`, `ELUGA_I7_EE`, board `k39tv1_64_a27`,
MT6739, arm64-v8a, Android 9 / API 28.
Scope: this branch only. The Mi A3, the HG-SEC-04 release candidate and the mail VPS are
untouched.

## 1. Current blocker — the physical device is absent

**Status: BLOCKED.** Verified once, not repeatedly re-run:

    adb devices -l   ->  only 192.168.1.14:40815 = laurel_sprout / Mi_A3  (wireless)
    lsusb            ->  no phone vendor id at all (no MediaTek 0e8d, no Google 18d1)
    USB port 2-3     ->  empty

The only reachable device is the Mi A3, which is explicitly out of scope for this workstream.
**Nothing was issued against it.** The identity guard in the staged tool exists specifically
because that situation is routine on this workstation: an unguarded `pm` command would
silently act on the wrong phone.

No Panasonic operation will be started until the physical device is attached.

## 2. Correction — the account-removal reading

An early probe printed `com.google entries: 0`, which superficially looks like confirmation
that the Google account is gone. **It is not evidence of anything.** `dumpsys account` never
ran, because the device was absent, so the count was taken from an empty file. An empty
result set from a command that never executed cannot establish removal.

The account state is therefore recorded as **owner-reported, not verified**. It will be
confirmed authoritatively on reconnection by re-reading `dumpsys account`, comparing against
the pre-removal baseline in the backup tree, and checking that the three non-Google accounts
survived. If the device disagrees with the report, the discrepancy is documented rather than
papered over.

## 3. Correction — application-removal authorization (my error)

I asked the manager to re-approve removal of WhatsApp, Facebook and Messenger, and treated
"unspecified" as keep. **That was wrong.** HG-CAM-05 had already authorized:

- removal of unnecessary user-installed applications from the Panasonic;
- permanent loss of their app-private data;
- specifically the removal of WhatsApp, Facebook and Messenger;
- removal of other clearly unnecessary third-party applications, individually.

The manager understood that WhatsApp messages might not be recoverable from the shared-storage
backup. This correction is recorded so the mistake is not repeated: **do not ask again**, and
do not treat these three as awaiting a separate decision.

This is not blanket authorization. Still excluded: system debloating, deleting shared storage,
factory reset, firmware flashing, partition modification, and uninstalling essential packages.

## 4. What is staged and ready

### `scripts/panasonic_pkg_inventory.sh` — READ-ONLY

Lists third-party packages with APK sizes and classifies them:

| class | meaning |
|---|---|
| `PRESERVE` | Android system, MTK/vendor, camera/audio/media stack, GMS, connectivity, security, installer |
| `CONFIRM` | WhatsApp / Facebook / Messenger — **already authorized for removal** under HG-CAM-05, flagged so each is still an individual, deliberate act rather than a wildcard |
| `review` | anything unrecognised — left installed unless separately justified |

The tool issues exactly six read-only shell commands and contains **no uninstall, disable,
clear, wipe or reset operation of any kind**. It cannot remove anything by construction.

### `scripts/test_panasonic_inventory_safety.sh` — 8 safety properties, verified offline

Because the tool's whole value is its refusal behaviour, it is tested against a stub `adb`
driving scenarios a real device cannot easily produce. It tests the real script file, so a
future edit that weakens the guard fails the suite.

| test | property | result |
|---|---|---|
| P1 | target absent → refuses, does not fall back, reports the visible device list | PASS (exit 3) |
| P2 | **only the Mi A3 connected → refuses and never substitutes it** | PASS (exit 3) |
| P3 | target present but `offline` → refuses | PASS (exit 3) |
| P4 | wrong model under the target serial (a Mi A3 masquerading) → identity mismatch | PASS (exit 2) |
| P5 | malformed property output → refused, not read as a valid identity | PASS (exit 2) |
| P6 | empty package list → completes cleanly, zero packages, no destructive act | PASS (exit 0) |
| P7 | correct device → inventory runs; the three apps classify `CONFIRM`, unknown classifies `review` | PASS |
| P8 | static scan: no uninstall/disable/clear/wipe/reset reachable from the tool | PASS |

## 5. Verified versus owner-reported

Kept explicit, because conflating them is how a project ends up believing something false.

| item | status |
|---|---|
| Device identity, capabilities, storage, battery | **VERIFIED** on hardware |
| Shared-storage backup + encrypted archive round-trip (3,839/3,839) | **VERIFIED** |
| Path B (no ROM, no kernel source, unlock unavailable as shipped) | **VERIFIED** by API-level search |
| arm64/API-28 APK build with foreground service | **VERIFIED** as a build artifact only |
| Google account removed | **OWNER-REPORTED, not verified** (§2) |
| Camera/microphone operation | **NOT TESTED** — a successful build is not operation |
| Screen-off capture survival | **NOT TESTED** — the gate for the whole workstream |

**No claim of camera, video or audio operation is made anywhere in this branch.** The APK
build evidence proves compilation and packaging, nothing more.

## 6. Commits — and why GitHub could not serve `6428f80`

The branch is **local only**; it has no `origin` counterpart, which is exactly why the
reviewer's fetch of `6428f80` failed. Nothing here has been pushed, and no private device
data is in any commit.

    branch: experiment/hg-cam-eluga-cctv   (local; no remote tracking branch)
    remote: https://github.com/methakon/homeguardian-ai.git  (branch not pushed)

| full SHA | subject |
|---|---|
| `6428f80cfcdaafe2c35104b37b4e1d3f98b4fb83` | HG-CAM-05: staged read-only package inventory tool; cleanup blocked, device absent |
| `c6fa0ca9a8efbc0eacc102d3bff5f909d1fcc3cc` | HG-CAM-04: foreground service + arm64/API-28 build, and power-resilience design |
| `84d393d33a763ecbfb63d03ac6379ee4f54de55e` | HG-CAM-03/04: correct the account inventory, add the CAM roadmap section and the CCTV APK spec |
| `87d2e3c3f449c57c9b346ade9b1464cdd02c4bdb` | HG-CAM-03: rebuild the config archive so it no longer embeds a false FAIL |
| `345f4d106f255a823d03aad5656eda71f3c78a31` | HG-CAM-02: fold in independently verified Path B evidence |
| `b2da71f5fec99a0137d2115fa74b86158df43b21` | HG-CAM-02/03: Panasonic ELUGA I7 backup verification, Path B OS assessment, reset decision |
| `e937f25ca3b3b7ed5dc3d45970edf68ce2b70bf0` | HG-CAM-03: ignore `/devices/backup/` (private device backups) |
| `cad758fb236e263645ffd0fec0a2cfcfd237e73c` | shared base commit (not mine) |

Evidence location for the manager's synchronisation: the three `docs/HG_CAM_*` files, the
two `scripts/panasonic_*` files, and `docs/ROADMAP.md` Phase CAM — all within this branch.
The backup itself lives outside version control at
`devices/backup/deviceprofile/panasonic-eluga-i7/` (gitignored) with its encrypted copy at
`~/backups/homeguardian/`.

## 7. When the device reconnects — the staged sequence

Short, independently recoverable steps, each re-verifying device identity first, per HG-CAM-06
Stages A–E:

1. **A** — confirm USB presence, serial, manufacturer/model/API; reject offline or ambiguous
   selection; never fall back. Stop immediately if the device disappears mid-step.
2. **B** — fresh `dumpsys account`; confirm Google absent and the three non-Google accounts
   intact; **types and counts only**, no identifiers.
3. **C** — run the read-only inventory; classify PRESERVE / REMOVE (authorized) / REVIEW.
   APK size is recorded but is never the reason for removal.
4. **D** — remove one app at a time: identity check → record package and state → record
   `/data` before → remove → record exit status and resulting state → verify networking and
   essential services still respond → record `/data` after → keep the evidence.
5. **E** — final health check: boot/responsive, Wi-Fi and ADB alive, camera/microphone/audio
   frameworks present *without initiating capture*, installer and security services present,
   remaining accounts and packages inventoried, and the shared-storage backup untouched.

## 8. Remaining risks and blockers

| item | status |
|---|---|
| Physical device absent | **BLOCKING** — everything in §7 waits on it |
| Google account removal unverified | open until Stage B runs |
| Screen-off capture survival | untested; the gate for the CCTV workstream |
| SoC temperature unreadable without root | permanent limitation; mitigated behaviourally |
| Backup and key share one disk | open — needs the owner's off-machine second copy |

Not in scope and not touched: factory reset, bootloader unlock, flashing, repartitioning,
live media activation, the Mi A3 workstream, HG-SEC-04, and the Oracle mail VPS.
