#!/usr/bin/env bash
# HG-LAN-MTLS-07 - WireGuard workstation handshake test: REVERSIBLE, LEAST-PRIVILEGE, GATED.
#
# Purpose
#   Prove a REAL WireGuard handshake end-to-end from this workstation (the only source the OCI NSG
#   currently admits) without changing Oracle, DNS, the router or the NSG. The tunnel is brought up
#   with overlay-only scope and torn down again, restoring the previous network state exactly.
#
# Modes
#   preflight (default)  prerequisites + snapshots. CHANGES NOTHING. Safe to run at any time.
#   up                   bring the tunnel up (requires --execute AND a peer config).
#   verify               assert least-privilege and a real handshake while up.
#   down                 tear down and prove the network state was restored.
#
# It will refuse to run any state-changing mode without --execute, and refuses outright in preflight
# if a prerequisite is missing, so a partial environment cannot half-apply a tunnel.
#
# NEVER commits or prints key material. The peer config/key stay outside the repository.
set -o pipefail

IFACE=wg0
ADDR=192.168.77.2/32                 # this workstation's overlay address (server side: /32 only)
OVERLAY_ONLY=192.168.77.0/24         # NOT 0.0.0.0/0 - a default-route hijack is the classic VPN mistake
ENDPOINT=${HG_WG_ENDPOINT:-161.33.146.238:51820}   # literal address: no DNS record exists
MTU=1420
STATE=${HG_WG_STATE:-$HOME/.hermes/cache/scratch/hg-wg-preflight}
CONF=${HG_WG_CONF:-$HOME/.wireguard/hg-workstation.conf}   # supplied by the operator; never tracked
DEADMAN_MIN=${HG_WG_DEADMAN_MIN:-15}
EXECUTE=0
MODE=preflight
PGM=$(basename "$0")

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    preflight|up|verify|down) MODE="$1" ;;
    --execute) EXECUTE=1 ;;
    -h|--help) usage ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
  shift
done

mkdir -p "$STATE"
say()  { printf '  %s\n' "$*"; }
pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; }
blk()  { printf '  [BLOCKED] %s\n' "$*"; }
need_execute() {
  [ "$EXECUTE" = 1 ] || { echo "refusing to change network state without --execute ($MODE)" >&2; exit 3; }
}

# ---------------------------------------------------------------- snapshots
# Volatile fields (DHCP lease timers) change on their own and must never be read as a network
# change, and an unreadable table must be reported NOT_TESTED - never compared as though it were
# measured, because a reversibility proof that passes vacuously is worse than no proof at all.
netsnap() { ip -4 route show; ip -br link show; ip -4 addr show | grep -vE 'valid_lft|preferred_lft'; }
natsnap() {
  if sudo -n iptables -t nat -S 2>/dev/null; then return 0; fi
  if [ "$(id -u)" = 0 ]; then iptables -t nat -S 2>/dev/null; return 0; fi
  echo "NOT_TESTED"
}
snapshot() {
  local tag=$1
  netsnap > "$STATE/net.$tag" 2>/dev/null
  natsnap > "$STATE/nat.$tag" 2>/dev/null
  sysctl -n net.ipv4.ip_forward > "$STATE/fwd.$tag" 2>/dev/null
  md5sum /etc/resolv.conf 2>/dev/null | awk '{print $1}' > "$STATE/resolv.$tag"
}
delta_since() {
  local tag=$1; local bad=0
  cmp -s "$STATE/net.$tag" <(netsnap) \
    && pass "routes/interfaces unchanged since $tag" || { fail "routes/interfaces DIFFER from $tag"; bad=1; }
  if grep -q NOT_TESTED "$STATE/nat.$tag" 2>/dev/null || ! natsnap | grep -q .; then
    echo "  [NOT_TESTED] nat table (no privilege to read it) - not claiming it is unchanged"
  elif cmp -s "$STATE/nat.$tag" <(natsnap); then
    pass "nat table unchanged since $tag (no masquerade added)"
  else fail "nat table DIFFERS from $tag"; bad=1; fi
  [ "$(sysctl -n net.ipv4.ip_forward 2>/dev/null)" = "$(cat "$STATE/fwd.$tag" 2>/dev/null)" ] \
    && pass "ip_forward unchanged" || { fail "ip_forward CHANGED"; bad=1; }
  if ! command -v wg >/dev/null; then return $bad; fi
  return $bad
}

# ---------------------------------------------------------------- preflight
preflight() {
  local blocked=0
  echo "== preflight: prerequisites =="
  command -v wg >/dev/null && pass "wg present ($(command -v wg))" || { blk "wg MISSING - install wireguard-tools"; blocked=1; }
  command -v wg-quick >/dev/null && pass "wg-quick present" || blk "wg-quick MISSING (audit only; this runbook uses wg+ip and does not need it)"
  if [ -d /sys/module/wireguard ] || modinfo wireguard >/dev/null 2>&1 || \
     grep -qE '(^| )wireguard( |$)' /proc/modules 2>/dev/null; then
    pass "kernel WireGuard support present"
  else
    blk "kernel WireGuard support NOT detected (module absent and not built in)"; blocked=1
  fi
  if sudo -n true 2>/dev/null; then pass "passwordless sudo available"
  elif [ "$(id -u)" = 0 ]; then pass "running as root"
  else blk "no passwordless sudo - bringing the interface up needs privilege"; blocked=1; fi
  if [ -f "$CONF" ]; then pass "peer config present ($CONF, mode $(stat -c %a "$CONF"))"
  else blk "no peer config at $CONF - the workstation key must be supplied (never committed)"; blocked=1; fi
  [ "$(stat -c %a "$CONF" 2>/dev/null)" = 600 ] || blk "peer config permissions are not 0600"
  if ip -br link show dev "$IFACE" >/dev/null 2>&1; then
    blk "$IFACE already exists - tear it down before a fresh test"
  else pass "no pre-existing $IFACE (clean slate)"; fi
  ip -4 route show default | grep -q . && pass "default route present: $(ip -4 route show default | head -1)" \
    || blk "no default route"
  echo "== preflight: endpoint =="
  local host=${ENDPOINT%:*}; local port=${ENDPOINT##*:}
  if command -v nc >/dev/null && nc -zuv -w3 "$host" "$port" >/dev/null 2>&1; then
    pass "udp $host:$port reachable (probe is advisory: wireshark-free UDP sends are unreliable)"
  else
    blk "udp $host:$port not confirmed - expected while the NSG pins the source, and note a phone on another network is dropped upstream"
  fi
  snapshot pre
  say "snapshot written to $STATE/net.pre (routes, interfaces, nat, ip_forward, resolv.conf)"
  echo "== preflight: least-privilege plan that WILL be asserted in 'verify' =="
  say "AllowedIPs = $OVERLAY_ONLY  (overlay only - NOT 0.0.0.0/0: no default-route hijack)"
  say "no 'DNS =' line            (no resolver takeover)"
  say "MTU $MTU, PersistentKeepalive 25, no PostUp/PostDown (no NAT, no forwarding)"
  say "server side stays AllowedIPs 192.168.77.2/32 (already the case)"
  echo
  if [ "$blocked" = 1 ]; then
    echo "  RESULT: BLOCKED - $MODE/up must not run until the [BLOCKED] items above are resolved."
    return 4
  fi
  echo "  RESULT: preflight clean - 'up --execute' is permitted."
  return 0
}

# ---------------------------------------------------------------- up / verify / down
up() {
  need_execute
  [ "$(preflight >/dev/null 2>&1; echo $?)" = 0 ] || { echo "refusing: preflight not clean" >&2; exit 4; }
  snapshot pre
  echo "== arming the dead-man switch: auto-teardown in ${DEADMAN_MIN}m =="
  # armed BEFORE the change, so a lost session cannot leave the tunnel up
  systemd-run --user --on-active="${DEADMAN_MIN}m" --unit="hg-wg-deadman-$$" \
    "$(readlink -f "$0")" down --execute >/dev/null 2>&1 \
    && say "dead-man armed (hg-wg-deadman-$$)" || say "WARN: dead-man not armed - tear down manually"
  echo "== bringing up $IFACE (least privilege) =="
  sudo ip link add "$IFACE" type wireguard || exit 5
  sudo wg setconf "$IFACE" "$CONF" || { sudo ip link del "$IFACE"; exit 5; }
  sudo ip -4 addr add "$ADDR" dev "$IFACE" || true
  sudo ip link set "$IFACE" mtu "$MTU" up || exit 5
  sudo ip route add "$OVERLAY_ONLY" dev "$IFACE" 2>/dev/null || true
  say "up. verifying a REAL handshake..."
  local hs=0
  for _ in $(seq 1 20); do
    if sudo wg show "$IFACE" latest-handshakes | grep -qvE '\s0$'; then hs=1; break; fi
    sleep 1
  done
  [ "$hs" = 1 ] && pass "handshake completed (real crypto over the real NSG path)" \
                || fail "no handshake within 20s - check the NSG source pinning and the peer key"
  sudo wg show "$IFACE" | grep -vE 'private' | sed 's/^/    /'
  echo "== least-privilege assertions =="
  verify
}

verify() {
  local bad=0
  delta_since pre || bad=1
  echo "== least-privilege =="
  local allowed; allowed=$(sudo wg show "$IFACE" allowed-ips 2>/dev/null | awk '{print $2}' | tr '\n' ' ')
  case "$allowed" in
    *0.0.0.0/0*) fail "AllowedIPs includes 0.0.0.0/0 - a full-tunnel hijack, not least privilege"; bad=1 ;;
    *) pass "AllowedIPs overlay-only: ${allowed:-<none>}" ;;
  esac
  grep -qE '^\s*DNS\s*=' "$CONF" 2>/dev/null && { fail "'DNS =' present: the tunnel would take over resolution"; bad=1; } \
                                            || pass "no DNS override in the config"
  if sudo iptables -t nat -S 2>/dev/null | grep -q MASQUERADE && ! iptables -t nat -S | grep -q MASQUERADE; then
    fail "a MASQUERADE rule appeared (this test must not NAT)"; bad=1
  else pass "no NAT/masquerade introduced"; fi
  ip -4 route show default | head -1 | sed 's/^/    default now: /'
  if command -v ping >/dev/null; then
    ping -c2 -W2 -I "$IFACE" 192.168.77.1 >/dev/null 2>&1 \
      && pass "overlay peer 192.168.77.1 reachable through $IFACE" \
      || say "overlay ping inconclusive (server may not answer ICMP on wg0 - not a failure by itself)"
  fi
  echo "  verify result: $([ $bad = 0 ] && echo PASS || echo FAIL)"
  return $bad
}

down() {
  need_execute
  snapshot post
  echo "== tearing down $IFACE =="
  sudo ip link del "$IFACE" 2>/dev/null && say "interface removed" || say "nothing to remove"
  systemctl --user list-units 'hg-wg-deadman-*' --no-legend 2>/dev/null | awk '{print $1}' | while read -r u; do
    systemctl --user stop "$u" 2>/dev/null; say "dead-man $u cancelled"
  done
  sudo systemctl reset-failed "hg-wg-deadman-$$" 2>/dev/null || true
  echo "== reversibility proof =="
  local bad=0
  ip -br link show dev "$IFACE" >/dev/null 2>&1 && { fail "$IFACE still present"; bad=1; } || pass "$IFACE gone"
  cmp -s "$STATE/net.pre" <(netsnap) \
    && pass "routes/interfaces restored EXACTLY to the pre-test snapshot" \
    || { fail "network state differs from the pre-test snapshot"; diff "$STATE/net.pre" \
         <(netsnap) | sed 's/^/      /' | head -20; bad=1; }
  if grep -q NOT_TESTED "$STATE/nat.pre" 2>/dev/null; then
    echo "  [NOT_TESTED] nat table could not be read before or after - NOT claimed as restored"
  elif cmp -s "$STATE/nat.pre" <(natsnap); then pass "nat table restored"
  else fail "nat table differs"; bad=1; fi
  [ "$(sysctl -n net.ipv4.ip_forward)" = "$(cat "$STATE/fwd.pre" 2>/dev/null)" ] \
    && pass "ip_forward restored" || { fail "ip_forward differs"; bad=1; }
  echo "  down result: $([ $bad = 0 ] && echo PASS || echo FAIL)"
  return $bad
}

case "$MODE" in
  preflight) preflight ;;
  up)        up ;;
  verify)    verify ;;
  down)      down ;;
esac
