#!/usr/bin/env bash
# HomeGuardian service status / start / stop / restart over Wi-Fi.
#
#   edge_dev_status.sh status     # default: report everything worth knowing
#   edge_dev_status.sh start
#   edge_dev_status.sh stop
#   edge_dev_status.sh restart
#   edge_dev_status.sh probe      # ask the service to probe MCP immediately
#
# CONTROL SURFACE NOTE. EdgeService is exported=false, so `am start-service` is
# correctly refused by the platform and there is deliberately no control
# broadcast. Two supported paths exist:
#
#   * restart via redeploy - `edge_dev_deploy.sh` installs an update, which fires
#     MY_PACKAGE_REPLACED and starts the service. Needs no screen interaction.
#   * restart via the UI - wake the screen, launch MainActivity, tap the button.
#     Used below, and it is the only way to reach start/probe after a plain stop.
#
# `stop` uses `am force-stop`, which is what the platform offers a shell without
# root; it kills the process and cancels the sticky restart, so a following
# `start` is required.
#
# Exit: 0 ok, 2 identity mismatch, 3 device gone, 6 service action failed.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

ACTION="${1:-status}"
SERIAL="$(hg_resolve_target)" || hg_die "no usable target: connect first (edge_dev_connect.sh) or set HG_EDGE_TARGET" 3
hg_assert_mi_a3 "$SERIAL"

PID="$(hg_adb "$SERIAL" shell "pidof $HG_EDGE_PKG" 2>/dev/null | tr -d '\r')"
LISTEN="$(hg_adb "$SERIAL" shell 'cat /proc/net/tcp /proc/net/tcp6 2>/dev/null' 2>/dev/null \
          | tr -d '\r' | awk '$4=="0A"{print $2}' | grep -i ':2256' || true)"
SVC="$(hg_adb "$SERIAL" shell "dumpsys activity services $HG_EDGE_PKG" 2>/dev/null | tr -d '\r')"
FG="$(grep -c 'isForeground=true' <<<"$SVC" || true)"
# This build's dumpsys does NOT expose crashCount/restartCount on the ServiceRecord
# (verified: those fields are absent). Report the counters that DO exist rather than
# print empty strings. The crash/restart budget is visible from the app's own state
# via the UI and from the lifecycle lines in the probe log.
SFC="$(grep -oE 'startForegroundCount=[0-9]+' <<<"$SVC" | head -1)"
STARTREQ="$(grep -oE 'startRequested=[a-z]+' <<<"$SVC" | head -1)"
CREATED="$(grep -oE 'createTime=[-0-9a-z]+' <<<"$SVC" | head -1)"
RESTARTED="$(grep -oE 'restartTime=[-0-9a-z]+' <<<"$SVC" | head -1)"

# Tap a button by its label: wake, dismiss keyguard, launch, dump, tap.
ui_tap() {
  local label="$1"
  hg_adb "$SERIAL" shell 'input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1
  hg_adb "$SERIAL" shell 'wm dismiss-keyguard' >/dev/null 2>&1
  hg_adb "$SERIAL" shell "am start -n $HG_EDGE_PKG/.MainActivity" >/dev/null 2>&1
  sleep 2
  local xml="/sdcard/hg_ui_edge.xml"
  hg_adb "$SERIAL" shell "uiautomator dump $xml" >/dev/null 2>&1
  local bounds
  bounds="$(hg_adb "$SERIAL" shell "cat $xml" 2>/dev/null | tr -d '\r' \
            | python3 -c "
import sys,re
want='$label'.lower()
d=sys.stdin.read()
for m in re.finditer(r'text=\"([^\"]*)\"[^>]*bounds=\"\[(\d+),(\d+)\]\[(\d+),(\d+)\]\"', d):
    if want in m.group(1).lower():
        x=(int(m.group(2))+int(m.group(4)))//2; y=(int(m.group(3))+int(m.group(5)))//2
        print(f'{x} {y}'); break
")"
  local x y
  read -r x y <<<"$bounds"
  [[ -n "${x:-}" ]] || { hg_log "could not find a '$label' button in the UI"; return 1; }
  hg_log "tapping '$label' at ($x,$y)"
  hg_adb "$SERIAL" shell "input tap $x $y" >/dev/null 2>&1
  return 0
}

case "$ACTION" in
  status)
    echo "HomeGuardian edge service status (over Wi-Fi)"
    echo "  device          : $HG_EDGE_SERIAL ($(hg_adb "$SERIAL" shell getprop ro.product.model | tr -d '\r'))"
    echo "  process pid     : ${PID:-NOT RUNNING}"
    echo "  isForeground    : $([[ "$FG" -gt 0 ]] && echo yes || echo 'no/absent')"
    echo "  foreground count: ${SFC:-n/a}   ${STARTREQ:-}   ${CREATED:-}   ${RESTARTED:-}"
    echo "  MCP listener    : ${LISTEN:-none}   (0100007F:2256 == 127.0.0.1:8790 loopback-only)"
    echo "  exposed on LAN  : $([[ -z "$LISTEN" ]] && echo n/a || { grep -q '0100007F:2256' <<<"$LISTEN" && echo NO || echo 'YES - INVESTIGATE'; })"
    echo "  screen          : $(hg_adb "$SERIAL" shell dumpsys power 2>/dev/null | tr -d '\r' | sed -n 's/.*\(mWakefulness=[A-Za-z]*\).*/\1/p' | head -1)"
    logf=/sdcard/Android/data/$HG_EDGE_PKG/files/probe.log
    echo "  probes          : $(hg_adb "$SERIAL" shell "grep -c PROBE_OK $logf" | tr -d '\r') ok / $(hg_adb "$SERIAL" shell "grep -c PROBE_FAIL $logf" | tr -d '\r') fail"
    ;;
  stop)
    hg_adb "$SERIAL" shell "am force-stop $HG_EDGE_PKG" || hg_die "force-stop failed" 6
    sleep 2
    [[ -z "$(hg_adb "$SERIAL" shell "pidof $HG_EDGE_PKG" | tr -d '\r')" ]] \
      && echo "stopped (pid gone, listener should be closed)" \
      || hg_die "process still alive after force-stop" 6
    hg_adb "$SERIAL" shell 'cat /proc/net/tcp /proc/net/tcp6' | tr -d '\r' | grep -qi ':2256' \
      && hg_die "port still listening after stop" 6
    echo "  MCP listener verified CLOSED"
    ;;
  start)
    UI_START_LABEL="${HG_START_LABEL:-Start HomeGuardian}"
    ui_tap "$UI_START_LABEL" || hg_die "could not tap the start control" 6
    sleep 6
    PID="$(hg_adb "$SERIAL" shell "pidof $HG_EDGE_PKG" | tr -d '\r')"
    [[ -n "$PID" ]] && echo "started, pid=$PID" || hg_die "service did not come up after tap" 6
    ;;
  restart)
    # Preferred: redeploy (no screen interaction). Fall back to the UI button.
    if [[ "${HG_RESTART_VIA:-ui}" == "deploy" ]]; then
      exec "$(dirname "${BASH_SOURCE[0]}")/edge_dev_deploy.sh"
    fi
    ui_tap "${HG_RESTART_LABEL:-Restart HomeGuardian}" || hg_die "could not tap restart" 6
    sleep 8
    NEW="$(hg_adb "$SERIAL" shell "pidof $HG_EDGE_PKG" | tr -d '\r')"
    echo "restart requested; before pid=${PID:-none} after pid=${NEW:-none}"
    [[ -n "$NEW" ]] || hg_die "service not running after restart" 6
    ;;
  probe)
    ui_tap "${HG_PROBE_LABEL:-Probe now}" || hg_die "could not tap probe" 6
    sleep 4
    logf=/sdcard/Android/data/$HG_EDGE_PKG/files/probe.log
    hg_adb "$SERIAL" shell "tail -2 $logf" | tr -d '\r' | sed 's/^/  /'
    ;;
  *) hg_die "unknown action '$ACTION' (status|start|stop|restart|probe)" 1 ;;
esac
