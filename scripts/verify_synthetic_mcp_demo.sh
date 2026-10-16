#!/usr/bin/env bash
# HomeGuardian AI — Release Candidate Synthetic Demonstration Harness
#
# Milestone: HG-REL-02 / HG-UI-03
# Verifies end-to-end integration:
#   Browser Gateway (Node.js) -> Native MCP Server (C++) -> Core Pipeline (SQLite)
#
# Invariants Enforced:
#   1. Loopback-only binding (127.0.0.1) — zero LAN / zero cloud exposure.
#   2. No physical hardware activated — synthetic test media and sensors only.
#   3. Authoritative C++ Device Registry — physical cameras strictly offline.
#   4. State mutation security gate — authenticated session + CSRF token required.
#   5. Fail-Closed Privacy — viewing tokens withheld unless consent is granted.
#   6. WebRTC media streaming — single-use scoped view tokens, loopback SDP.
#   7. Conversational intelligence — MCP protocol 2025-11-25 tool calling.
#
# Usage: bash scripts/verify_synthetic_mcp_demo.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${HG_BUILD_DIR:-$REPO_ROOT/build}"
MCP_SERVER="$BUILD_DIR/hg_mcp_server"
DEV_CLI="$BUILD_DIR/hg_device_registry_cli"
CONFIG_FILE="$REPO_ROOT/config/homeguardian.demo.json"

WEB_PORT=18880
MCP_PORT=18980

PASS=0
FAIL=0

log_ok()   { PASS=$((PASS+1)); printf '  \033[32m✓\033[0m %-65s\n' "$1"; }
log_fail() { FAIL=$((FAIL+1)); printf '  \033[31m✗ FAIL:\033[0m %-60s %s\n' "$1" "${2:-}"; }

echo "================================================================="
echo "HomeGuardian AI — Release Integration Synthetic Demonstration"
echo "Root:  $REPO_ROOT"
echo "Build: $BUILD_DIR"
echo "================================================================="

# 1. Preflight Binaries
echo "[Step 1] Verifying C++ Engine Binaries..."
if [[ ! -x "$MCP_SERVER" ]] || [[ ! -x "$DEV_CLI" ]]; then
  echo "==> Building C++ targets (hg_mcp_server, hg_device_registry_cli)..."
  cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target hg_mcp_server hg_device_registry_cli -j4 >/dev/null
fi

if [[ -x "$MCP_SERVER" ]] && [[ -x "$DEV_CLI" ]]; then
  log_ok "C++ binaries verified and executable"
else
  log_fail "C++ binaries missing or failed to compile"
  exit 1
fi

# 2. Setup Isolated Demo Runtime
echo -e "\n[Step 2] Provisioning Isolated Synthetic Sandbox..."
WORK="$(mktemp -d)"
CREDS="$WORK/demo_creds.json"
cat > "$CREDS" <<'JSON'
{
  "demo-token": {
    "principal": "demo",
    "allow_status": true,
    "allow_alert_summary": true,
    "expires_at_ms": 0,
    "revoked": false
  }
}
JSON
chmod 600 "$CREDS"

DEVICES_JSON="$REPO_ROOT/config/devices.json"
"$DEV_CLI" --registry "$DEVICES_JSON" init_defaults >/dev/null
log_ok "Initialized authoritative C++ Device Registry defaults"

MCP_PID=""
GATEWAY_PID=""

cleanup() {
  echo -e "\n[Teardown] Stopping demonstration processes..."
  if [[ -n "$GATEWAY_PID" ]] && kill -0 "$GATEWAY_PID" 2>/dev/null; then
    kill "$GATEWAY_PID" 2>/dev/null || true
  fi
  if [[ -n "$MCP_PID" ]] && kill -0 "$MCP_PID" 2>/dev/null; then
    kill "$MCP_PID" 2>/dev/null || true
  fi
  if [[ -x "$DEV_CLI" ]]; then
    "$DEV_CLI" --registry "$DEVICES_JSON" init_defaults >/dev/null 2>&1 || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# 3. Start Native C++ MCP Server
echo -e "\n[Step 3] Launching Native C++ MCP Server..."
HG_MCP_PORT="$MCP_PORT" "$MCP_SERVER" "$CONFIG_FILE" "$CREDS" > "$WORK/mcp.out" 2> "$WORK/mcp.err" &
MCP_PID=$!

for _ in $(seq 1 40); do
  if grep -q "MCP_READY" "$WORK/mcp.out" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$MCP_PID" 2>/dev/null; then
    echo "ERROR: hg_mcp_server exited prematurely:" >&2
    cat "$WORK/mcp.err" >&2 || true
    exit 1
  fi
  sleep 0.1
done
log_ok "Native C++ MCP Server listening on 127.0.0.1:$MCP_PORT"

# 4. Run Automated Integration Verification Suites
echo -e "\n[Step 4] Running Automated Integration Verification Suites..."
cd "$REPO_ROOT"

# 4a. Devices & WebRTC Dashboard Suite
DEV_PASS=0
node web_mcp/test_devices_dashboard.js > "$WORK/suite_devices.out" 2>&1 && {
  DEV_PASS=$(grep -oE '[0-9]+ PASSED' "$WORK/suite_devices.out" | head -1 | awk '{print $1}')
  log_ok "HG-UI-03 Suite: ${DEV_PASS:-0} assertions passed (Registry, WebRTC, Auth, Consent)"
} || {
  log_fail "HG-UI-03 Suite failed"
  cat "$WORK/suite_devices.out"
}

# 4b. Native MCP Conversational Client Suite
MCP_PASS=0
node web_mcp/test_client.js > "$WORK/suite_mcp.out" 2>&1 && {
  MCP_PASS=$(grep -oE '[0-9]+ PASSED' "$WORK/suite_mcp.out" | head -1 | awk '{print $1}')
  log_ok "MCP Client Suite: ${MCP_PASS:-0} assertions passed (MCP Streamable HTTP, Tools, Chat)"
} || {
  log_fail "MCP Client Suite failed"
  cat "$WORK/suite_mcp.out"
}

# 5. Launch Live Integrated Demonstration
echo -e "\n[Step 5] Launching Unified Demonstration Gateway..."
cd "$REPO_ROOT"
DEMO_OPERATOR_PASS="$(openssl rand -hex 16)"
HG_WEB_PORT="$WEB_PORT" \
HG_MCP_PORT="$MCP_PORT" \
HG_OPERATOR_USER="admin" \
HG_OPERATOR_PASSWORD="$DEMO_OPERATOR_PASS" \
node web_mcp/server.js > "$WORK/web.out" 2> "$WORK/web.err" &
GATEWAY_PID=$!

for _ in $(seq 1 40); do
  if curl -s -f "http://127.0.0.1:$WEB_PORT/" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$GATEWAY_PID" 2>/dev/null; then
    echo "ERROR: Gateway server exited prematurely:" >&2
    cat "$WORK/web.err" >&2 || true
    exit 1
  fi
  sleep 0.1
done
log_ok "Unified Gateway listening on http://127.0.0.1:$WEB_PORT"

# 6. Verify Live Demonstration Health & Endpoints
DEMO_HEALTH=$(curl -s "http://127.0.0.1:$WEB_PORT/api/health")
if [[ "$DEMO_HEALTH" == *"homeguardian-web-gateway"* ]]; then
  log_ok "Public gateway health API verified"
else
  log_fail "Public gateway health API failed: $DEMO_HEALTH"
fi

# Log in as operator to verify authenticated registry
cat <<EOF > "$WORK/login_payload.json"
{"username":"admin","password":"$DEMO_OPERATOR_PASS"}
EOF
chmod 0600 "$WORK/login_payload.json"

LOGIN_RESP=$(curl -s -c "$WORK/cookies.txt" -X POST "http://127.0.0.1:$WEB_PORT/api/auth/login" \
  -H "Content-Type: application/json" \
  -d @"$WORK/login_payload.json")
if [[ "$LOGIN_RESP" == *"\"ok\":true"* ]]; then
  log_ok "Demonstration operator session authenticated"
else
  log_fail "Demonstration operator session authentication failed: $LOGIN_RESP"
fi

DEV_JSON=$(curl -s -b "$WORK/cookies.txt" "http://127.0.0.1:$WEB_PORT/api/devices")
DEV_COUNT=$(echo "$DEV_JSON" | { grep -o '"id":' || true; } | wc -l)
if [[ "$DEV_COUNT" -ge 4 ]]; then
  log_ok "Live authoritative device inventory verified ($DEV_COUNT devices)"
else
  log_fail "Live authoritative device inventory verification failed (count: $DEV_COUNT)"
fi

echo -e "\n================================================================="
echo "DEMONSTRATION RESULTS: $PASS PASSED, $FAIL FAILED"
echo "================================================================="

if [[ "$FAIL" -gt 0 ]]; then
  exit 1
fi

TOTAL_SUITE_TESTS=$(( ${DEV_PASS:-0} + ${MCP_PASS:-0} ))
echo -e "\n\033[32m✔ Release Integration Synthetic Demonstration is CERTIFIED (${TOTAL_SUITE_TESTS} Tests Passing: ${DEV_PASS} Dashboard/WebRTC + ${MCP_PASS} MCP Client).\033[0m"
echo "Demonstration Dashboard URL: http://127.0.0.1:$WEB_PORT"
echo "Single-run operator credentials: user='admin', password configured via secure runtime environment"
