#!/usr/bin/env bash
# Connect to the Mi A3 over authorized Wi-Fi, resilient to BOTH an IP change and a
# rotating debugging port.
#
#   edge_dev_connect.sh                 # layered discovery
#   edge_dev_connect.sh --port N        # pin the port
#   edge_dev_connect.sh --wait N        # extra patience passes (N x 30s)
#   edge_dev_connect.sh --status        # report state only
#
# WHY LAYERED - all measured on this LAN, none of it assumed away:
#   * the wireless-debugging PORT ROTATES when Wi-Fi reconnects (41639 -> 40815);
#   * after a Wi-Fi reconnect there is a window of minutes with NOTHING listening
#     (an immediate TCP RST: a live stack, no listener), so one probe is a false
#     negative and retries are mandatory;
#   * mDNS answers only INTERMITTENTLY: a raw multicast query to
#     _adb-tls-connect._tcp.local returned 8 records from the phone on one attempt
#     and nothing on the next four. So mDNS is ONE candidate source, never the only;
#   * `adb mdns services` lists nothing here even when the phone does answer - adb's
#     own mDNS client is the broken part, hence the Python resolver.
#
# Candidate sources, each VERIFIED by identity before acceptance:
#   1. --port   2. remembered endpoint   3. mDNS   4. bounded port probe (fallback)
#
# Never uses `adb tcpip 5555` (plaintext, unauthenticated, all-interfaces).
# Exit: 0 ok, 3 not found, 4 not paired, 8 no network path.

source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PIN_PORT=""; MODE="connect"; PATIENCE="${HG_EDGE_PATIENCE:-1}"
while (( $# )); do
  case "$1" in
    --port)   PIN_PORT="$2"; shift 2 ;;
    --wait)   PATIENCE="$2"; shift 2 ;;
    --status) MODE="status"; shift ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) hg_die "unknown argument: $1" 1 ;;
  esac
done

hg_net_ok() { ping -c 2 -W 2 "$1" >/dev/null 2>&1; }

report_status() {
  echo "wireless discovery state"
  echo "  remembered endpoint : $([[ -f "$(hg_target_file)" ]] && cat "$(hg_target_file)" || echo '<none>')"
  echo "  LAN address         : $HG_EDGE_HOST   reachable: $(hg_net_ok "$HG_EDGE_HOST" && echo yes || echo NO)"
  echo "  adb mdns services   : $(timeout 15 adb mdns services 2>/dev/null | wc -l) lines (0 = adb's client finds nothing)"
  echo "  adb devices         :"; adb devices -l | sed 's/^/    /'
  echo "  python mDNS resolver:"; timeout 25 python3 "$HERE/edge_dev_mdns_resolve.py" 2>/dev/null | sed 's/^/    /'
}
[[ "$MODE" == "status" ]] && { report_status; exit 0; }

hg_net_ok "$HG_EDGE_HOST" || hg_die "cannot reach $HG_EDGE_HOST on the LAN (phone off Wi-Fi, or wrong address)" 8

CANDS=()
add_cand() {
  local c="$1"; [[ -n "$c" ]] || return 0
  local x; for x in ${CANDS[@]+"${CANDS[@]}"}; do [[ "$x" == "$c" ]] && return 0; done
  CANDS+=("$c")
}

try_candidates() {
  local ep serial out
  for ep in ${CANDS[@]+"${CANDS[@]}"}; do
    [[ -n "$ep" ]] || continue
    hg_log "trying $ep"
    out="$(timeout 25 adb connect "$ep" 2>&1)"
    if grep -qiE 'already connected|connected to' <<<"$out"; then
      serial="$(adb devices -l | awk -v e="${ep%%:*}" '$0 ~ e && $2=="device" {print $1}' | head -1)"
      if [[ -z "$serial" ]]; then sleep 2; serial="$(adb devices -l | awk -v e="${ep%%:*}" '$0 ~ e && $2=="device" {print $1}' | head -1)"; fi
      if [[ -n "$serial" ]] && hg_assert_mi_a3 "$serial" quiet 2>/dev/null; then
        OK="$serial"; return 0
      fi
      hg_log "  $ep answered but the identity check failed - ignoring"
    else
      hg_log "  $(tr '\n' ' ' <<<"$out")"
      grep -qiE 'failed to authenticate|cannot authenticate' <<<"$out" \
        && hg_log "  -> this host key is not trusted; pairing is required (edge_dev_pair.sh)"
    fi
  done
  return 1
}

probe_ports() {
  timeout 240 python3 "$HERE/edge_dev_discover_port.py" "$HG_EDGE_HOST" 2>/dev/null \
    | grep -oE 'open:.*' | sed 's/open: *//'
}

if [[ -n "$PIN_PORT" ]]; then
  add_cand "$HG_EDGE_HOST:$PIN_PORT"
else
  [[ -f "$(hg_target_file)" ]] && add_cand "$(head -1 "$(hg_target_file)")"
  echo "  mDNS resolution (candidate source 3)..."
  while read -r line; do add_cand "$(awk '{print $1}' <<<"$line")"; done \
    < <(timeout 25 python3 "$HERE/edge_dev_mdns_resolve.py" --serial "$HG_EDGE_SERIAL" 2>/dev/null || true)
fi

OK=""
try_candidates || true

if [[ -z "$OK" ]]; then
  echo "  mDNS/remembered candidates did not verify - probing the phone's ports (fallback 4)"
  for p in $(probe_ports); do add_cand "$HG_EDGE_HOST:$p"; done
  try_candidates || true
fi

if [[ -z "$OK" && "$PATIENCE" -gt 0 ]]; then
  hg_log "nothing yet; retrying for ${PATIENCE} x 30s (the post-reconnect window has no listener)"
  for i in $(seq 1 "$PATIENCE"); do
    sleep 30
    hg_log "retry $i"
    CANDS=()
    for p in $(probe_ports); do add_cand "$HG_EDGE_HOST:$p"; done
    while read -r line; do add_cand "$(awk '{print $1}' <<<"$line")"; done \
      < <(timeout 25 python3 "$HERE/edge_dev_mdns_resolve.py" --serial "$HG_EDGE_SERIAL" 2>/dev/null || true)
    try_candidates && break
  done
fi

if [[ -z "$OK" ]]; then
  cat >&2 <<EOF
No verified Mi A3 reachable over Wi-Fi. In order of likelihood:
  1. Wireless Debugging is OFF. It CANNOT be enabled from adb - the framework resets
     'settings put global adb_wifi_enabled'. Use edge_dev_enable_wireless.sh with a
     temporary USB cable, or toggle it on the phone:
       Settings > System > Developer options > Wireless debugging
  2. Just after a Wi-Fi reconnect the listener is absent for a while. Re-run with
     --wait 4 and let it run.
  3. The port shown on the phone's Wireless debugging screen can be pinned: --port N
  4. This host's key is already trusted, so a re-pair is normally NOT needed.
EOF
  exit 3
fi

hg_remember_target "$OK"
echo
hg_assert_mi_a3 "$OK"
cat <<EOF

wireless ADB established
  serial     : $OK
  transport  : $(adb devices -l | awk -v s="$OK" '$1==s')
  remembered : $(cat "$(hg_target_file)")
  MCP stays  : 127.0.0.1:$HG_EDGE_MCP_PORT on the device (never exposed)

Next: ./edge_dev_deploy.sh   ./edge_dev_status.sh   ./edge_dev_test.sh
EOF
