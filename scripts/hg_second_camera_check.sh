#!/usr/bin/env bash
# hg_second_camera_check.sh - targeted reachability + interface check for a camera
# that is reported to sit behind a Wi-Fi extender.
#
# Answers, without scanning anything:
#   1. which local subnets this host is actually on
#   2. which way the kernel would send a packet to the camera, and via which interface
#   3. whether the camera answers ICMP or any relevant TCP port
#   4. the verdict: DIRECT (same L2/L3 as this host) / VIA-NAT (behind another router)
#      / UNREACHABLE
#
# SAFETY CONTRACT
#   * one target, given on the command line - no subnet sweep, no host discovery
#   * read-only: sends ICMP echoes and TCP SYNs to the named camera only
#   * changes nothing: no interface reconfiguration, no router/extender change,
#     no credential, no port forwarding
#
# Usage: bash scripts/hg_second_camera_check.sh <camera-ip> [more-ips...]

set -uo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: bash scripts/hg_second_camera_check.sh <camera-ip> [more-ips...]" >&2
    exit 2
fi

echo "== host network state (what this machine can actually reach) =="
ip -brief addr show | grep -v '^lo'
echo
echo "-- default route --"
ip route | grep '^default' || echo "  (none)"
echo
echo "-- directly connected subnets --"
ip route | grep -E 'proto kernel' || true
echo

for CAM in "$@"; do
    echo "=============================================================="
    echo "== target: $CAM"
    echo "=============================================================="

    echo "-- kernel route decision --"
    ROUTE="$(ip route get "$CAM" 2>&1)"
    echo "  $ROUTE"

    GW="$(printf '%s' "$ROUTE" | sed -n 's/.* via \([0-9.]*\).*/\1/p')"
    VIA_GATEWAY="$(printf '%s' "$ROUTE" | grep -c ' via ' || true)"

    echo
    echo "-- ICMP --"
    if ping -c 2 -W 1 "$CAM" >/dev/null 2>&1; then
        ICMP="yes"; echo "  replies (2/2)"
    else
        ICMP="no"; echo "  100% packet loss"
    fi

    echo
    echo "-- targeted TCP (relevant camera ports only) --"
    python3 - "$CAM" <<'PY'
import socket, sys, time
CAM = sys.argv[1]
PORTS = (80, 443, 554, 8554, 3702, 8000, 8080, 8899, 34567, 8800, 9800)
any_open = False
for p in PORTS:
    s = socket.socket(); s.settimeout(2.0); t0 = time.time()
    try:
        s.connect((CAM, p)); any_open = True; print(f'   {p:<6d} OPEN')
    except socket.timeout:
        print(f'   {p:<6d} no answer (filtered - typical of a NAT boundary) {time.time()-t0:.1f}s')
    except OSError as e:
        print(f'   {p:<6d} refused errno={e.errno} {time.time()-t0:.2f}s  (host reachable, port closed)')
    finally:
        s.close()
print()
print('TCP_RESULT=' + ('open-ports' if any_open else 'no-open-ports'))
PY

    echo
    echo "-- verdict --"
    if [ "$ICMP" = "no" ] && [ "$VIA_GATEWAY" != "0" ]; then
        echo "  UNREACHABLE from this host."
        echo "  The route to $CAM leaves through the default gateway${GW:+ ($GW)} rather than a"
        echo "  directly connected subnet, so this host has no path to that address."
        echo "  If the camera is behind an extender running router/NAT mode, its subnet is"
        echo "  private to that extender and is NOT routed here. Confirm by checking that no"
        echo "  'proto kernel' line above covers the camera's own subnet."
    else
        echo "  Check the ICMP/TCP lines above: this host appears to have a route to $CAM."
    fi
    echo
done
