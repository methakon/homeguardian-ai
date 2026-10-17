#!/usr/bin/env bash
# Re-enable Android 11 Wireless Debugging on the Mi A3 via the UI.
#
# WHY THIS EXISTS: measured behaviour (HG-EDGE-04 test T4) - a Wi-Fi outage does NOT
# merely rotate the wireless-debugging port, it STOPS wireless debugging. The phone came
# back on the same IP and the same randomized MAC, but every port refused, so a manual
# re-enable is required. Android 11 exposes no adb-callable toggle: `settings put global
# adb_wifi_enabled 1` is reset by the framework (verified twice). So the only path is the
# UI, and this drives it.
#
# Requires an EXISTING adb transport (USB) - it is the bootstrap for getting wireless
# back, and it is exactly the "temporary USB" step the operator approved for recovery.
#
# The network-trust dialog ("Allow wireless debugging on this network?") is a security
# decision and is NOT made by this script. It re-applies the operator's standing choice:
# ALLOW with "Always allow on this network" left UNTICKED. If you want a different
# answer, tap it yourself - the script deliberately never touches the checkbox.
#
# Usage: edge_dev_enable_wireless.sh [serial]
# Exit: 0 enabled (prints IP:PORT), 1 could not drive the UI, 3 no transport.

set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

SERIAL="${1:-${HG_EDGE_TARGET:-$HG_EDGE_SERIAL}}"
[[ "$(hg_state "$SERIAL")" == "device" ]] || hg_die "need an existing adb transport to bootstrap; '$SERIAL' is not usable" 3
hg_assert_mi_a3 "$SERIAL"

D=/sdcard/hg_dbg.xml
dump() { hg_adb "$SERIAL" shell "rm -f $D; uiautomator dump $D >/dev/null 2>&1; cat $D" 2>/dev/null | tr -d '\r'; }
find_text() { # find_text <label> -> "x y" of its centre, case-insensitive substring
  dump | python3 -c "
import sys, re
want = '''$1'''.lower()
d = sys.stdin.read()
for n in re.findall(r'<node\b[^>]*>', d):
    t = re.search(r'text=\"([^\"]*)\"', n)
    b = re.search(r'bounds=\"\[(\d+),(\d+)\]\[(\d+),(\d+)\]\"', n)
    if not t or not b: continue
    if want in t.group(1).lower():
        x = (int(b.group(1)) + int(b.group(3))) // 2
        y = (int(b.group(2)) + int(b.group(4))) // 2
        print(f'{x} {y} {t.group(1)}'); break
"
}
tap() { hg_adb "$SERIAL" shell "input tap $1 $2" >/dev/null 2>&1; sleep "${3:-2}"; }

hg_adb "$SERIAL" shell 'input keyevent KEYCODE_WAKEUP' >/dev/null 2>&1
hg_adb "$SERIAL" shell 'wm dismiss-keyguard' >/dev/null 2>&1

echo "1. developer options"
hg_adb "$SERIAL" shell 'am start -n com.android.settings/.Settings\$DevelopmentSettingsDashboardActivity' >/dev/null 2>&1
sleep 3

echo "2. locate 'Wireless debugging' (scrolling as needed)"
ROW=""
for i in $(seq 1 10); do
  ROW="$(find_text 'wireless debugging')"
  [[ -n "$ROW" ]] && break
  hg_adb "$SERIAL" shell 'input swipe 360 1200 360 500 300' >/dev/null 2>&1
  sleep 1
done
[[ -n "$ROW" ]] || hg_die "could not find the 'Wireless debugging' row in Developer options" 1
read -r x y _ <<<"$ROW"
echo "   tapping it at ($x,$y)"
tap "$x" "$y" 3

echo "3. read the toggle state"
STATE="$(find_text 'Off')$(find_text 'On')"
TOG="$(find_text 'Off')"
if [[ -n "$TOG" ]]; then
  read -r tx ty _ <<<"$TOG"
  echo "   toggle reads Off -> turning it on at ($tx,$ty)"
  tap "$tx" "$ty" 3
else
  echo "   toggle already reads On"
fi

echo "4. network-trust dialog (re-applying your standing choice: ALLOW, checkbox unticked)"
ALLOW="$(find_text 'ALLOW')"
if [[ -n "$ALLOW" ]]; then
  read -r ax ay _ <<<"$ALLOW"
  CK="$(find_text 'Always allow on this network')"
  echo "   dialog present; tapping ALLOW at ($ax,$ay)"
  [[ -n "$CK" ]] && echo "   (checkbox present but deliberately NOT touched)"
  tap "$ax" "$ay" 4
else
  echo "   no trust dialog appeared (already trusted, or the toggle was already on)"
fi

echo "5. read the endpoint from the Wireless debugging screen"
# If we navigated into Device details, come back.
EP=""
for i in 1 2 3; do
  EP="$(dump | grep -oE '192\.168\.[0-9]+\.[0-9]+:[0-9]{4,5}' | head -1)"
  [[ -n "$EP" ]] && break
  hg_adb "$SERIAL" shell 'input keyevent KEYCODE_BACK' >/dev/null 2>&1
  sleep 2
done
[[ -n "$EP" ]] || hg_die "could not read an IP:PORT from the Wireless debugging screen" 1

echo
echo "WIRELESS DEBUGGING ENABLED"
echo "  endpoint        : $EP"
echo "  adb_wifi_enabled: $(hg_adb "$SERIAL" shell 'settings get global adb_wifi_enabled' | tr -d '\r')"
echo "  paired devices  : $(find_text 'swarna-sekhar-dhar@SS-DHAR' | head -1)"
echo
echo "next: ./edge_dev_connect.sh --port ${EP##*:}"
