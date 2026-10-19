#!/usr/bin/env bash
# usb_stability_preflight.sh — read-only USB/ADB stability measurement for the Mi A3.
#
# Answers two DIFFERENT questions, because they have different answers:
#
#   1. AVAILABILITY  - could adb reach the device at each sample?
#   2. TRANSPORT STABILITY - did the USB link stay enumerated?
#
# These are not the same thing, and the difference is the whole point. A previous run of
# this measurement reported 59/59 samples "device" (100% availability) while the adb
# transport_id changed 1 -> 4 -> 5 -> 7 - three re-enumerations in 297s. Each recovery
# completed inside one sample interval, so an availability-only check reports a clean PASS
# on a transport that is NOT safe to flash across.
#
# Therefore this script tracks transport_id churn as a first-class result, and the verdict
# requires ZERO churn. Do not relax that: a flash is a single uninterruptible operation, and
# a re-enumeration during a write is the failure mode that bricks a device.
#
# Read-only: getprop / devices / get-state only. No writes, no settings changes, no reboots.
#
# Usage: usb_stability_preflight.sh [serial] [duration_seconds] [interval_seconds]
set -u

SERIAL="${1:-edd53dbd04de}"
DUR="${2:-300}"
INT="${3:-5}"
OUT="${HG_STABILITY_OUT:-/tmp/usb_stability_$(date +%Y%m%d-%H%M%S).tsv}"

printf 'elapsed\tadb_state\tmodel\ttransport\tboot_locked\twall\n' > "$OUT"

# Refuse to run against the wrong handset - the Panasonic is the other device on this bus.
identify() {
  timeout 10 adb -s "$SERIAL" shell getprop ro.product.model 2>/dev/null | tr -d '\r'
}
MODEL="$(identify)"
if [ -z "$MODEL" ]; then
  echo "BLOCKED: device $SERIAL not reachable. If it is physically attached, restart the adb"
  echo "         server - a stale server caches the old bus state and reports only the other"
  echo "         device, which looks identical to a dead cable."
  exit 2
fi
case "$MODEL" in
  *Mi*|*mi*) : ;;
  *) echo "REFUSING: $SERIAL reports model '$MODEL', not the Mi A3."; exit 3 ;;
esac
echo "target: $SERIAL  model=$MODEL  window=${DUR}s  interval=${INT}s"

start=$(date +%s); samples=0; up=0; gone=0; churn=0
prev_state=""; prev_trans=""
first_trans=""; last_trans=""

while :; do
  now=$(date +%s); el=$((now-start))
  [ "$el" -ge "$DUR" ] && break

  raw=$(timeout 8 adb -s "$SERIAL" get-state 2>&1 | tr -d '\r' | head -1)
  case "$raw" in
    device) state="device"; up=$((up+1)) ;;
    "")     state="GONE";   gone=$((gone+1)) ;;
    *)      state="$raw" ;;
  esac
  line=$(timeout 8 adb -s "$SERIAL" devices -l 2>/dev/null | grep "^$SERIAL" | tr -d '\r')
  trans=$(printf '%s' "$line" | grep -o 'transport_id:[0-9]*' | cut -d: -f2)
  model=$(printf '%s' "$line" | grep -o 'model:[^ ]*' | cut -d: -f2)
  locked=$(timeout 8 adb -s "$SERIAL" shell getprop ro.boot.flash.locked 2>/dev/null | tr -d '\r')

  [ -n "$prev_state" ] && [ "$state" != "$prev_state" ] && churn=$((churn+1))
  if [ -n "$prev_trans" ] && [ -n "$trans" ] && [ "$trans" != "$prev_trans" ]; then
    churn=$((churn+1))
    printf '  !! re-enumeration at t=%ss: transport %s -> %s\n' "$el" "$prev_trans" "$trans"
  fi
  [ -z "$first_trans" ] && first_trans="$trans"
  [ -n "$trans" ] && last_trans="$trans"
  prev_state="$state"; prev_trans="$trans"; samples=$((samples+1))

  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$el" "$state" "${model:-?}" "${trans:-?}" "${locked:-?}" "$(date +%H:%M:%S)" >> "$OUT"
  printf '  %4ss  %-7s  t=%-3s\n' "$el" "$state" "${trans:-?}"
  sleep "$INT"
done

echo
echo "================ RESULT ================"
echo "window              : ${DUR}s"
echo "samples             : $samples"
echo "AVAILABILITY"
echo "  state=device      : $up"
echo "  state=GONE        : $gone"
echo "  up-time           : $(awk -v a=$up -v s=$samples 'BEGIN{printf "%.1f%%",(s?a*100/s:0)}')"
echo "TRANSPORT STABILITY"
echo "  transport first   : ${first_trans:-?}"
echo "  transport last    : ${last_trans:-?}"
echo "  re-enumerations   : $churn"
echo "  distinct ids      : $(tail -n +2 "$OUT" | awk -F'\t' '{print $4}' | sort -u | tr '\n' ' ')"
echo "results             : $OUT"
echo
# The verdict deliberately requires BOTH conditions. Availability alone is not enough.
# Note: during a re-enumeration a sample can read neither "device" nor empty (transient
# "offline"/error state), so up<samples does NOT imply the device dropped out. Report the
# three cases distinctly - an earlier version conflated them and printed a self-contradictory
# "dead device, 0 GONE samples" message.
if [ "$churn" -gt 0 ]; then
  echo "VERDICT: BLOCKED - $churn re-enumeration(s) in ${DUR}s."
  echo "         Usable for long-running adb work; NOT safe to flash across."
  [ "$gone" -gt 0 ] && echo "         Also $gone sample(s) fully GONE."
  rc=1
elif [ "$gone" -gt 0 ]; then
  echo "VERDICT: FAIL    - device dropped out entirely ($gone sample(s) GONE)."
  rc=1
elif [ "$up" -eq "$samples" ]; then
  echo "VERDICT: PASS    - available for the full window with no re-enumeration"
  rc=0
else
  echo "VERDICT: REVIEW  - no re-enumeration and nothing GONE, but $((samples-up)) sample(s)"
  echo "                   were in an unexpected state. Inspect the results before trusting it."
  rc=1
fi
exit $rc
