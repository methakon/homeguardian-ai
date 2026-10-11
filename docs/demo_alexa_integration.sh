#!/usr/bin/env bash
# HomeGuardian AI — local end-to-end Alexa integration demo (F22-30).
#
# Starts the real C++ bridge (hg_alexa_bridge) and drives it with newline-
# delimited JSON over stdio — the SAME boundary the JavaScript Alexa adapter
# uses. No network listener, no AWS, no credentials, no physical sensors.
#
# Prerequisites:
#   cmake -S . -B hg_build
#   cmake --build hg_build --target hg_alexa_bridge
#
# Usage:  bash docs/demo_alexa_integration.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$REPO_ROOT/hg_build/hg_alexa_bridge"
CONFIG="$REPO_ROOT/config/homeguardian.json"

if [[ ! -x "$BRIDGE" ]]; then
  echo "ERROR: bridge not built at $BRIDGE" >&2
  echo "Build it:  cmake -S . -B hg_build && cmake --build hg_build --target hg_alexa_bridge" >&2
  exit 1
fi

# Isolated temp workspace: test-only credentials + audit DB, never in the repo.
WORK="$(mktemp -d)"
CREDS="$WORK/creds.json"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

# Generate deterministic, TEST-ONLY credentials (not real secrets). The token
# values are arbitrary local fixtures; they grant read-only status/alert_summary.
cat > "$CREDS" <<'JSON'
{
  "demo-status":   {"principal":"demo","allow_status":true,  "allow_alert_summary":false, "expires_at_ms":0},
  "demo-full":     {"principal":"demo","allow_status":true,  "allow_alert_summary":true,  "expires_at_ms":0}
}
JSON

echo "=== HomeGuardian AI — local Alexa integration demo ==="
echo "bridge:  $BRIDGE"
echo "config:  $CONFIG"
echo "workdir: $WORK (isolated; removed on exit)"
echo

echo "--- 1. Authenticated status request (read-only) ---"
printf '%s\n' '{"version":1,"operation":"status","credential":"demo-full"}' 'shutdown' \
  | (cd "$WORK" && "$BRIDGE" "$CONFIG" "$CREDS")
echo

echo "--- 2. Authenticated alert_summary request (read-only) ---"
printf '%s\n' '{"version":1,"operation":"alert_summary","credential":"demo-full"}' 'shutdown' \
  | (cd "$WORK" && "$BRIDGE" "$CONFIG" "$CREDS")
echo

echo "--- 3. Rejected request: invalid credential (fail closed) ---"
printf '%s\n' '{"version":1,"operation":"status","credential":"not-a-real-token"}' 'shutdown' \
  | (cd "$WORK" && "$BRIDGE" "$CONFIG" "$CREDS")
echo

echo "--- 4. Rejected request: authenticated but unauthorized operation ---"
printf '%s\n' '{"version":1,"operation":"alert_summary","credential":"demo-status"}' 'shutdown' \
  | (cd "$WORK" && "$BRIDGE" "$CONFIG" "$CREDS")
echo

echo "--- 5. Graceful unavailable-service response ---"
echo "(credentials file missing -> verifier fails closed -> every request unauthorized;"
echo " a degraded/unavailable app would return error=unavailable)"
printf '%s\n' '{"version":1,"operation":"status","credential":"demo-full"}' 'shutdown' \
  | (cd "$WORK" && "$BRIDGE" "$CONFIG" "$WORK/does-not-exist.json")
echo

echo "=== Demo complete. All requests were read-only; no sensor, network, or cloud was touched. ==="
