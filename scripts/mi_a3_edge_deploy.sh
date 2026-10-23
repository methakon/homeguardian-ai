#!/usr/bin/env bash
# mi_a3_edge_deploy.sh — push the MCP server to the Mi A3 and start it
# loopback-only, measuring cold-start latency.
#
# Safety: this script
#   * touches ONLY /data/local/tmp/hg_edge (device-local scratch);
#   * never installs or removes a package, never touches /system, never wipes;
#   * starts the server on 127.0.0.1 only - HG_MCP_ALLOW_REMOTE is never set, and
#     the server itself refuses a non-loopback bind without it;
#   * generates the bearer credential ON THE DEVICE at mode 0600 so no secret is
#     ever stored in this repository or on the host disk.
#
# Usage: scripts/mi_a3_edge_deploy.sh [serial]
set -uo pipefail

SERIAL="${1:-edd53dbd04de}"
DEV_DIR=/data/local/tmp/hg_edge
PORT="${HG_MCP_PORT:-8790}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$ROOT/deploy/mi_a3_edge"

adbs() { timeout 60 adb -s "$SERIAL" "$@"; }

[ -x "$STAGE/hg_mcp_server" ] || { echo "run scripts/mi_a3_edge_build.sh first"; exit 1; }

echo "==> device: $(adbs shell getprop ro.product.model | tr -d '\r') / $(adbs shell getprop ro.build.version.release | tr -d '\r') / $(adbs shell getprop ro.product.cpu.abi | tr -d '\r')"

echo "==> stopping any previous instance"
# `pkill -f <abs path>` does NOT match: the process is started with a relative
# argv[0] after `cd`, so the absolute path never appears in /proc/<pid>/cmdline.
# pidof matches the executable name regardless of how it was invoked.
adbs shell 'for p in $(pidof hg_mcp_server); do kill "$p"; done' 2>/dev/null
for _ in $(seq 1 30); do
  adbs shell 'pidof hg_mcp_server' 2>/dev/null | grep -q . || break
  sleep 0.2
done
if adbs shell 'pidof hg_mcp_server' 2>/dev/null | grep -q .; then
  echo "    still alive after SIGTERM -> SIGKILL"
  adbs shell 'for p in $(pidof hg_mcp_server); do kill -9 "$p"; done' 2>/dev/null
  sleep 1
fi
adbs shell 'pidof hg_mcp_server' 2>/dev/null | grep -q . && echo "    WARNING: could not stop the old instance" || echo "    stopped"
sleep 1

echo "==> staging"
adbs shell "mkdir -p $DEV_DIR/logs $DEV_DIR/data"
adbs push "$STAGE/hg_mcp_server"      "$DEV_DIR/hg_mcp_server"    >/dev/null
adbs push "$STAGE/libc++_shared.so"   "$DEV_DIR/libc++_shared.so" >/dev/null
adbs push "$STAGE/hg_config.json"     "$DEV_DIR/hg_config.json"   >/dev/null
adbs shell "chmod 755 $DEV_DIR/hg_mcp_server; chmod 644 $DEV_DIR/libc++_shared.so $DEV_DIR/hg_config.json"

echo "==> provisioning credential (device-side, 0600, owner-only)"
adbs shell "cd $DEV_DIR && umask 077 && TOK=\$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n') && \
  printf '{\"%s\":{\"principal\":\"mi-a3-edge\",\"allow_status\":true,\"allow_alert_summary\":true}}\n' \"\$TOK\" > mcp_credentials.json && \
  chmod 600 mcp_credentials.json && ls -l mcp_credentials.json"

echo "==> verifying fail-closed: a bad credential path must exit 3"
adbs shell "cd $DEV_DIR && LD_LIBRARY_PATH=$DEV_DIR ./hg_mcp_server hg_config.json /nonexistent.json 2>&1; echo EXIT=\$?" | tail -2

echo "==> cold start (timed to first listen)"
T0=$(date +%s%N)
adbs shell "cd $DEV_DIR && nohup setsid env LD_LIBRARY_PATH=$DEV_DIR ./hg_mcp_server hg_config.json mcp_credentials.json > logs/hg_mcp.out 2>&1 < /dev/null & echo started" >/dev/null
for _ in $(seq 1 100); do
  if adbs shell "cat /proc/net/tcp" 2>/dev/null | grep -qi ":$(printf '%04X' "$PORT")"; then break; fi
  sleep 0.1
done
T1=$(date +%s%N)
echo "==> listening after $(( (T1-T0)/1000000 )) ms"

echo "==> bind check (0100007F = 127.0.0.1 loopback; 00000000 would be all-interfaces)"
adbs shell "cat /proc/net/tcp" | awk -v p="$(printf '%04X' "$PORT")" '$2 ~ ":"p {print "    " $2, "state="$4}'
echo "==> process"
adbs shell "ps -A -o PID,USER,RSS,VSZ,STAT,NAME | grep -E 'hg_mcp|PID'"
echo "==> server log"
adbs shell "cat $DEV_DIR/logs/hg_mcp.out"
echo
echo "Next: scripts/mi_a3_edge_verify.sh   (functional MCP tests over adb forward)"
