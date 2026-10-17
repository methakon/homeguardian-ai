#!/usr/bin/env bash
# HomeGuardian AI — Coordinator Health & Resource Verification
#
# Milestone: HG-NET-02 (Secure Remote MCP Coordinator)
#
# Checks coordinator health, edge connection status, system resource usage,
# and verifies that co-hosted mail services (Postfix/Dovecot) remain healthy.

set -euo pipefail

BIND="${COORD_BIND:-10.42.0.1}"
PORT="${COORD_HTTP_PORT:-18995}"
BASE_URL="${1:-http://${BIND}:${PORT}}"

echo "================================================================"
echo "  HomeGuardian Remote MCP Coordinator — Health Check"
echo "================================================================"
echo "Timestamp: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "Target:    ${BASE_URL}/health"
echo "----------------------------------------------------------------"

# 1. Query Coordinator Health Endpoint
HEALTH_JSON=$(curl -s --connect-timeout 3 "${BASE_URL}/health" || echo "")

if [ -z "$HEALTH_JSON" ]; then
  # Fallback to loopback if 10.42.0.1 not reached
  if [ "$BIND" != "127.0.0.1" ]; then
    HEALTH_JSON=$(curl -s --connect-timeout 2 "http://127.0.0.1:${PORT}/health" || echo "")
  fi
fi

if [ -n "$HEALTH_JSON" ]; then
  STATUS=$(echo "$HEALTH_JSON" | grep -o '"status": *"[^"]*"' | head -n 1 | cut -d'"' -f4 || echo "UNKNOWN")
  ONLINE=$(echo "$HEALTH_JSON" | grep -o '"edge_online": *[^,}]*' | head -n 1 | cut -d':' -f2 | tr -d ' ' || echo "false")
  DEVICE_ID=$(echo "$HEALTH_JSON" | grep -o '"device_id": *"[^"]*"' | head -n 1 | cut -d'"' -f4 || echo "none")
  RSS_MB=$(echo "$HEALTH_JSON" | grep -o '"rss": *[^,}]*' | head -n 1 | cut -d':' -f2 | tr -d ' ' || echo "0")
  HEAP_MB=$(echo "$HEALTH_JSON" | grep -o '"heap_used": *[^,}]*' | head -n 1 | cut -d':' -f2 | tr -d ' ' || echo "0")
  UPTIME=$(echo "$HEALTH_JSON" | grep -o '"uptime_seconds": *[^,}]*' | head -n 1 | cut -d':' -f2 | tr -d ' ' || echo "0")

  echo "[Coordinator Service]  STATUS: ONLINE"
  echo "  - Process RSS:        ${RSS_MB} MB (systemd cap: 120 MB)"
  echo "  - V8 Heap Used:       ${HEAP_MB} MB (V8 cap: 64 MB)"
  echo "  - Service Uptime:     ${UPTIME} seconds"
  echo ""
  echo "[Edge Hardware State]  DEVICE STATUS: ${STATUS}"
  if [ "$ONLINE" = "true" ]; then
    echo "  - Mi A3 Hardware:     CONNECTED (device_id: ${DEVICE_ID})"
  else
    echo "  - Mi A3 Hardware:     DISCONNECTED / OFFLINE"
    echo "  - Mode:               LOCAL SYNTHETIC FIXTURE — MI A3 OFFLINE"
  fi
else
  echo "[Coordinator Service]  STATUS: OFFLINE / UNREACHABLE"
  echo "  Could not reach health endpoint at ${BASE_URL}/health"
fi

echo "----------------------------------------------------------------"
echo "[Host System & Resource Impact]"
if command -v free >/dev/null 2>&1; then
  free -h | awk 'NR<=2 {print "  " $0}'
fi

if command -v uptime >/dev/null 2>&1; then
  echo "  Load Average: $(uptime | awk -F'load average:' '{print $2}')"
fi

echo "----------------------------------------------------------------"
echo "[Co-Hosted Mail Services Integrity (Oracle VPS)]"
MAIL_HEALTHY=true
for svc in postfix dovecot opendkim; do
  if command -v systemctl >/dev/null 2>&1; then
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
      echo "  - $svc: ACTIVE / RUNNING"
    elif systemctl list-unit-files "$svc.service" >/dev/null 2>&1; then
      echo "  - $svc: INACTIVE"
    fi
  fi
done

echo "================================================================"
if [ -n "$HEALTH_JSON" ]; then
  exit 0
else
  exit 1
fi
