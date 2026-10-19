# Bootloader read-only diagnostic plan

Prepared for the next **owner-authorised** bootloader session. Nothing here has been executed.
No unlock, no flash, no wipe, no OEM toggle — the only owner action already taken was enabling
OEM unlocking in Developer options, which by itself changes nothing.

---

## Current state (read from the running OS, this session)

```
ro.boot.flash.locked         1        bootloader LOCKED
ro.boot.verifiedbootstate    green    verified boot intact
ro.boot.vbmeta.device_state  locked
sys.oem_unlock_allowed       1        OEM unlocking ENABLED by the owner
ro.oem_unlock_supported      1        device supports unlocking
ro.build.version.release     11       Android 11, unaffected
```

`flash.locked = 1` alongside `oem_unlock_allowed = 1` is exactly right: the toggle **permits**
unlocking, it does not perform it.

---

## 1. How to inspect OEM unlocking availability

**From Android (already done, no risk):**

```bash
adb -s edd53dbd04de shell getprop sys.oem_unlock_allowed     # expect 1
adb -s edd53dbd04de shell getprop ro.oem_unlock_supported    # expect 1
```

**From the bootloader (read-only, needs the session):**

```bash
fastboot flashing get_unlock_ability      # expect 1 now that the toggle is on
```

If this returns `0`, `fastboot flashing unlock` will be **refused** regardless of the toggle —
this is the exact condition in the documented brick case where the device was left stuck with a
locked bootloader and unbootable slots.

## 2. Whether the bootloader is locked or unlocked

```bash
fastboot oem device-info
# look for:  (bootloader) Device unlocked: false
#            (bootloader) Device critical unlocked: false
#            (bootloader) Verity mode: true
```

**Critical distinction:** there are **two** unlock levels on this device —
`Device unlocked` and `Device critical unlocked`. The Mi A3 requires **both** commands for a full
OS replacement:

```bash
fastboot flashing unlock            # standard partitions
fastboot flashing unlock_critical   # critical partitions
```

The second is the one people miss, and without it the bootloader-adjacent partitions remain
write-protected.

## 3. How to enter and exit bootloader mode without flashing

**Enter — no writes occur on entry:**

```bash
adb -s edd53dbd04de reboot bootloader
```
Or on the phone: power off, then hold **Volume Down + Power** until the FASTBOOT screen appears.

**Confirm you are there:**

```bash
HOME/Android/Sdk/platform-tools/fastboot devices
# expect:  edd53dbd04de   fastboot
```

Note `fastboot` is **not on PATH** — call it by absolute path,
`/home/swarna-sekhar-dhar/Android/Sdk/platform-tools/fastboot`, or a bare `fastboot` fails and
looks like "not installed".

**Exit — no writes:**

```bash
fastboot reboot
```
Or hold **Power** ~10 s. If the device will not exit, the bootloader's own menu has
"Reboot system now".

**Entering and exiting bootloader mode does not, by itself, modify anything.** The danger is
entirely in the commands issued while there.

## 4. How to measure fastboot connection stability

ADB stability is **not** a proxy for fastboot stability, and this is the one gate that cannot be
settled from Android. The measurement to run, entirely read-only:

```bash
# every 5 s, for >= 30 min, record:
fastboot getvar product          # should always be laurel_sprout
fastboot getvar current-slot
fastboot getvar serialno

# and in parallel, from the host, the SAME authoritative signals used for ADB:
/sys/bus/usb/devices/<port>/devnum        # kernel USB device number
dmesg | grep "new .*USB device"           # kernel announcing re-enumeration
```

**Pass condition: zero device-number changes over >= 30 minutes.** Availability alone is not
enough — that is the mistake the earlier ADB measurement made, and it reported 100% availability
across three re-enumerations.

## 5. What could cause data loss

Ranked by likelihood, with the honest mechanism for each:

| Action | Data loss | Mechanism |
|---|---|---|
| `fastboot flashing unlock` | **total** | AOSP: the unlock transition "performs a factory data reset" |
| `fastboot flashing unlock_critical` | **total** (if not already unlocked) | same |
| `fastboot erase userdata` | **total** | wipes the userdata partition |
| `flash_all.sh` from the stock package | **total** | it runs `erase userdata` then flashes `userdata.img` |
| `flash_all_except_data_storage.sh` | **none** | explicitly leaves userdata alone |
| `flash_all_lock.sh` | **total on unlock, then possible brick** | ends with `fastboot flashing lock` |
| `fastboot flashing lock` (relock) | **on some paths** | relocking over a mismatched boot chain is a documented brick |
| entering/leaving bootloader | **none** | no partition is modified |

**The user-files backup is a mitigation, not a solution.** It is 941 files of shared-storage
media. It **cannot** restore installed apps, their data, account logins, SMS, call history, or
device settings. After a wipe the phone is a fresh Android install plus your photos and music.

**Before unlocking, do these or FRP will block you afterwards:**
- sign out of any Mi / Google account on the device
- remove screen locks
- otherwise Factory Reset Protection demands those exact credentials after the wipe

## 6. The unresolved question this session should settle

Sources genuinely conflict on the **unlock mechanism** for this device:

- **/e/OS install docs**: Mi Unlock **app required**, Windows 7+, Mi Account with phone number, a
  SIM, device linked under "Mi Unlock status", **possible wait up to 30 days**, one device per
  account per 30 days.
- **XDA guides (several)**: plain `fastboot flashing unlock` and `unlock_critical`, no Mi Unlock
  app.

The Mi A3 is an **Android One** device, which makes the plain-AOSP path plausible — but /e/OS
maintains an install path for this exact device and documents the app. **Both the account
requirement and any waiting period are unverified for this device** and should be observed rather
than asserted.

Cheap to settle read-only: `fastboot flashing get_unlock_ability`. If it reports 1 with the
toggle on, the plain path is likely available. It does not prove unlock will *succeed*, only that
it would not be refused on that ground.

## 7. Proposed session command list — all read-only

For approval. **Not one of these writes to the device:**

```bash
FB=/home/swarna-sekhar-dhar/Android/Sdk/platform-tools/fastboot
adb -s edd53dbd04de reboot bootloader
"$FB" devices
"$FB" getvar product
"$FB" getvar current-slot
"$FB" getvar slot-count
"$FB" getvar anti                  # <-- anti-rollback version; decides if rollback is possible
"$FB" getvar rollback-index
"$FB" getvar unlocked
"$FB" getvar secure
"$FB" getvar max-download-size
"$FB" flashing get_unlock_ability  # <-- settles the unlock-mechanism question
"$FB" oem device-info
"$FB" getvar all                   # full capture, saved for later comparison
"$FB" reboot
```

**The single most important read is `getvar anti`.** The stock restore package refuses to run if
the device's anti-rollback version exceeds the package's own:

```sh
ver=`fastboot getvar anti`; if [ $ver -gt 0 ]; then echo "...greater than this package"; exit 1; fi
```

If `anti` is greater than 0, **this verified recovery package cannot restore the device**, and the
whole plan changes before anything is risked. That is why this read comes first, before any
decision about unlocking.

---

## Standing position

Entering bootloader mode is a state change, so it waits for explicit authorisation of **this
specific operation**. Until then: image build continues host-side, and nothing on the phone is
touched.
