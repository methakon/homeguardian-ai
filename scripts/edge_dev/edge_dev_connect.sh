#!/usr/bin/env bash
# Connect to the Mi A3 over authorized Wi-Fi (Android 11 native TLS Wireless Debugging).
#
#   edge_dev_connect.sh            # discover + connect + verify identity
#   edge_dev_connect.sh --port N   # pin the connect port explicitly
#   edge_dev_connect.sh --status   # report current wireless state only
#
# Does NOT use legacy `adb tcpip 5555` (plaintext, no pairing, listens on all
# interfaces). This uses `adb connect` to the TLS endpoint only.
#
# Exit: 0 ok, 3 not found, 4 not paired, 8 no network path.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

PIN_PORT=""
MODE="connect"
while (( $# )); do
  case "$1" in
    --port)   PIN_PORT="$2"; shift 2 ;;
    --status) MODE="status"; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) hg_die "unknown argument: $1" 1 ;;
  esac
done

# --- 1. is the phone on the network at all? ---------------------------------
hg_net_ok() {
  ping -c 2 -W 2 "$HG_EDGE_HOST" >/dev/null 2>&1
}

report_status() {
  echo "wireless debugging state"
  echo "  expected host : $HG_EDGE_HOST"
  echo "  reachable     : $(hg_net_ok && echo yes || echo NO)"
  echo "  adb mdns      :"
  timeout 15 adb mdns services 2>&1 | sed 's/^/    /'
  echo "  adb devices   :"
  adb devices -l | sed 's/^/    /'
  echo "  paired keys   :"
  ls -1 ~/.android/adbkey* 2>/dev/null | sed 's/^/    /'
}

if [[ "$MODE" == "status" ]]; then report_status; exit 0; fi

hg_net_ok || hg_die "cannot reach $HG_EDGE_HOST - is the phone on the same Wi-Fi (and not asleep)?" 8

# --- 2. find candidate TLS endpoints ----------------------------------------
# Preferred: mDNS, which is how Android advertises the rotating port.
# `_adb-tls-connect._tcp` is the port for `adb connect` on an already-paired host;
# `_adb-tls-pairing._tcp` exists only while the pairing dialog is open.
find_mdns() {
  timeout 20 adb mdns services 2>/dev/null \
    | awk '/_adb-tls-connect\._tcp/ {print $3}' | sort -u
}
find_mdns_pairing() {
  timeout 20 adb mdns services 2>/dev/null \
    | awk '/_adb-tls-pairing\._tcp/ {print $3}' | sort -u
}

CANDIDATES=""
if [[ -n "$PIN_PORT" ]]; then
  CANDIDATES="$HG_EDGE_HOST:$PIN_PORT"
else
  CANDIDATES="$(find_mdns || true)"
  if [[ -z "$CANDIDATES" ]]; then
    # mDNS is filtered by many consumer APs - measured to be the case here, where
    # `adb mdns services` lists nothing even though the phone is advertising.
    # Fall back to the endpoint verified last time: the port only rotates when
    # Wireless Debugging is toggled or the device reboots, so it usually still
    # works. This is the practical "discover the paired device" path on this LAN.
    rf="$(hg_target_file)"
    if [[ -f "$rf" ]]; then
      LAST="$(head -1 "$rf")"
      if [[ -n "$LAST" ]]; then
        hg_log "mDNS found nothing; retrying the last verified endpoint ($LAST)"
        CANDIDATES="$LAST"
      fi
    fi
  fi
fi

if [[ -z "$CANDIDATES" ]]; then
  local_hint="$(find_mdns_pairing || true)"
  hg_log "no _adb-tls-connect service discovered via mDNS"
  if [[ -n "$local_hint" ]]; then
    hg_log "BUT a pairing service IS advertising: $local_hint"
    hg_log "-> the phone is still waiting to be paired; run edge_dev_pair.sh first"
    exit 4
  fi
  cat >&2 <<EOF
No wireless-debugging endpoint found. Most likely causes, in order:
  1. 'Wireless debugging' is OFF. It cannot be enabled over adb - Android 11 only
     exposes the toggle in the UI (a 'settings put global adb_wifi_enabled 1' was
     tried and the framework immediately reset it back to 0). On the phone:
       Settings > System > Developer options > Wireless debugging > toggle ON
  2. mDNS/multicast is blocked between this host and the phone by the AP. Then pass
     the port shown on the Wireless debugging screen explicitly:
       $0 --port <PORT_FROM_PHONE_SCREEN>
  3. The phone dropped off Wi-Fi. Check: the Wireless debugging screen shows an
     'IP address & Port'. Host expects the phone at $HG_EDGE_HOST.
EOF
  exit 3
fi

# --- 3. connect to each candidate until one authorises + verifies ------------
OK=""
while read -r EP; do
  [[ -n "$EP" ]] || continue
  hg_log "trying $EP"
  out="$(timeout 25 adb connect "$EP" 2>&1)" && rc=0 || rc=$?
  hg_log "  adb connect rc=$rc: $(tr '\n' ' ' <<<"$out")"
  if grep -qiE 'already connected|connected to' <<<"$out"; then
    # Resolve which serial now answers on that endpoint and verify it.
    SERIAL="$(adb devices -l | awk -v e="${EP%%:*}" '$0 ~ e {print $1}' | head -1)"
    if [[ -z "$SERIAL" ]]; then
      # Transport present but serial not yet resolved - re-query.
      sleep 2
      SERIAL="$(adb devices -l | awk -v e="${EP%%:*}" '$0 ~ e {print $1}' | head -1)"
    fi
    if [[ -n "$SERIAL" ]]; then
      if hg_assert_mi_a3 "$SERIAL" quiet 2>/dev/null; then
        OK="$SERIAL"
        hg_log "connected and identity-verified: $SERIAL @ $EP"
        break
      else
        hg_log "  WARNING: $EP answered but the target is NOT a verified Mi A3 - ignoring"
      fi
    fi
  fi
  grep -qiE 'failed to authenticate|cannot authenticate' <<<"$out" && hg_log "  -> this host key is not trusted yet; pairing is required"
done < <(tr ' ' '\n' <<<"$CANDIDATES")

if [[ -z "$OK" ]]; then
  hg_die "no verified Mi A3 reachable over Wi-Fi (see reasons above)" 4
fi

# Remember it, so the other scripts work once the USB cable is gone and the
# device serial is the wireless endpoint rather than the USB serial.
hg_remember_target "$OK"

# --- 4. prove it is the real device, over the wireless transport -------------
echo
hg_assert_mi_a3 "$OK"
cat <<EOF

wireless ADB established
  serial      : $OK
  transport   : $(adb devices -l | awk -v s="$OK" '$1==s')
  phone LAN   : $HG_EDGE_HOST
  MCP stays   : 127.0.0.1:$HG_EDGE_MCP_PORT on the device (never exposed)

Next: ./edge_dev_deploy.sh   ./edge_dev_status.sh   ./edge_dev_test.sh
EOF
