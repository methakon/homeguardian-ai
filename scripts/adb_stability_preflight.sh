#!/usr/bin/env bash
# adb_stability_preflight.sh — READ-ONLY USB/ADB stability preflight for the Xiaomi Mi A3.
#
# Purpose: decide whether the Mi A3's USB link is stable enough to attempt an authenticated
# ADB session, BEFORE any install, authorization or VPN work depends on it. It observes only.
#
# Verdicts
#   PASS     Mi A3 present, Android 11 / API 30, zero USB churn for the whole window
#   FAIL     Mi A3 present but churn observed (disconnect / re-enumeration / enumeration error)
#   BLOCKED  Mi A3 not present (also the verdict when only the Panasonic handset is attached)
#
# Guarantees
#   - Modifies nothing: no install, no uninstall, no authorization reset, no key change,
#     no device setting, no credential access. Reads sysfs, `getprop`, `dumpsys` and the
#     kernel ring buffer only.
#   - Never mistakes the Panasonic ELUGA I7 CCTV handset for the target. The Panasonic is
#     USB vendor 0e8d (MediaTek); the Mi A3 is 2717 (Xiaomi) with product "Mi A3" /
#     device "laurel_sprout". A vendor mismatch is an explicit refusal, not a fallback.
#   - Redacts serial numbers. No raw serial appears in any output.
#
# Injectability (so the tool is testable without hardware — see scripts/adb_stability_fixtures/)
#   HG_SYSFS_ROOT  sysfs root to read USB topology from      (default: /sys)
#   HG_KLOG        file to read kernel USB events from       (default: journalctl -k)
#   HG_ADB         adb command to invoke                     (default: adb)
#   HG_WINDOW      observation window in seconds             (default: 300)
#   HG_POLL        sample interval in seconds                 (default: 5)
set -u

SYSFS="${HG_SYSFS_ROOT:-/sys}"
ADB_BIN="${HG_ADB:-adb}"
KLOG="${HG_KLOG:-}"
WINDOW="${HG_WINDOW:-300}"
POLL="${HG_POLL:-5}"

MI_VENDOR="2717"          # Xiaomi
PANASONIC_VENDOR="0e8d"   # MediaTek / Panasonic ELUGA
MI_PRODUCT_MATCH="Mi A3"
MI_DEVICE_MATCH="laurel_sprout"
REQUIRED_ANDROID_MAJOR="11"
REQUIRED_API="30"

say()  { printf '%s\n' "$*"; }
hdr()  { printf '\n== %s\n' "$*"; }
ok()   { printf '  [ok]   %s\n' "$*"; }
warn() { printf '  [warn] %s\n' "$*"; }
bad()  { printf '  [BAD]  %s\n' "$*"; }

# Kernel USB events. Real runs read the ring buffer; fixture runs read a file.
klog() {
  if [ -n "$KLOG" ]; then
    cat "$KLOG" 2>/dev/null
  else
    journalctl -k --no-pager 2>/dev/null || dmesg 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------------------
# 1. Discover USB devices from sysfs. Vendor/product/product-string only — the
#    serial is never printed.
# ---------------------------------------------------------------------------
hdr "USB topology (${SYSFS}/bus/usb/devices)"

MI_PORT=""
MI_FOUND_VENDOR=""
declare -a SEEN_PORTS=()
declare -a SEEN_DESC=()

for d in "$SYSFS"/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] || continue
  port="$(basename "$d")"
  case "$port" in *:*) continue ;; esac          # skip interfaces, keep devices
  vid="$(cat "$d/idVendor" 2>/dev/null)"
  pid="$(cat "$d/idProduct" 2>/dev/null)"
  prod="$(cat "$d/product" 2>/dev/null)"
  SEEN_PORTS+=("$port")
  SEEN_DESC+=("$vid:$pid $prod")

  if [ "$vid" = "$PANASONIC_VENDOR" ]; then
    ok "port $port: vendor $vid (MediaTek) \"$prod\" — NOT the target, ignored"
  elif [ "$vid" = "$MI_VENDOR" ]; then
    case "$prod" in
      *"$MI_PRODUCT_MATCH"*)
        MI_PORT="$port"; MI_FOUND_VENDOR="$vid"
        ok "port $port: vendor $vid (Xiaomi) \"$prod\" — Mi A3 CANDIDATE"
        ;;
      *)
        warn "port $port: vendor $vid (Xiaomi) \"$prod\" — Xiaomi but product != \"$MI_PRODUCT_MATCH\""
        ;;
    esac
  else
    : # unrelated USB device
  fi
done

if [ "${#SEEN_PORTS[@]}" -eq 0 ]; then
  warn "no USB devices found under $SYSFS/bus/usb/devices"
fi

# ---------------------------------------------------------------------------
# 2. Identify the target. Absence => BLOCKED. Never fall back to another handset.
# ---------------------------------------------------------------------------
hdr "Target identification"

if [ -z "$MI_PORT" ]; then
  bad "Mi A3 NOT present (no vendor-$MI_VENDOR device with product \"$MI_PRODUCT_MATCH\")"
  for i in "${!SEEN_PORTS[@]}"; do say "       saw ${SEEN_PORTS[$i]}: ${SEEN_DESC[$i]}"; done
  say ""
  say "VERDICT: BLOCKED — target handset absent; nothing to observe."
  say "         The Panasonic ELUGA CCTV handset must never be substituted for the Mi A3."
  exit 3
fi

ok "target on USB port $MI_PORT (serial redacted)"

# ---------------------------------------------------------------------------
# 3. Confirm Android 11 / API 30 through adb. Read-only getprop calls.
# ---------------------------------------------------------------------------
hdr "Android platform check"

ANDROID_MAJOR=""; SDK=""; DEVNAME=""; MODEL=""

# Resolve the transport for the discovered port without printing the serial.
TARGET_SERIAL=""
if command -v "$ADB_BIN" >/dev/null 2>&1; then
  # Match by USB path so we never pick the Panasonic.
  TARGET_SERIAL="$("$ADB_BIN" devices -l 2>/dev/null \
    | awk -v p="usb:$MI_PORT" 'NR>1 && $0 ~ p {print $1; exit}' | tr -d '\r')"
fi

if [ -z "$TARGET_SERIAL" ]; then
  bad "Mi A3 is on USB but has NO adb transport — likely mid-re-enumeration or unauthorized"
  say ""
  say "VERDICT: FAIL — device present on USB but not addressable by adb."
  exit 2
fi

ANDROID_MAJOR="$("$ADB_BIN" -s "$TARGET_SERIAL" shell getprop ro.build.version.release 2>/dev/null | tr -d '\r' || true)"
SDK="$("$ADB_BIN" -s "$TARGET_SERIAL" shell getprop ro.build.version.sdk 2>/dev/null | tr -d '\r' || true)"
DEVNAME="$("$ADB_BIN" -s "$TARGET_SERIAL" shell getprop ro.product.device 2>/dev/null | tr -d '\r' || true)"
MODEL="$("$ADB_BIN" -s "$TARGET_SERIAL" shell getprop ro.product.model 2>/dev/null | tr -d '\r' || true)"

say "  model=$MODEL device=$DEVNAME android=$ANDROID_MAJOR api=$SDK   (serial redacted)"

TARGET_OK=1
case "$DEVNAME" in
  *"$MI_DEVICE_MATCH"*) ok "device codename matches \"$MI_DEVICE_MATCH\"" ;;
  *) bad "device codename \"$DEVNAME\" != \"$MI_DEVICE_MATCH\" — wrong handset?"; TARGET_OK=0 ;;
esac
[ "$ANDROID_MAJOR" = "$REQUIRED_ANDROID_MAJOR" ] && ok "Android $REQUIRED_ANDROID_MAJOR" \
  || { bad "Android \"$ANDROID_MAJOR\" != required \"$REQUIRED_ANDROID_MAJOR\""; TARGET_OK=0; }
[ "$SDK" = "$REQUIRED_API" ] && ok "API $REQUIRED_API" \
  || { bad "API \"$SDK\" != required \"$REQUIRED_API\""; TARGET_OK=0; }

if [ "$TARGET_OK" -ne 1 ]; then
  say ""
  say "VERDICT: BLOCKED — a device responded but it does not match the Mi A3 profile."
  exit 3
fi

# ---------------------------------------------------------------------------
# 4. Baseline churn counters from the kernel log for this USB port.
# ---------------------------------------------------------------------------
hdr "Baseline before observation"

baseline_enum=$(klog | grep -c "usb $MI_PORT: new .*USB device" || true)
baseline_disc=$(klog | grep -c "usb $MI_PORT: USB disconnect" || true)
baseline_fail=$(klog | grep -cE "usb $MI_PORT: (device descriptor read.*error|Device not responding to setup address|device not accepting address)" || true)

devnum_now="$(cat "$SYSFS/bus/usb/devices/$MI_PORT/devnum" 2>/dev/null || echo '?')"

say "  device number now .......... $devnum_now"
say "  enumerations to date ....... $baseline_enum"
say "  disconnects to date ........ $baseline_disc"
say "  enumeration failures ....... $baseline_fail"
say "  (a high device number relative to enumeration count indicates prior churn)"

# ---------------------------------------------------------------------------
# 5. Observation window. Poll for churn. Changes nothing on the device.
# ---------------------------------------------------------------------------
hdr "Stability window: ${WINDOW}s (sampling every ${POLL}s)"

start=$(date +%s)
elapsed=0
churn=0
samples=0
state_changes=0
last_state=""

while [ "$elapsed" -lt "$WINDOW" ]; do
  samples=$((samples+1))

  enum_now=$(klog | grep -c "usb $MI_PORT: new .*USB device" || true)
  disc_now=$(klog | grep -c "usb $MI_PORT: USB disconnect" || true)
  fail_now=$(klog | grep -cE "usb $MI_PORT: (device descriptor read.*error|Device not responding to setup address|device not accepting address)" || true)

  if [ "$enum_now" -gt "$baseline_enum" ] || [ "$disc_now" -gt "$baseline_disc" ] || [ "$fail_now" -gt "$baseline_fail" ]; then
    churn=$((churn+1))
    bad "t+${elapsed}s churn detected: enum $baseline_enum->$enum_now, disconnect $baseline_disc->$disc_now, failures $baseline_fail->$fail_now"
    baseline_enum=$enum_now; baseline_disc=$disc_now; baseline_fail=$fail_now
  fi

  cur_state="$("$ADB_BIN" devices 2>/dev/null | awk -v s="$TARGET_SERIAL" '$1==s {print $2}')"
  if [ -n "$last_state" ] && [ "$cur_state" != "$last_state" ]; then
    state_changes=$((state_changes+1))
    bad "t+${elapsed}s adb state changed: $last_state -> ${cur_state:-<gone>}"
  fi
  last_state="$cur_state"

  sleep "$POLL"
  now=$(date +%s); elapsed=$((now-start))
done

hdr "Window summary"
say "  samples .................... $samples"
say "  churn events ............... $churn"
say "  adb state changes .......... $state_changes"
say "  final adb state ............ ${last_state:-<gone>}"

# ---------------------------------------------------------------------------
# 6. Verdict.
# ---------------------------------------------------------------------------
hdr "VERDICT"

if [ "$churn" -gt 0 ] || [ "$state_changes" -gt 0 ]; then
  bad "FAIL — USB churn observed during the window."
  say "       'Always allow' cannot persist while the transport keeps dying."
  say "       Fix the physical link (port / cable / hub) before authorizing anything."
  exit 1
fi

if [ "$last_state" != "device" ]; then
  bad "FAIL — adb state settled at \"${last_state:-<gone>}\", not \"device\"."
  exit 1
fi

ok "PASS — Mi A3 stable for ${WINDOW}s: no re-enumeration, no disconnects, no state changes."
say "       Safe to proceed to an authorization / acceptance gate."
say "       Reminder: a stable USB link says NOTHING about VPN routing or confinement."
exit 0
