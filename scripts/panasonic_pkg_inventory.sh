#!/usr/bin/env bash
# HG-CAM-05 step 2: READ-ONLY inventory and classification of installed packages.
#
# This script NEVER uninstalls, disables, clears data or changes any device state. It only
# reads. That is deliberate: the brief requires each removal to be an individual, verified
# decision, and forbids generic debloating. So the removal step is performed separately, one
# package at a time, with the operator's explicit approval for each.
#
# It also refuses to run against anything that is not the targeted Panasonic. That guard is
# load-bearing rather than ceremonial: another phone (the Mi A3) is routinely reachable over
# wireless ADB on this workstation, and an unguarded `pm` command would silently act on it.
#
# Usage: panasonic_pkg_inventory.sh [--json]
# Exit: 0 ok, 2 identity mismatch, 3 device absent.

set -uo pipefail

SERIAL="LRW86SC6RO8TZ99H"
MODEL="ELUGA_I7_EE"
BOARD="k39tv1_64_a27"
ABI="arm64-v8a"

A() { timeout 180 adb -s "$SERIAL" "$@"; }

# ---------------------------------------------------------------- guard
state="$(adb devices | awk -v s="$SERIAL" '$1==s{print $2}')"
if [[ "$state" != "device" ]]; then
  echo "FAIL: '$SERIAL' is not attached (state='${state:-absent}')." >&2
  echo "      Devices currently visible:" >&2
  adb devices -l | sed 's/^/        /' >&2
  echo "      Not falling back to any other device - another phone is often present." >&2
  exit 3
fi
read -r m b a <<<"$(A shell 'getprop ro.product.model; getprop ro.product.board; getprop ro.product.cpu.abi' | tr -d '\r' | tr '\n' ' ')"
if [[ "$m" != "$MODEL" || "$b" != "$BOARD" || "$a" != "$ABI" ]]; then
  echo "FAIL: identity mismatch. Got model='$m' board='$b' abi='$a'; expected $MODEL / $BOARD / $ABI." >&2
  exit 2
fi
echo "identity OK: $SERIAL $m $b $a"
echo

# ---------------------------------------------------------------- policy
# PRESERVE: never remove. System function, camera/audio/media stack, connectivity, security,
# ADB and the installer itself. The brief names these explicitly.
PRESERVE_RE='^(com\.android\.|android\.|com\.google\.android\.(gms|gsf|gmscore|inputmethod|packageinstaller|permissioncontroller|setupwizard|backuptransport)|com\.mediatek\.|com\.mtk\.|vendor\.|com\.media\.|org\.codeaurora\.)'
# CONFIRM: removable on paper, but their app-private data is not in the backup and may be
# unrecoverable. The brief requires separate confirmation for exactly these.
CONFIRM_RE='(whatsapp|facebook|messenger|katana|orca)'

echo "=== third-party (user-installed) packages, with on-disk APK size ==="
printf '  %-4s %-46s %10s  %s\n' "#" "package" "APK bytes" "class"
printf '  %-4s %-46s %10s  %s\n' "---" "----------------------------------------------" "---------" "-----"
i=0
: > /tmp/hg_pkg_inv.txt
for p in $(A shell 'pm list packages -3' | tr -d '\r' | sed 's/^package://' | sort); do
  # APK size(s); a package can have several splits
  bytes=0
  for path in $(A shell "pm path $p" | tr -d '\r' | sed 's/^package://'); do
    sz=$(A shell "ls -l $path 2>/dev/null" | tr -d '\r' | awk '{print $5}')
    [[ -n "${sz:-}" ]] && bytes=$((bytes + sz))
  done
  if   [[ "$p" =~ $CONFIRM_RE ]];    then cls="CONFIRM"
  elif [[ "$p" =~ $PRESERVE_RE ]];   then cls="PRESERVE"
  else cls="review"
  fi
  i=$((i+1))
  printf '  %-4s %-46s %10s  %s\n' "$i" "$p" "$bytes" "$cls"
  echo "$p|$bytes|$cls" >> /tmp/hg_pkg_inv.txt
done
echo "  total third-party packages: $i"
echo

echo "=== counts by class ==="
awk -F'|' '{c[$3]++} END {for (k in c) printf "  %-10s %s\n", k, c[k]}' /tmp/hg_pkg_inv.txt
echo

echo "=== CONFIRM-required packages (app-private data may be unrecoverable) ==="
awk -F'|' '$3=="CONFIRM" {print "  " $1}' /tmp/hg_pkg_inv.txt
echo

echo "=== storage and health snapshot (baseline for measuring what cleanup recovers) ==="
A shell 'df -h /data' | tr -d '\r' | awk 'NR==2{printf "  /data: %s total, %s used, %s available\n",$2,$3,$4}'
A shell 'dumpsys battery' | tr -d '\r' | grep -E 'level|health|status|temperature' | sed 's/^/  /'
echo

echo "=== READ-ONLY. Nothing was uninstalled, disabled or cleared. ==="
echo "Full list written to /tmp/hg_pkg_inv.txt for the removal step."
exit 0
