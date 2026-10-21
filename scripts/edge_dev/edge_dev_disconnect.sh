#!/usr/bin/env bash
# Safely wind down the wireless development session.
#
#   edge_dev_disconnect.sh              # drop the wireless transport, keep the service running
#   edge_dev_disconnect.sh --off        # ALSO turn Wireless Debugging off on the phone
#
# `--off` is the security-preferred end state: Wireless Debugging should only be
# enabled on a trusted development network and disabled when it is not in use.
# It needs the phone screen (Android 11 exposes no adb-callable toggle), so this
# script attempts it and tells you plainly if it could not.
#
# Exit: 0 ok, 3 nothing to do.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

TURN_OFF=0
[[ "${1:-}" == "--off" ]] && TURN_OFF=1

# Remove any forwards this toolkit created.
for p in "$HG_EDGE_FWD_PORT"; do
  timeout 15 adb forward --remove "tcp:$p" >/dev/null 2>&1 && hg_log "removed host forward tcp:$p" || true
done

WL="$(adb devices -l | awk '$0 ~ /_adb-tls-connect|:[0-9]+/ && $1 ~ /^[0-9a-f]+$/ {print $1}' | head -1)"
if adb devices -l | grep -qE '^[0-9a-f]+ +device +usb:'; then
  hg_log "a USB transport is present - wireless disconnect will not remove it"
fi

# Disconnect every network transport that is not the USB one.
mapfile -t eps < <(adb devices -l | awk '/_adb-tls-connect|device product:.*device:/ {next} /:[0-9]+/ {print $1}' | sort -u)
if (( ${#eps[@]} )); then
  for e in "${eps[@]}"; do
    timeout 15 adb disconnect "$e" >/dev/null 2>&1 && hg_log "disconnected $e" || true
  done
else
  hg_log "no wireless transport to disconnect"
fi

echo
echo "remaining devices:"
adb devices -l | sed 's/^/  /'

if (( TURN_OFF )); then
  SERIAL="${HG_EDGE_TARGET:-$HG_EDGE_SERIAL}"
  if [[ "$(hg_state "$SERIAL")" == "device" ]]; then
    hg_log "attempting to disable Wireless Debugging"
    hg_adb "$SERIAL" shell 'settings put global adb_wifi_enabled 0' >/dev/null 2>&1 || true
    v="$(hg_adb "$SERIAL" shell 'settings get global adb_wifi_enabled' | tr -d '\r')"
    if [[ "$v" == "0" ]]; then
      echo "  Wireless Debugging reports disabled"
    else
      cat <<EOF
  COULD NOT disable Wireless Debugging from adb (it reads '$v').
  Android 11 only exposes that toggle in the UI, and the framework resets the
  global setting. On the phone:
    Settings > System > Developer options > Wireless debugging > toggle OFF
  Until then the TLS endpoint stays available on the trusted LAN - it still
  requires this host's key, but turning it off is the intended end state.
EOF
    fi
  else
    hg_log "device not reachable to toggle the setting - disable it on the phone screen"
  fi
fi
