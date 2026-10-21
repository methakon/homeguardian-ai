#!/usr/bin/env bash
# HG-REAL-CPP-ALEXA-HUB-11 / HG-CPP-REAL-HUB-RETRY-16 — LOCAL HUB RUNBOOK (receiver side, C++ lane).
#
# WHY THIS FILE EXISTS: the retry asked for an executable local runbook, because the deployed stack has NO
# reachable receiver endpoint. Evidence from the actual host, not from the compose file's intent:
#
#   docker ps                     -> docker-ingress-1, healthy, image f1c07e4841b5
#   docker inspect ... Ports      -> {}                      (nothing is published)
#   docker port docker-ingress-1   -> (empty)
#   </dev/tcp/127.0.0.1/8443      -> refused
#
# The ingress therefore has provisioned mTLS material and a healthy process, and is reachable from NOBODY
# outside its own container network. This runbook brings up an equivalent stack with a port published ON
# LOOPBACK, which is the smallest step that makes the receiver testable without opening a network boundary.
#
# STEPS 3-4 ARE THE BLOCKED ONES and are marked. Do not skip the marks and claim a live endpoint.
set -uo pipefail
REPO="${HG_REPO:-$HOME/projects/homeguardian-ai}"
WT="${HG_WORKTREE:-$REPO-worktrees/hermes-v380-09}"
CERTS="${HG_CERTS_DIR:-$HOME/hg_h03/deploy-runtime}"      # ca/ devices/ server.crt server.key ingress.json
say() { printf '\n== %s\n' "$*"; }
run() { printf '   $ %s\n' "$*"; [ "${HG_RUN:-0}" = "1" ] && eval "$@" || true; }

say "0. what this proves"
echo "   An mTLS ingress that a real device can reach, and the native C++ receiver behind it."
echo "   It does NOT prove camera media: see the camera section at the end."

say "1. prerequisites (all checked, nothing installed)"
for f in "$CERTS/server.crt" "$CERTS/server.key" "$CERTS/ingress.json" "$CERTS/devices.json"; do
  [ -f "$f" ] && echo "   ok   $f" || echo "   MISSING $f   <- provisioning stops here"
done
command -v docker >/dev/null && echo "   ok   docker" || echo "   MISSING docker"

say "2. build the receiver image — BLOCKED ON THE BRANCH, not on the host"
echo "   The canonical deploy/docker/Dockerfile:61 builds seven targets, three of which are NOT defined in"
echo "   this branch's CMakeLists: hg_device_ingress, hg_device_client, hg_ingress_probe."
run "cd '$REPO' && git log -1 --format=%h -- CMakeLists.txt"
run "cd '$WT' && cmake --build build --target hg_device_ingress"
echo "   Until this branch carries those targets, build the slice that this branch DOES define:"
run "cd '$WT' && docker build -f /tmp/hg-slice.Dockerfile -t hg-receiver-slice-test ."

say "3. start hub + ingress with a LOOPBACK-published port  (HG_RUN=1 to execute)"
echo "   Loopback only ON PURPOSE. Publishing on a LAN interface is a network-boundary change and belongs to"
echo "   the network owner (Hermes 1, #23) — do not do it to make a test pass."
run "cd '$REPO/deploy/docker' && cp -n secrets/creds.json.example secrets/creds.json"
run "cd '$REPO/deploy/docker' && HG_CERTS_DIR='$CERTS' docker compose -f docker-compose.yml -f docker-compose.loopback.yml up -d"
run "docker compose -f '$REPO/deploy/docker/docker-compose.yml' ps"

say "4. verify the receiver answers  (this is the gate for 'real-device ingress')"
run "docker port \"\$(docker ps --format '{{.Names}}' | grep -i ingress | head -1)\""
run "bash '$REPO/scripts/verify_ingress_healthcheck.sh'"
run "bash '$REPO/scripts/verify_detection_end_to_end.sh'"

say "5. point ONE approved device at it (Hermes 2's lane — do not do this from here)"
echo "   Android currently reports NO verified reachable hub endpoint, and its frames are 4096-byte"
echo "   observations/features, NOT raw video/audio. The device needs: the hub's address, a device entry in"
echo "   devices.json, and consent. The Mi A3 harness is a fresh install pending security approval."

say "6. cameras: the genuine-observation blocker, with evidence"
echo "   192.168.1.10  pings, 8800/9800 OPEN (vendor path, closed as unsupported), 554/8899 CLOSED"
echo "   192.168.1.19  no ping, all ports closed    192.168.1.20  no ping, all ports closed"
echo "   So no camera can produce a genuine observation today: the only live one answers on the path that was"
echo "   closed, and its standard RTSP/ONVIF ports are shut despite the ONVIF toggle."

say "7. stop"
run "cd '$REPO/deploy/docker' && docker compose down"
