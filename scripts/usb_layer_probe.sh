#!/usr/bin/env bash
# usb_layer_probe.sh — distinguish a REAL USB bus re-enumeration from an adb-server artifact.
#
# Why this exists: the earlier measurement tracked adb transport_id, which is assigned by the
# ADB SERVER. A transport_id change can mean (a) the USB link genuinely re-enumerated, or
# (b) the adb server reassigned its internal handles without the bus ever changing. Those have
# completely different implications for flashing, so they must be told apart.
#
# Decisive evidence, in order of authority:
#   1. /sys/bus/usb/devices/<port>/devnum   - the kernel's USB device number. Increments ONLY
#                                             on a real re-enumeration. Authoritative.
#   2. dmesg "new full-speed/full-speed USB device number N"  - kernel announcing a new device.
#   3. adb server PID                        - if THIS changes, the server restarted and any
#                                             transport_id churn is explained by that alone.
#   4. adb transport_id                      - weakest; a server-side handle.
#
# Read-only: no writes, no settings changes, no reboots, no firmware modification.
set -u

PORT="${1:-2-1}"          # USB port path of the Mi A3
DUR="${2:-600}"
INT="${3:-5}"
OUT="${HG_PROBE_OUT:-/tmp/usb_layer_$(date +%Y%m%d-%H%M%S).tsv}"
DEV="/sys/bus/usb/devices/$PORT"

[ -d "$DEV" ] || { echo "PORT $PORT not present. Device may be unplugged or moved."; exit 2; }

# Confirm this port really is the Mi A3 before measuring anything.
MFR=$(cat "$DEV/manufacturer" 2>/dev/null); PROD=$(cat "$DEV/product" 2>/dev/null)
SER=$(cat "$DEV/serial" 2>/dev/null)
echo "probing port $PORT : manufacturer='$MFR' product='$PROD' serial='$SER'"
case "$PROD" in *"Mi A3"*) : ;; *) echo "REFUSING: port $PORT is not the Mi A3 ('$PROD')"; exit 3 ;; esac

adb_server_pid() { pgrep -f 'adb -L tcp:5037 fork-server' 2>/dev/null | head -1; }

printf 'elapsed\tusb_devnum\tusb_speed\tadb_server_pid\ttransport\tadb_state\tkernel_newdev\n' > "$OUT"

start=$(date +%s); samples=0
bus_reenum=0; adb_restart=0; first=1
p_devnum=""; p_pid=""

echo
echo "=== probing ${DUR}s every ${INT}s ==="
while :; do
  now=$(date +%s); el=$((now-start)); [ "$el" -ge "$DUR" ] && break

  # 1. kernel's authoritative USB device number
  devnum=$(cat "$DEV/devnum" 2>/dev/null || echo "?")
  speed=$(cat "$DEV/speed" 2>/dev/null || echo "?")
  # 3. adb server pid - catches a server restart, which would explain transport churn alone
  pid=$(adb_server_pid)
  # 4. weakest signal
  line=$(timeout 8 adb -s "${SERIAL:-edd53dbd04de}" devices -l 2>/dev/null | grep -i "mi_a3\|${SERIAL:-edd53dbd04de}" | head -1 | tr -d '\r')
  trans=$(printf '%s' "$line" | grep -o 'transport_id:[0-9]*' | cut -d: -f2)
  state=$(timeout 8 adb -s "${SERIAL:-edd53dbd04de}" get-state 2>&1 | tr -d '\r' | head -1)
  # 2. kernel log: did the kernel announce a NEW device in the last interval?
  # `grep -c` prints a count AND exits non-zero when there are no matches, so `|| echo 0`
  # appended a SECOND zero: the variable became "0\n0", which corrupted every TSV row.
  # Read the count, then default only if genuinely empty.
  newdev=$(dmesg --since "${INT} seconds ago" 2>/dev/null | grep -ci "new .*USB device number")
  newdev=${newdev:-0}

  [ -n "$p_devnum" ] && [ "$devnum" != "$p_devnum" ] && { bus_reenum=$((bus_reenum+1)); echo "  !! BUS RE-ENUMERATION at t=${el}s : devnum $p_devnum -> $devnum"; }
  [ -n "$p_pid" ] && [ -n "$pid" ] && [ "$pid" != "$p_pid" ] && { adb_restart=$((adb_restart+1)); echo "  !! ADB SERVER RESTARTED at t=${el}s : pid $p_pid -> $pid"; }
  p_devnum="$devnum"; p_pid="$pid"
  [ "$newdev" -gt 0 ] && echo "  -- kernel announced $newdev new USB device(s) in the last ${INT}s"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$el" "$devnum" "$speed" "${pid:-?}" "${trans:-?}" "${state:-?}" "$newdev" >> "$OUT"
  printf '  %4ss devnum=%-4s pid=%-7s trans=%-3s state=%-7s\n' "$el" "$devnum" "${pid:-?}" "${trans:-?}" "${state:-?}"
  samples=$((samples+1))
  first=0
  sleep "$INT"
done

BS=$(awk -F'\t' 'NR>1{print $2}' "$OUT" | sort -u | tr '\n' ' ')
PS=$(awk -F'\t' 'NR>1{print $4}' "$OUT" | sort -u | tr '\n' ' ')
TS=$(awk -F'\t' 'NR>1{print $5}' "$OUT" | sort -u | tr '\n' ' ')

echo
echo "================ INTERPRETATION ================"
echo "window                    : ${DUR}s ($samples samples)"
echo "USB devnum values seen    : $BS"
echo "adb server pids seen      : $PS"
echo "adb transport ids seen    : $TS"
echo
echo "  USB bus re-enumerations : $bus_reenum   (kernel device number changed)"
echo "  adb server restarts     : $adb_restart   (server pid changed)"
echo
if [ "$bus_reenum" -gt 0 ]; then
  echo "  => REAL USB instability. A kernel-level re-enumeration is a hardware/link fault."
  echo "     This is the condition that makes flashing dangerous."
elif [ "$adb_restart" -gt 0 ]; then
  echo "  => ADB SERVER restarted. Transport churn is explained by the server, NOT the bus."
  echo "     The USB link itself did not re-enumerate."
else
  echo "  => Bus stable and server stable. Any earlier transport churn was not reproduced here."
fi
echo
echo "results: $OUT"
