#!/bin/bash
# HG-CAM-08D Stage 2: 30-minute battery-powered screen-off acceptance.
# Attended: interval sampling with hard safety aborts.
#
# FIXED after the first attempt died at 20:13:19: it referenced $T while having declared
# PREV_T, so `set -u` aborted the script on the SECOND loop iteration (the first passed
# only because `[ -n "$PREV_CPU" ]` short-circuited before $T was evaluated). The battery
# parse was also wrong - awk $2 picked the temperature, not the level.
set -u
W=<LAN_IP>:5555
A() { timeout 90 adb -s $W "$@"; }
P=org.homeguardian.harness
LOG=/tmp/hgcam8d_stage2.log
TRACE=/tmp/hgcam8d_stage2_logcat.txt
: > "$LOG"; : > "$TRACE"
say() { echo "$@" >> "$LOG"; echo "$@"; }

say "=== SETUP $(date -Is) ==="
say "power: $(A shell 'dumpsys battery' | tr -d '\r' | grep -iE 'AC powered|USB powered' | tr -d ' ' | tr '\n' ' ')"
A shell 'svc power stayon false' >/dev/null 2>&1
A shell 'input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1; sleep 3
A logcat -c >/dev/null 2>&1
A shell "am force-stop $P" >/dev/null 2>&1; sleep 2

say "arming video..."
A shell "am start -n $P/.MainActivity -a $P.CAM2_START" >/dev/null 2>&1; sleep 14
say "arming microphone..."
A shell "am start -n $P/.MainActivity -a $P.MIC2_START" >/dev/null 2>&1; sleep 14
say "isForeground=$(A shell "dumpsys activity services $P" | tr -d '\r' | grep -oE 'isForeground=[a-z]+' | head -1)"

( timeout 2400 adb -s $W logcat -s HGCctv:V HGCam2:V HGAudio:V > "$TRACE" 2>&1 ) &
LPID=$!
sleep 3

say "=== SCREEN OFF + LOCK $(date -Is) ==="
A shell 'input keyevent KEYCODE_SLEEP' >/dev/null 2>&1
sleep 6
say "wakefulness=$(A shell 'dumpsys power' | tr -d '\r' | grep -m1 'mWakefulness=' | sed 's/^ *//')"
say "screen=$(A shell 'dumpsys display' | tr -d '\r' | grep -m1 -oE 'mScreenState=[A-Z]+')"

START=$(date +%s)
PREV_CPU=""
PREV_T=""
while :; do
  NOW=$(date +%s); EL=$((NOW-START))
  [ "$EL" -ge 1800 ] && break

  PID=$(A shell "pidof $P" | tr -d '\r' | awk '{print $1}')
  [ -z "$PID" ] && PID=0
  CPU=$(A shell "awk '{print \$14+\$15}' /proc/$PID/stat 2>/dev/null" | tr -d '\r')
  RSS=$(A shell "awk '/VmRSS/{print \$2}' /proc/$PID/status 2>/dev/null" | tr -d '\r')
  UP=$(A shell 'cut -d" " -f1 /proc/uptime' | tr -d '\r')
  BRAW=$(A shell 'dumpsys battery' | tr -d '\r')
  LEVEL=$(echo "$BRAW" | awk -F: '/level:/{gsub(/ /,"",$2); print $2}')
  TEMP=$(echo "$BRAW" | awk -F: '/temperature:/{gsub(/ /,"",$2); print $2}')
  FG=$(A shell "dumpsys activity services $P" | tr -d '\r' | grep -oE 'isForeground=[a-z]+' | head -1)
  VT=$(grep -a 'cam2probe' "$TRACE" 2>/dev/null | grep -oE 'encodedBuffers=[0-9]+' | tail -1)
  VTK=$(grep -a 'cam2probe' "$TRACE" 2>/dev/null | grep -oE 'keyFrames=[0-9]+' | tail -1)
  MT=$(grep -a 'mic STARTED' "$TRACE" 2>/dev/null | grep -oE 'samples=[0-9]+' | tail -1)
  ERRS=$(grep -ac 'FATAL EXCEPTION' "$TRACE" 2>/dev/null)

  CPUPCT="?"
  if [ -n "$PREV_CPU" ] && [ -n "$CPU" ] && [ -n "$PREV_T" ]; then
    SPAN=$((EL-PREV_T))
    if [ "$SPAN" -gt 0 ]; then
      CPUPCT=$(awk -v a="$PREV_CPU" -v b="$CPU" -v d="$SPAN" 'BEGIN{printf "%.1f", (b-a)/100.0/d*100}')
    fi
  fi
  PREV_CPU="$CPU"; PREV_T="$EL"

  say "t=${EL}s pid=$PID $FG rss=${RSS}kB cpu=${CPUPCT}% up=${UP} level=${LEVEL} temp=${TEMP} ${VT:-nodata} ${VTK:-} ${MT:-nodata} fatal=$ERRS"

  if [ -n "${LEVEL:-}" ] && [ "${LEVEL:-0}" -lt 15 ] 2>/dev/null; then say "ABORT: battery below 15%"; break; fi
  if [ -n "${TEMP:-}" ] && [ "${TEMP:-0}" -gt 450 ] 2>/dev/null; then say "ABORT: temperature above 45C"; break; fi
  if [ "$PID" = "0" ]; then say "ABORT: app process gone"; break; fi

  sleep 60
done

say "=== 30-MINUTE WINDOW ENDED $(date -Is) ==="
say "VIDEO first->last: $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | head -1) -> $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | tail -1)"
say "VIDEO keyframes first->last: $(grep -a 'cam2probe' "$TRACE" | grep -oE 'keyFrames=[0-9]+' | head -1) -> $(grep -a 'cam2probe' "$TRACE" | grep -oE 'keyFrames=[0-9]+' | tail -1)"
say "MIC   first->last: $(grep -a 'mic STARTED' "$TRACE" | grep -oE 'samples=[0-9]+' | head -1) -> $(grep -a 'mic STARTED' "$TRACE" | grep -oE 'samples=[0-9]+' | tail -1)"
say "ticks=$(grep -ac 'tick #' "$TRACE") restarts: arming_video=$(grep -ac 'arming capture' "$TRACE") arming_mic=$(grep -ac 'arming mic' "$TRACE") camera_reopens=$(grep -ac 'device OPENED' "$TRACE") disarms=$(grep -ac 'disarming' "$TRACE")"
say "errors: fatal=$(grep -ac 'FATAL EXCEPTION' "$TRACE") camera_denied=$(grep -ac 'CAMERA_DISABLED' "$TRACE") aac_fail=$(grep -ac 'AAC ENCODE FAILURE' "$TRACE") probe_fail=$(grep -ac 'PROBE FAILURE' "$TRACE")"

say "=== screen back ON, control check ==="
A shell 'input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1; sleep 5
say "wakefulness=$(A shell 'dumpsys power' | tr -d '\r' | grep -m1 'mWakefulness=' | sed 's/^ *//')"
say "still capturing: $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | tail -1)"

say "=== DISARM both, verify IDLE RELEASE ==="
A shell "am start -n $P/.MainActivity -a $P.CAM2_STOP" >/dev/null 2>&1; sleep 6
A shell "am start -n $P/.MainActivity -a $P.MIC2_STOP" >/dev/null 2>&1; sleep 6
say "camera_clients=$(A shell 'dumpsys media.camera' | tr -d '\r' | grep -A1 'Active Camera Clients' | tail -1 | sed 's/^ *//')"
IDLEPID=$(A shell "pidof $P" | tr -d '\r' | awk '{print $1}')
IDLEPID=${IDLEPID:-0}
C1=$(A shell "awk '{print \$14+\$15}' /proc/$IDLEPID/stat 2>/dev/null" | tr -d '\r')
sleep 20
C2=$(A shell "awk '{print \$14+\$15}' /proc/$IDLEPID/stat 2>/dev/null" | tr -d '\r')
IDLE=$(awk -v a="${C1:-0}" -v b="${C2:-0}" 'BEGIN{printf "%.2f%%", (b-a)/100.0/20*100}')
say "idle cpu: ${C1:-n/a} -> ${C2:-n/a} ticks over 20s = $IDLE"
say "idle rss: $(A shell "awk '/VmRSS/{print \$2}' /proc/$IDLEPID/status 2>/dev/null" | tr -d '\r') kB"
say "battery: $(A shell 'dumpsys battery' | tr -d '\r' | grep -E 'level:|temperature:|health:' | tr -d ' ' | tr '\n' ' ')"
kill $LPID 2>/dev/null
say "=== DONE $(date -Is) ==="
