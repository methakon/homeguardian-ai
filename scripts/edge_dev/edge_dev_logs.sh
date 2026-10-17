#!/usr/bin/env bash
# Collect sanitized logs and evidence from the Mi A3 over Wi-Fi.
#
#   edge_dev_logs.sh            # collect everything into logs/device-artifacts/
#
# Sanitization is mandatory: a 6-digit pairing code and any 32+ hex token are
# replaced before anything is written. Only OUR package's logcat is collected -
# a full logcat would sweep ~400 third-party packages and has no place in this
# repository's evidence set.
#
# Exit: 0 ok, 2 identity mismatch, 3 device gone.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

SERIAL="$(hg_resolve_target)" || hg_die "no usable target: connect first (edge_dev_connect.sh) or set HG_EDGE_TARGET" 3
hg_assert_mi_a3 "$SERIAL"

OUT="${HG_EDGE_ARTIFACTS:-$(dirname "${BASH_SOURCE[0]}")/../../logs/device-artifacts}"
mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"

collect() { # collect <name> <device command>
  local name="$1"; shift
  local f="$OUT/${name}.${STAMP}.txt"
  hg_adb "$SERIAL" shell "$@" 2>/dev/null | tr -d '\r' | hg_redact > "$f"
  printf '  %-38s %6s lines -> %s\n' "$name" "$(wc -l < "$f")" "$(basename "$f")"
}

echo "collecting from $HG_EDGE_SERIAL (Wi-Fi) into $OUT"
collect probe-log        "cat /sdcard/Android/data/$HG_EDGE_PKG/files/probe.log"
collect service-state    "dumpsys activity services $HG_EDGE_PKG"
collect package-state    "dumpsys package $HG_EDGE_PKG"
collect sockets          "cat /proc/net/tcp /proc/net/tcp6"
collect power-state      "dumpsys power | grep -E 'mWakefulness|mScreenOn|mHoldingDisplay|mIsPowered'"
collect battery-state    "dumpsys battery"
collect wifi-state       "dumpsys wifi | grep -m1 mWifiInfo"
collect thermals         "for z in /sys/class/thermal/thermal_zone*/; do echo \"\$(cat \$z/type) \$(cat \$z/temp)\"; done | sort"

# Our package's logcat only, current + crash buffers.
LOG="$OUT/logcat_edge.${STAMP}.txt"
timeout 30 adb -s "$SERIAL" logcat -d -v threadtime 2>/dev/null \
  | grep -E "$HG_EDGE_PKG|hg_edge|EdgeService|libhg_edge" | hg_redact > "$LOG"
printf '  %-38s %6s lines -> %s\n' "logcat (our package only)" "$(wc -l < "$LOG")" "$(basename "$LOG")"

CRASH="$OUT/logcat_crash.${STAMP}.txt"
timeout 30 adb -s "$SERIAL" logcat -d -b crash -v threadtime 2>/dev/null \
  | grep -E "$HG_EDGE_PKG|hg_edge" | hg_redact > "$CRASH" || true
printf '  %-38s %6s lines -> %s\n' "native/java crash buffer" "$(wc -l < "$CRASH")" "$(basename "$CRASH")"

TMB="$OUT/tombstones.${STAMP}.txt"
hg_adb "$SERIAL" shell 'ls -l /data/tombstones/ 2>/dev/null' | hg_redact > "$TMB"
printf '  %-38s %6s lines -> %s\n' "tombstone listing (root-only files)" "$(wc -l < "$TMB")" "$(basename "$TMB")"

# Post-collection sanity: no secret may have survived redaction.
echo
echo "sanitization check (must all be 0):"
echo "  6-digit pairing-code shapes : $(cat "$OUT"/*."$STAMP".txt 2>/dev/null | grep -cE '\b[0-9]{6}\b')"
echo "  32+ hex token shapes        : $(cat "$OUT"/*."$STAMP".txt 2>/dev/null | grep -cE '\b[0-9a-fA-F]{32,}\b')"
echo
echo "evidence written to $OUT (timestamp $STAMP)"
