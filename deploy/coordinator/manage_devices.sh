#!/usr/bin/env bash
# HomeGuardian AI — Coordinator Device & Secret Management Utility
#
# Milestone: HG-NET-02 (Secure Remote MCP Coordinator)
#
# Usage:
#   manage_devices.sh enroll <device_id> [device_name]
#   manage_devices.sh revoke <device_id>
#   manage_devices.sh list
#   manage_devices.sh rotate-client-token

set -euo pipefail

DEVICES_FILE="${COORD_DEVICES_FILE:-/etc/homeguardian-coordinator/devices.json}"
CLIENT_TOKEN_FILE="${COORD_CLIENT_TOKEN_FILE:-/etc/homeguardian-coordinator/client_token.secret}"

function ensure_dir() {
  local dir
  dir=$(dirname "$DEVICES_FILE")
  if [ ! -d "$dir" ]; then
    mkdir -p "$dir"
    chmod 0700 "$dir"
  fi
  if [ ! -f "$DEVICES_FILE" ]; then
    echo "{}" > "$DEVICES_FILE"
    chmod 0600 "$DEVICES_FILE"
  fi
}

function reload_service() {
  if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet coordinator.service 2>/dev/null; then
    systemctl reload coordinator.service || true
  elif pgrep -f "coordinator/service.js" >/dev/null 2>&1; then
    pkill -HUP -f "coordinator/service.js" || true
  fi
}

function cmd_enroll() {
  local device_id="${1:-}"
  local device_name="${2:-Xiaomi Mi A3}"

  if [ -z "$device_id" ]; then
    echo "Error: device_id required." >&2
    echo "Usage: $0 enroll <device_id> [device_name]" >&2
    exit 1
  fi

  ensure_dir

  local auth_token
  auth_token=$(openssl rand -hex 32)

  node -e "
    const fs = require('fs');
    const file = process.argv[1];
    const devId = process.argv[2];
    const devName = process.argv[3];
    const token = process.argv[4];

    let data = {};
    if (fs.existsSync(file)) {
      try { data = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (_) {}
    }
    data[devId] = {
      authToken: token,
      name: devName,
      revoked: false,
      enrolledAt: Date.now()
    };
    fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n', { mode: 0o600 });
  " "$DEVICES_FILE" "$device_id" "$device_name" "$auth_token"

  chmod 0600 "$DEVICES_FILE"
  reload_service

  echo "================================================================"
  echo "  Device Successfully Enrolled in Coordinator Registry"
  echo "================================================================"
  echo "  Device ID:    $device_id"
  echo "  Device Name:  $device_name"
  echo "  Auth Token:   $auth_token"
  echo "  Registry:     $DEVICES_FILE"
  echo "================================================================"
  echo "  IMPORTANT: Securely provision this token to the edge node."
  echo "  This secret will NOT be printed again."
  echo "================================================================"
}

function cmd_revoke() {
  local device_id="${1:-}"
  if [ -z "$device_id" ]; then
    echo "Error: device_id required." >&2
    echo "Usage: $0 revoke <device_id>" >&2
    exit 1
  fi

  if [ ! -f "$DEVICES_FILE" ]; then
    echo "Error: registry file $DEVICES_FILE does not exist." >&2
    exit 1
  fi

  local found
  found=$(node -e "
    const fs = require('fs');
    const file = process.argv[1];
    const devId = process.argv[2];
    let data = {};
    try { data = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (_) {}
    if (data[devId]) {
      data[devId].revoked = true;
      data[devId].revokedAt = Date.now();
      fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n', { mode: 0o600 });
      console.log('1');
    } else {
      console.log('0');
    }
  " "$DEVICES_FILE" "$device_id")

  if [ "$found" = "1" ]; then
    reload_service
    echo "Successfully revoked device '$device_id'. Active connections severed with 4403."
  else
    echo "Error: device '$device_id' not found in registry." >&2
    exit 1
  fi
}

function cmd_list() {
  if [ ! -f "$DEVICES_FILE" ]; then
    echo "No devices enrolled (registry $DEVICES_FILE does not exist)."
    return
  fi

  node -e "
    const fs = require('fs');
    const file = process.argv[1];
    let data = {};
    try { data = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (_) {}
    const entries = Object.entries(data);
    if (entries.length === 0) {
      console.log('No devices enrolled.');
      process.exit(0);
    }
    console.log('Enrolled Devices:');
    console.log('--------------------------------------------------------------------------------');
    console.log('DEVICE ID'.padEnd(28) + 'NAME'.padEnd(24) + 'STATUS'.padEnd(12) + 'ENROLLED');
    console.log('--------------------------------------------------------------------------------');
    for (const [id, dev] of entries) {
      const status = dev.revoked ? 'REVOKED' : 'AUTHORIZED';
      const enrolled = dev.enrolledAt ? new Date(dev.enrolledAt).toISOString().split('T')[0] : 'N/A';
      console.log(id.padEnd(28) + (dev.name || 'N/A').padEnd(24) + status.padEnd(12) + enrolled);
    }
  " "$DEVICES_FILE"
}

function cmd_rotate_client_token() {
  local dir
  dir=$(dirname "$CLIENT_TOKEN_FILE")
  if [ ! -d "$dir" ]; then
    mkdir -p "$dir"
    chmod 0700 "$dir"
  fi

  local new_token
  new_token=$(openssl rand -hex 32)
  echo "$new_token" > "$CLIENT_TOKEN_FILE"
  chmod 0600 "$CLIENT_TOKEN_FILE"
  reload_service

  echo "================================================================"
  echo "  Client Ingress Bearer Token Rotated"
  echo "================================================================"
  echo "  New Token: $new_token"
  echo "  Saved to:  $CLIENT_TOKEN_FILE"
  echo "================================================================"
}

case "${1:-}" in
  enroll)
    shift
    cmd_enroll "$@"
    ;;
  revoke)
    shift
    cmd_revoke "$@"
    ;;
  list)
    cmd_list
    ;;
  rotate-client-token)
    cmd_rotate_client_token
    ;;
  *)
    echo "HomeGuardian Coordinator Device Management"
    echo "Usage:"
    echo "  $0 enroll <device_id> [device_name]"
    echo "  $0 revoke <device_id>"
    echo "  $0 list"
    echo "  $0 rotate-client-token"
    exit 1
    ;;
esac
