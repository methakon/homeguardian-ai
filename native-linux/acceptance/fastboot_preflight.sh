#!/usr/bin/env bash
# fastboot_preflight.sh — READ-ONLY preflight for the Mi A3 bootloader session.
#
# Writes NOTHING. This script never issues a command that modifies the device: no flash, no
# erase, no unlock, no partition change, no slot change. Every command below is a query.
#
# It exists to answer the two questions that decide GO/NO-GO before anything is risked:
#   1. CAN WE GET BACK?  -> `getvar anti` decides whether the verified restore package will
#                           even run. The package refuses if the device's anti-rollback
#                           version exceeds its own.
#   2. IS THE TRANSPORT SAFE? -> sustained fastboot stability, measured on the KERNEL's USB
#                           device number, not on "did the command answer".
#
# ADB stability is NOT evidence for fastboot stability. Different transport, different
# failure mode, and this project has already been misled once by an availability-only check
# that reported 100% across three re-enumerations.
#
# Usage here: print the plan. The actual session needs owner approval.
set -u

FB="$HOME/Android/Sdk/platform-tools/fastboot"
SERIAL="edd53dbd04de"
WANT_PRODUCT="laurel_sprout"

# Absolute path: fastboot is NOT on PATH, and a bare `fastboot` fails in a way that looks
# like "not installed".
[ -x "$FB" ] || { echo "FATAL: fastboot not at $FB"; exit 2; }

cat <<'PLAN'
==============================================================
 Mi A3 bootloader preflight — READ-ONLY
 No command below writes to the device.
==============================================================

STEP 0  enter bootloader mode (reversible; no partition is touched)
        adb -s edd53dbd04de reboot bootloader

        exit is equally harmless:
        fastboot reboot          (or hold Power ~10s)
==============================================================
PLAN

echo
echo "=== the exact command list that would run ==="
cat <<CMDS
  "$FB" devices                         # is it visible in fastboot?
  "$FB" getvar product                  # must be $WANT_PRODUCT - wrong device = stop
  "$FB" getvar serialno
  "$FB" getvar current-slot
  "$FB" getvar slot-count
  "$FB" getvar anti                     # *** decides whether rollback is POSSIBLE ***
  "$FB" getvar rollback-index
  "$FB" getvar unlocked
  "$FB" getvar secure
  "$FB" getvar max-download-size
  "$FB" getvar partition-type:userdata
  "$FB" flashing get_unlock_ability     # *** settles the unlock-mechanism conflict ***
  "$FB" oem device-info
  "$FB" getvar all                      # full capture, saved for later comparison
CMDS
echo
echo "=== decision rules ==="
cat <<'RULES'
  product != laurel_sprout        -> STOP. Wrong device.
  anti > 0                        -> STOP. The verified restore package will refuse to run
                                     ("Current device antirollback version is greater than
                                     this package"). Rollback is impossible with the artifact
                                     we hold, and the plan must change before anything is risked.
  get_unlock_ability == 0         -> `fastboot flashing unlock` would be REFUSED.
  unstable transport (see below)  -> STOP. Do not flash across a re-enumerating link.
RULES
echo
echo "=== transport stability measurement (the part that actually takes time) ==="
cat <<'STAB'
  Sample every 5s for >= 1800s. Record, in parallel:

    "$FB" getvar product                     -> must ALWAYS be laurel_sprout
    /sys/bus/usb/devices/<port>/devnum       -> kernel USB device number
    dmesg | grep "new .*USB device"          -> kernel announcing re-enumeration

  PASS requires ZERO device-number changes over the window.
  Availability alone is NOT a pass - that mistake was already made once.
STAB
echo
echo "=== data-loss reminders (informational) ==="
cat <<'LOSS'
  fastboot flashing unlock          -> FACTORY RESET (AOSP: the unlock transition resets)
  fastboot flashing unlock_critical -> resets if not already unlocked
  fastboot erase userdata           -> wipes userdata
  flash_all.sh                      -> erases + rewrites userdata (full wipe)
  flash_all_except_data_storage.sh  -> leaves userdata alone
  flash_all_lock.sh                 -> NEVER RUN: ends with `fastboot flashing lock`
  entering/leaving bootloader       -> nothing is modified
LOSS
echo
echo "=== current device state (read from Android, no bootloader needed) ==="
if command -v adb >/dev/null 2>&1; then
  for p in ro.boot.flash.locked ro.boot.verifiedbootstate sys.oem_unlock_allowed \
           ro.oem_unlock_supported ro.build.version.release ro.build.display.id; do
    v=$(timeout 10 adb -s "$SERIAL" shell getprop "$p" 2>/dev/null | tr -d '\r')
    printf '  %-30s %s\n' "$p" "${v:-<unreachable>}"
  done
else
  echo "  adb unavailable"
fi
echo
echo "STATUS: AWAITING OWNER APPROVAL. No bootloader command has been issued."
