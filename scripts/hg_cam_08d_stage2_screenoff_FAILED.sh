#!/bin/bash
# HG-CAM-08D Stage 2 — PRESERVED FAILED VERSION (kept deliberately, do not run).
#
# ARTIFACT IDENTITY: this is the FIRST version of the 30-minute screen-off sampler. It is
# retained because it failed, and the way it failed is the reason the corrected version can
# be trusted. Do not "fix" this file; the corrected one is hg_cam_08d_stage2_screenoff.sh.
#
# FAILURE (measured, 2026-10-17):
#     === SCREEN OFF + LOCK 2026-10-17T20:13:11+05:30 ===
#     t=0s pid=11248 isForeground=true rss=117932kB cpu=?% up=5918.44 batt=97 320 \
#          encodedBuffers=1018 keyFrames=68 samples=1006080 fatal=0
#     /tmp/hgcam8d_stage2.sh: line 56: T: unbound variable
#
# TWO DEFECTS:
#
# 1. UNBOUND VARIABLE (the fatal one). The CPU-percent branch tested `$T`, but the variable
#    actually assigned was `PREV_T`:
#        if [ -n "$PREV_CPU" ] && [ -n "$CPU" ] && [ "$T" != "" ]; then
#    On iteration 1, `[ -n "$PREV_CPU" ]` was false, so the shell short-circuited before it
#    ever evaluated `$T` and the sample printed. On iteration 2, PREV_CPU was set, so `$T`
#    was evaluated, `set -u` aborted the script, and the log froze at t=0s.
#    That is why the run looks like it "produced one good sample then stopped": the only
#    sample it ever produced is the one that could not reach the bug.
#
# 2. SWAPPED BATTERY PARSE. `grep -E 'level:|temperature:'` then `tr -dc '0-9\n'` yields
#    "97\n320", so `awk '{print $2}'` returns 320 — the TEMPERATURE, not the level. The
#    two safety-abort thresholds were therefore being applied to the wrong numbers. It did
#    not fire on this run, but a real low-battery condition would not have aborted it.
#
# CONSEQUENCE FOR METHOD: a sampler that prints one sample proves nothing. The corrected
# run is not treated as valid until it has emitted BOTH t=0 and t=60.
#
# Reconstructed verbatim from the version that was executed; only this header is added.

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

( timeout 2200 adb -s $W logcat -s HGCctv:V HGCam2:V HGAudio:V > "$TRACE" 2>&1 ) &
LPID=$!
sleep 3

say "=== SCREEN OFF + LOCK $(date -Is) ==="
A shell 'input keyevent KEYCODE_SLEEP' >/dev/null 2>&1
sleep 6
say "wakefulness=$(A shell 'dumpsys power' | tr -d '\r' | grep -m1 'mWakefulness=' | sed 's/^ *//')"
say "screen=$(A shell 'dumpsys display' | tr -d '\r' | grep -m1 -oE 'mScreenState=[A-Z]+')"

START=$(date +%s)
PREV_CPU=""; PREV_T=""
while :; do
  NOW=$(date +%s); EL=$((NOW-START))
  [ "$EL" -ge 1800 ] && break

  PID=$(A shell "pidof $P" | tr -d '\r' | awk '{print $1}')
  [ -z "$PID" ] && PID=0
  CPU=$(A shell "awk '{print \$14+\$15}' /proc/$PID/stat 2>/dev/null" | tr -d '\r')
  RSS=$(A shell "awk '/VmRSS/{print \$2}' /proc/$PID/status 2>/dev/null" | tr -d '\r')
  D=$(A shell 'cat /proc/uptime' | tr -d '\r'); UP=${D%% *}
  BAT=$(A shell 'dumpsys battery' | tr -d '\r' | grep -E 'level:|temperature:' | tr -dc '0-9\n' | tr '\n' ' ')
  FG=$(A shell "dumpsys activity services $P" | tr -d '\r' | grep -oE 'isForeground=[a-z]+' | head -1)
  VT=$(grep -a 'cam2probe' "$TRACE" 2>/dev/null | grep -oE 'encodedBuffers=[0-9]+' | tail -1)
  VTK=$(grep -a 'cam2probe' "$TRACE" 2>/dev/null | grep -oE 'keyFrames=[0-9]+' | tail -1)
  MT=$(grep -a 'mic STARTED' "$TRACE" 2>/dev/null | grep -oE 'samples=[0-9]+' | tail -1)
  ERRS=$(grep -ac 'FATAL EXCEPTION' "$TRACE" 2>/dev/null)

  CPUPCT="?"
  # DEFECT 1 lives on the next line: $T was never assigned; the variable is PREV_T.
  if [ -n "$PREV_CPU" ] && [ -n "$CPU" ] && [ "$T" != "" ]; then
    CPUPCT=$(awk -v a="$PREV_CPU" -v b="$CPU" -v d="$((EL-PREV_T))" 'BEGIN{ if(d>0) printf "%.1f", (b-a)/100.0/d*100; else print "?" }')
  fi
  PREV_CPU="$CPU"; PREV_T="$EL"

  # DEFECT 2 lives here: these are positional fields, not labelled ones.
  LVL=$(echo "$BAT" | awk '{print $2}')
  TMP=$(echo "$BAT" | awk '{print $3}')
  say "t=${EL}s pid=$PID $FG rss=${RSS}kB cpu=${CPUPCT}% up=${UP} batt=${BAT} ${VT:-no-data} ${VTK:-} ${MT:-no-data} fatal=$ERRS"

  if [ -n "${LVL:-}" ] && [ "$LVL" -lt 15 ] 2>/dev/null; then say "ABORT: battery below 15%"; break; fi
  if [ -n "${TMP:-}" ] && [ "$TMP" -gt 450 ] 2>/dev/null; then say "ABORT: temperature above 45C"; break; fi
  if [ "$PID" = "0" ]; then say "ABORT: app process gone"; break; fi

  sleep 60
done

say "=== 30-MINUTE WINDOW ENDED $(date -Is) ==="
say "initial vs final (service ticks during the screen-off window):"
say "  $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | head -1) -> $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | tail -1)"
say "  $(grep -a 'mic STARTED' "$TRACE" | grep -oE 'samples=[0-9]+' | head -1) -> $(grep -a 'mic STARTED' "$TRACE" | grep -oE 'samples=[0-9]+' | tail -1)"
say "  tick count: $(grep -ac 'tick #' "$TRACE")"
say "  arm/restart events: arming_capture=$(grep -ac 'arming capture' "$TRACE") arming_mic=$(grep -ac 'arming mic' "$TRACE") reopens=$(grep -ac 'device OPENED' "$TRACE")"
say "  fatal=$(grep -ac 'FATAL EXCEPTION' "$TRACE") camera_denied=$(grep -ac 'CAMERA_DISABLED' "$TRACE")"

say "=== screen back ON, control check ==="
A shell 'input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1; sleep 5
say "wakefulness=$(A shell 'dumpsys power' | tr -d '\r' | grep -m1 'mWakefulness=' | sed 's/^ *//')"
say "still capturing: $(grep -a 'cam2probe' "$TRACE" | grep -oE 'encodedBuffers=[0-9]+' | tail -1)"

say "=== DISARM both, then verify IDLE RELEASE ==="
A shell "am start -n $P/.MainActivity -a $P.CAM2_STOP" >/dev/null 2>&1; sleep 6
A shell "am start -n $P/.MainActivity -a $P.MIC2_STOP" >/dev/null 2>&1; sleep 6
say "camera_clients=$(A shell 'dumpsys media.camera' | tr -d '\r' | grep -A1 'Active Camera Clients' | tail -1 | sed 's/^ *//')"
IDLEPID=$(A shell "pidof $P" | tr -d '\r' | awk '{print $1}')
C1=$(A shell "awk '{print \$14+\$15}' /proc/${IDLEPID:-0}/stat 2>/dev/null" | tr -d '\r')
sleep 20
C2=$(A shell "awk '{print \$14+\$15}' /proc/${IDLEPID:-0}/stat 2>/dev/null" | tr -d '\r')
say "idle cpu: $C1 -> $C2 ticks over 20s = $(awk -v a="$C1" -v b="$C2" 'BEGIN{ if(a!=""&&b!="") printf "%.1f%%", (b-a)/100.0/20*100; else print "n/a" }')"
say "idle rss: $(A shell "awk '/VmRSS/{print \$2}' /proc/${IDLEPID:-0}/status 2>/dev/null" | tr -d '\r') kB"
say "battery: $(A shell 'dumpsys battery' | tr -d '\r' | grep -E 'level:|temperature:|health:' | tr -d ' ' | tr '\n' ' ')"
kill $LPID 2>/dev/null
say "=== DONE $(date -Is) ==="
