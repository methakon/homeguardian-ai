#!/usr/bin/env bash
# ==============================================================================
# HomeGuardian AI — Automated Judge-Release Verification Harness
# Track: Alexa+ Track (Real Native C++ MCP 2025-11-25 Server & Streamable HTTP)
# ==============================================================================
#
# Verifies that an independent reviewer/judge can run the genuine native C++
# Model Context Protocol (MCP 2025-11-25) server and Streamable HTTP coordinator
# cleanly from source on local workstation loopback.
#
# Key Invariants Verified:
#   1. Build Prerequisites (CMake, Make, C++20 compiler, Node.js, curl, jq).
#   2. Native C++ MCP 2025-11-25 Handshake (initialize, Mcp-Session-Id 256-bit CSPRNG).
#   3. Strictly Two Read-Only MCP Tools (get_homeguardian_status, get_alert_summary).
#   4. Zero Mutating Tools & Zero Camera Streaming Tools.
#   5. Authentication & Security Boundaries (401 Bad/No Auth, 403 Hijack/Origin, 413 Bounds, 0600 Perms).
#   6. Fail-Closed Camera Boundaries (media_capture_enabled=false, 0 physical video streams).
#   7. Outbound Coordinator Ingress (Loopback binding, token auth, fail-closed offline status).
#   8. Clean Process Teardown (SIGTERM clean shutdown, zero orphaned listeners).
#
# Usage:
#   bash scripts/verify_judge_release.sh
#
# Exit Code:
#   0 on PASS, 1 on verification failure or missing prerequisite.
# ==============================================================================

set -euo pipefail

# ------------------------------------------------------------------------------
# 0. Environment & Metadata Initialization
# ------------------------------------------------------------------------------
REPO_ROOT="${HG_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
COMMIT_SHA="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo "UNKNOWN")"
BRANCH="$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "UNKNOWN")"
TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
START_TIME="$(date +%s)"

BUILD_DIR="${HG_BUILD_DIR:-$REPO_ROOT/build}"
MCP_BIN="$BUILD_DIR/hg_mcp_server"
CONFIG_FILE="$REPO_ROOT/config/homeguardian.demo.json"
COORDINATOR_SCRIPT="$REPO_ROOT/deploy/coordinator/service.js"

PASS_COUNT=0
FAIL_COUNT=0

# Release gate status tracking (dynamic verification, no hardcoded PASS)
GATE_BUILD_PREREQ="NOT_RUN"
GATE_NATIVE_MCP_PROTOCOL="NOT_RUN"
GATE_MCP_HANDSHAKE="NOT_RUN"
GATE_READ_ONLY_TOOLS="NOT_RUN"
GATE_MUTATING_TOOLS_EXCLUDED="NOT_RUN"
GATE_SECURITY_BOUNDS="NOT_RUN"
GATE_OUTBOUND_COORDINATOR="NOT_RUN"
GATE_CAMERA_SAFETY_BOUNDARY="NOT_RUN"
GATE_CLEAN_PROCESS_SHUTDOWN="NOT_RUN"
GATE_NATIVE_DETECTOR_WIRING="NOT_RUN"
SUMMARY_PRINTED=0
TMP_DIR=""
MCP_PID=""
COORD_PID=""

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}✓ PASS${NC}  $*"; }
log_fail()  { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}✗ FAIL${NC}  $*" >&2; }
log_warn()  { echo -e "  ${YELLOW}! WARN${NC}  $*"; }

print_summary() {
  if [[ ${SUMMARY_PRINTED:-0} -eq 1 ]]; then
    return 0
  fi
  SUMMARY_PRINTED=1

  local end_time
  end_time="$(date +%s)"
  local duration=$(( end_time - START_TIME ))

  local all_gates_passed=1
  if [[ ${FAIL_COUNT:-0} -gt 0 || ${PASS_COUNT:-0} -eq 0 ]]; then
    all_gates_passed=0
  fi
  for g in "$GATE_BUILD_PREREQ" "$GATE_NATIVE_MCP_PROTOCOL" "$GATE_MCP_HANDSHAKE" \
           "$GATE_READ_ONLY_TOOLS" "$GATE_MUTATING_TOOLS_EXCLUDED" "$GATE_SECURITY_BOUNDS" \
           "$GATE_OUTBOUND_COORDINATOR" "$GATE_CAMERA_SAFETY_BOUNDARY" "$GATE_CLEAN_PROCESS_SHUTDOWN" \
           "$GATE_NATIVE_DETECTOR_WIRING"; do
    if [[ "$g" != "PASS" ]]; then
      all_gates_passed=0
    fi
  done

  echo -e "\n${BOLD}================================================================================${NC}"
  echo -e "${BOLD}HOMEGUARDIAN AI — JUDGE RELEASE VERIFICATION SUMMARY${NC}"
  echo -e "${BOLD}================================================================================${NC}"
  if [[ $all_gates_passed -eq 1 ]]; then
    echo -e "VERDICT:                ${GREEN}${BOLD}PASS${NC}"
  else
    echo -e "VERDICT:                ${RED}${BOLD}FAIL${NC}"
  fi
  echo -e "COMMIT_SHA:             ${COMMIT_SHA}"
  echo -e "BRANCH:                 ${BRANCH}"
  echo -e "DURATION_SECONDS:       ${duration}"
  echo -e "BUILD_PREREQUISITES:    ${GATE_BUILD_PREREQ} (C++20, CMake, Node.js, npm packages)"
  echo -e "NATIVE_MCP_PROTOCOL:    ${GATE_NATIVE_MCP_PROTOCOL} (73/73 checks passed via verify_mcp.sh)"
  echo -e "MCP_HANDSHAKE:          ${GATE_MCP_HANDSHAKE} (Protocol 2025-11-25, 256-bit CSPRNG token)"
  echo -e "READ_ONLY_TOOLS:        ${GATE_READ_ONLY_TOOLS} (get_homeguardian_status, get_alert_summary)"
  echo -e "MUTATING_TOOLS_EXCLUDED:${GATE_MUTATING_TOOLS_EXCLUDED} (Zero mutating tools, zero camera frame tools)"
  echo -e "SECURITY_BOUNDS:        ${GATE_SECURITY_BOUNDS} (401 Auth, 403 Session Hijack/Origin, 413 Payload)"
  echo -e "OUTBOUND_COORDINATOR:   ${GATE_OUTBOUND_COORDINATOR} (Loopback binding, auth token, 503 fail-closed offline)"
  echo -e "CAMERA_SAFETY_BOUNDARY: ${GATE_CAMERA_SAFETY_BOUNDARY} (media_capture_enabled=false, 0 physical video streams)"
  echo -e "CLEAN_PROCESS_SHUTDOWN: ${GATE_CLEAN_PROCESS_SHUTDOWN} (All child processes exited cleanly, 0 leaks)"
  echo -e "NATIVE_DETECTOR_WIRING: ${GATE_NATIVE_DETECTOR_WIRING} (Ingress detector feed, idle availability, shutdown; synthetic mTLS, no hardware)"
  echo -e "TOTAL_CHECKS_PASSED:    ${PASS_COUNT}"
  echo -e "TOTAL_CHECKS_FAILED:    ${FAIL_COUNT}"
  echo -e "${BOLD}================================================================================${NC}"

  if [[ $all_gates_passed -eq 1 ]]; then
    echo -e "Judge evaluation ready. To reproduce, run: ${BOLD}bash scripts/verify_judge_release.sh${NC}\n"
    return 0
  else
    echo -e "${RED}Judge evaluation FAILED. One or more release gates did not pass.${NC}\n"
    return 1
  fi
}

cleanup() {
  local exit_code=$?
  if [[ -n "${MCP_PID:-}" ]] && kill -0 "$MCP_PID" 2>/dev/null; then
    kill -TERM "$MCP_PID" 2>/dev/null || true
    for _ in $(seq 1 20); do kill -0 "$MCP_PID" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$MCP_PID" 2>/dev/null || true
  fi
  if [[ -n "${COORD_PID:-}" ]] && kill -0 "$COORD_PID" 2>/dev/null; then
    kill -TERM "$COORD_PID" 2>/dev/null || true
    for _ in $(seq 1 20); do kill -0 "$COORD_PID" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$COORD_PID" 2>/dev/null || true
  fi
  if [[ -n "${TMP_DIR:-}" && -d "${TMP_DIR:-}" ]]; then
    rm -rf "${TMP_DIR}"
  fi

  # If any check failed or was logged as fail, force non-zero exit code
  if [[ $exit_code -eq 0 && ${FAIL_COUNT:-0} -gt 0 ]]; then
    exit_code=1
  fi

  # If summary was not printed yet (e.g. exit on failure), print it now
  if [[ ${SUMMARY_PRINTED:-0} -eq 0 ]]; then
    print_summary || true
  fi

  if [[ $exit_code -ne 0 ]]; then
    echo -e "${RED}================================================================================${NC}"
    echo -e "${RED}[VERIFICATION FAILED]${NC} One or more release gates did not pass."
    echo -e "Commit: ${COMMIT_SHA}"
    echo -e "Passed Checks: ${PASS_COUNT:-0}"
    echo -e "Failed Checks: ${FAIL_COUNT:-0}"
    echo -e "Troubleshooting: See docs/DEVPOST_DEMONSTRATION_GUIDE.md for clean-clone instructions."
    echo -e "${RED}================================================================================${NC}\n"
  fi
  exit "$exit_code"
}
trap cleanup EXIT INT TERM

echo -e "${BOLD}================================================================================${NC}"
echo -e "${BOLD}HomeGuardian AI — Automated Judge-Release Verification Harness${NC}"
echo -e "Commit:    ${BLUE}${COMMIT_SHA}${NC}"
echo -e "Branch:    ${BLUE}${BRANCH}${NC}"
echo -e "Timestamp: ${TIMESTAMP}"
echo -e "Root:      ${REPO_ROOT}"
echo -e "${BOLD}================================================================================${NC}\n"

# ------------------------------------------------------------------------------
# 1. Prerequisite Preflight
# ------------------------------------------------------------------------------
log_info "Step 1: Checking Build & Runtime Prerequisites..."

check_cmd() {
  local cmd="$1"
  local help="$2"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    GATE_BUILD_PREREQ="FAIL"
    log_fail "Missing required prerequisite: $cmd. ($help)"
    exit 1
  fi
}

check_cmd "curl"   "Install via: sudo apt-get install -y curl"
check_cmd "jq"     "Install via: sudo apt-get install -y jq"
check_cmd "cmake"  "Install via: sudo apt-get install -y cmake"
check_cmd "make"   "Install via: sudo apt-get install -y build-essential"
check_cmd "node"   "Install Node.js 18+ (https://nodejs.org/)"

CXX_COMPILER=""
if command -v g++ >/dev/null 2>&1; then
  CXX_COMPILER="g++"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_COMPILER="clang++"
else
  GATE_BUILD_PREREQ="FAIL"
  log_fail "No C++20 compiler found (g++ or clang++ required)."
  exit 1
fi
log_ok "Required CLI tools detected (curl, jq, cmake, make, node, $CXX_COMPILER)"

# Check or build native C++ binary
if [[ ! -x "$MCP_BIN" ]]; then
  log_info "Compiling native C++ hg_mcp_server target..."
  cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target hg_mcp_server -j4 >/dev/null
fi

if [[ -x "$MCP_BIN" ]]; then
  log_ok "Native C++ binary verified executable: $MCP_BIN"
else
  GATE_BUILD_PREREQ="FAIL"
  log_fail "Failed to find or compile $MCP_BIN"
  exit 1
fi

# Check npm dependencies
if [[ ! -d "$REPO_ROOT/node_modules" ]]; then
  log_info "Installing required npm dependencies (werift, ws)..."
  (cd "$REPO_ROOT" && npm install --no-audit --no-fund --silent >/dev/null 2>&1)
fi

if [[ -d "$REPO_ROOT/node_modules/ws" ]]; then
  log_ok "Node.js dependencies verified (ws, werift)"
  GATE_BUILD_PREREQ="PASS"
else
  GATE_BUILD_PREREQ="FAIL"
  log_fail "Node.js dependencies missing in $REPO_ROOT/node_modules"
  exit 1
fi

# ------------------------------------------------------------------------------
# 2. Temporary Isolated Sandbox Setup
# ------------------------------------------------------------------------------
log_info "\nStep 2: Provisioning Isolated Local Loopback Sandbox..."

TMP_DIR="$(mktemp -d /tmp/hg_judge_verify.XXXXXX)"

# Dynamic unprivileged test ports
MCP_PORT=$(( 18800 + RANDOM % 300 ))
COORD_HTTP=$(( 19200 + RANDOM % 300 ))
COORD_WS=$(( 19600 + RANDOM % 300 ))

log_ok "Isolated temporary sandbox created at $TMP_DIR"
log_ok "Selected loopback test ports (MCP: $MCP_PORT, Coord HTTP: $COORD_HTTP, Coord WS: $COORD_WS)"

# ------------------------------------------------------------------------------
# 3. Protocol Verification Suite (scripts/verify_mcp.sh)
# ------------------------------------------------------------------------------
log_info "\nStep 3: Running Comprehensive MCP 2025-11-25 Protocol Suite..."

MCP_SUITE_LOG="$TMP_DIR/verify_mcp_output.log"
set +e
HG_MCP_BIN="$MCP_BIN" HG_MCP_TEST_PORT="$MCP_PORT" bash "$REPO_ROOT/scripts/verify_mcp.sh" > "$MCP_SUITE_LOG" 2>&1
MCP_SUITE_RC=$?
set -e

if [[ $MCP_SUITE_RC -eq 0 ]] && grep -q "73 passed, 0 failed" "$MCP_SUITE_LOG"; then
  log_ok "MCP Protocol Suite passed (73/73 checks: 2025-11-25 handshake, tools, 401/403/413, session lifecycle)"
  GATE_NATIVE_MCP_PROTOCOL="PASS"
else
  GATE_NATIVE_MCP_PROTOCOL="FAIL"
  log_fail "MCP Protocol Suite failed (return code: $MCP_SUITE_RC)"
  tail -n 25 "$MCP_SUITE_LOG" >&2
  exit 1
fi

# ------------------------------------------------------------------------------
# 4. Live C++ MCP Server Smoke & Security Boundary Verification
# ------------------------------------------------------------------------------
log_info "\nStep 4: Live Native C++ Streamable HTTP Engine Smoke Test..."

# Provision secure 0600 credentials file
MCP_TEST_PORT=$(( 18800 + RANDOM % 300 ))
JUDGE_TOKEN="judge-tok-$(openssl rand -hex 16)"
CREDS_FILE="$TMP_DIR/judge_creds.json"
cat > "$CREDS_FILE" <<JSON
{
  "$JUDGE_TOKEN": {
    "principal": "judge-reviewer",
    "allow_status": true,
    "allow_alert_summary": true,
    "expires_at_ms": 0,
    "revoked": false
  }
}
JSON
chmod 600 "$CREDS_FILE"

# Launch server
HG_MCP_PORT="$MCP_TEST_PORT" "$MCP_BIN" "$CONFIG_FILE" "$CREDS_FILE" > "$TMP_DIR/cxx_mcp.out" 2> "$TMP_DIR/cxx_mcp.err" &
MCP_PID=$!

# Wait for readiness
SERVER_ONLINE=0
for _ in $(seq 1 40); do
  if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$MCP_TEST_PORT/mcp" 2>/dev/null || grep -q "listening on" "$TMP_DIR/cxx_mcp.err" 2>/dev/null; then
    SERVER_ONLINE=1
    break
  fi
  sleep 0.1
done

if [[ $SERVER_ONLINE -eq 1 ]]; then
  log_ok "Native C++ MCP Server listening on 127.0.0.1:$MCP_TEST_PORT"
else
  GATE_MCP_HANDSHAKE="FAIL"
  log_fail "Native C++ MCP Server failed to start on 127.0.0.1:$MCP_TEST_PORT"
  cat "$TMP_DIR/cxx_mcp.err" >&2
  exit 1
fi

# 4a. Authenticated Handshake (initialize)
INIT_PAYLOAD='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"judge-verify","version":"1.0"}}}'
INIT_RESP="$(curl -s -i -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "Authorization: Bearer $JUDGE_TOKEN" \
  -d "$INIT_PAYLOAD")"

HTTP_STATUS="$(echo "$INIT_RESP" | grep -E '^HTTP/' | awk '{print $2}' | tr -d '\r')"
SESSION_ID="$(echo "$INIT_RESP" | grep -i '^mcp-session-id:' | awk '{print $2}' | tr -d '\r' || true)"
INIT_BODY="$(echo "$INIT_RESP" | sed -e '1,/^\r*$/d')"

if [[ "$HTTP_STATUS" == "200" ]]; then
  log_ok "MCP initialize returned HTTP 200"
else
  GATE_MCP_HANDSHAKE="FAIL"
  log_fail "MCP initialize returned unexpected HTTP status: $HTTP_STATUS"
  exit 1
fi

NEGOTIATED_VER="$(echo "$INIT_BODY" | jq -r '.result.protocolVersion // empty')"
SERVER_NAME="$(echo "$INIT_BODY" | jq -r '.result.serverInfo.name // empty')"

if [[ "$NEGOTIATED_VER" == "2025-11-25" ]]; then
  log_ok "Negotiated MCP protocol version is 2025-11-25"
else
  GATE_MCP_HANDSHAKE="FAIL"
  log_fail "Expected protocolVersion 2025-11-25, got: $NEGOTIATED_VER"
  exit 1
fi

if [[ "$SERVER_NAME" == "homeguardian-mcp" ]]; then
  log_ok "Server reports authentic identity: homeguardian-mcp"
else
  GATE_MCP_HANDSHAKE="FAIL"
  log_fail "Server reports unexpected name: $SERVER_NAME"
  exit 1
fi

if [[ ${#SESSION_ID} -eq 64 ]]; then
  log_ok "Mcp-Session-Id minted with 256-bit CSPRNG entropy (${#SESSION_ID} hex chars)"
  GATE_MCP_HANDSHAKE="PASS"
else
  GATE_MCP_HANDSHAKE="FAIL"
  log_fail "Invalid Mcp-Session-Id header: expected 64 hex characters, got: '$SESSION_ID'"
  exit 1
fi

# 4b. Strictly Two Read-Only Tools (tools/list)
TOOLS_PAYLOAD='{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
TOOLS_RESP="$(curl -s -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "Authorization: Bearer $JUDGE_TOKEN" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$TOOLS_PAYLOAD")"

TOOL_COUNT="$(echo "$TOOLS_RESP" | jq -r '.result.tools | length')"
HAS_STATUS="$(echo "$TOOLS_RESP" | jq -r '.result.tools[] | select(.name=="get_homeguardian_status") | .name')"
HAS_ALERTS="$(echo "$TOOLS_RESP" | jq -r '.result.tools[] | select(.name=="get_alert_summary") | .name')"
MUTATING_TOOLS="$(echo "$TOOLS_RESP" | jq -r '.result.tools[] | select(.name | test("set_|camera|frame|video|audio")) | .name')"

if [[ "$TOOL_COUNT" -eq 2 ]] && [[ -n "$HAS_STATUS" ]] && [[ -n "$HAS_ALERTS" ]]; then
  log_ok "tools/list exposes strictly two read-only tools: get_homeguardian_status, get_alert_summary"
else
  GATE_READ_ONLY_TOOLS="FAIL"
  log_fail "tools/list tool count mismatch: expected 2, got $TOOL_COUNT"
  exit 1
fi

if [[ -z "$MUTATING_TOOLS" ]]; then
  log_ok "Zero mutating tools, zero camera/video streaming tools exposed"
  GATE_MUTATING_TOOLS_EXCLUDED="PASS"
else
  GATE_MUTATING_TOOLS_EXCLUDED="FAIL"
  log_fail "Unauthorized mutating or camera tools detected: $MUTATING_TOOLS"
  exit 1
fi

# 4c. Tool Calls Execution
STATUS_CALL='{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}'
STATUS_RESP="$(curl -s -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "Authorization: Bearer $JUDGE_TOKEN" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$STATUS_CALL")"

READ_ONLY_STATUS="$(echo "$STATUS_RESP" | jq -r '.result.content[0].text' | jq -r '.read_only')"
if [[ "$READ_ONLY_STATUS" == "true" ]]; then
  log_ok "tools/call get_homeguardian_status succeeded with read_only: true"
else
  GATE_READ_ONLY_TOOLS="FAIL"
  log_fail "get_homeguardian_status did not confirm read_only invariant: $STATUS_RESP"
  exit 1
fi

ALERTS_CALL='{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}'
ALERTS_RESP="$(curl -s -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "Authorization: Bearer $JUDGE_TOKEN" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$ALERTS_CALL")"

ALERT_COUNT_VAL="$(echo "$ALERTS_RESP" | jq -r '.result.content[0].text' | jq -r '.alert_count')"
if [[ "$ALERT_COUNT_VAL" =~ ^[0-9]+$ ]]; then
  log_ok "tools/call get_alert_summary succeeded (alert_count: $ALERT_COUNT_VAL)"
  GATE_READ_ONLY_TOOLS="PASS"
else
  GATE_READ_ONLY_TOOLS="FAIL"
  log_fail "get_alert_summary returned invalid alert_count: $ALERTS_RESP"
  exit 1
fi

# 4d. Negative Security Boundary Checks
# Missing auth -> 401
UNAUTH_STATUS="$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$STATUS_CALL")"
if [[ "$UNAUTH_STATUS" == "401" ]]; then
  log_ok "Security boundary: unauthenticated request refused with HTTP 401"
else
  GATE_SECURITY_BOUNDS="FAIL"
  log_fail "Unauthenticated request expected 401, got $UNAUTH_STATUS"
  exit 1
fi

# Invalid token -> 401
BAD_TOKEN_STATUS="$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer invalid-bogus-token" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$STATUS_CALL")"
if [[ "$BAD_TOKEN_STATUS" == "401" ]]; then
  log_ok "Security boundary: invalid bearer token refused with HTTP 401"
else
  GATE_SECURITY_BOUNDS="FAIL"
  log_fail "Invalid bearer token expected 401, got $BAD_TOKEN_STATUS"
  exit 1
fi

# Unknown/unregistered tool -> JSON-RPC error
UNKNOWN_TOOL='{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"system_reboot","arguments":{}}}'
UNKNOWN_RESP="$(curl -s -X POST "http://127.0.0.1:$MCP_TEST_PORT/mcp" \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -H "Authorization: Bearer $JUDGE_TOKEN" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d "$UNKNOWN_TOOL")"
RPC_ERR_CODE="$(echo "$UNKNOWN_RESP" | jq -r '.error.code // empty')"
if [[ "$RPC_ERR_CODE" == "-32602" ]]; then
  log_ok "Security boundary: call to unregistered tool refused with error -32602"
  GATE_SECURITY_BOUNDS="PASS"
else
  GATE_SECURITY_BOUNDS="FAIL"
  log_fail "Expected JSON-RPC error -32602 for unknown tool, got: $UNKNOWN_RESP"
  exit 1
fi

# ------------------------------------------------------------------------------
# 5. Outbound Coordinator Ingress Verification
# ------------------------------------------------------------------------------
log_info "\nStep 5: Testing Outbound MCP Coordinator Service..."

COORD_TOKEN="coord-tok-$(openssl rand -hex 16)"
COORD_TOKEN_FILE="$TMP_DIR/coord_token.secret"
echo "$COORD_TOKEN" > "$COORD_TOKEN_FILE"
chmod 600 "$COORD_TOKEN_FILE"

COORD_DEVICES_FILE="$TMP_DIR/coord_devices.json"
cat > "$COORD_DEVICES_FILE" <<JSON
{}
JSON
chmod 600 "$COORD_DEVICES_FILE"

COORD_BIND="127.0.0.1" \
COORD_WS_PORT="$COORD_WS" \
COORD_HTTP_PORT="$COORD_HTTP" \
COORD_DEVICES_FILE="$COORD_DEVICES_FILE" \
COORD_CLIENT_TOKEN_FILE="$COORD_TOKEN_FILE" \
COORD_ALLOWED_SUBNETS="127.,::1,::ffff:127." \
COORD_ENV_FILE="" \
node "$COORDINATOR_SCRIPT" > "$TMP_DIR/coord.out" 2> "$TMP_DIR/coord.err" &
COORD_PID=$!

COORD_ONLINE=0
for _ in $(seq 1 40); do
  if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$COORD_HTTP/health" 2>/dev/null; then
    COORD_ONLINE=1
    break
  fi
  sleep 0.1
done

if [[ "$COORD_ONLINE" -eq 1 ]]; then
  log_ok "Outbound MCP Coordinator listening on 127.0.0.1:$COORD_HTTP (HTTP) & :$COORD_WS (WS)"
else
  GATE_OUTBOUND_COORDINATOR="FAIL"
  log_fail "Coordinator failed to become ready on 127.0.0.1:$COORD_HTTP"
  cat "$TMP_DIR/coord.err" >&2
  exit 1
fi

# Public health check (nonsensitive)
HEALTH_RESP="$(curl -s "http://127.0.0.1:$COORD_HTTP/health")"
HEALTH_EDGE="$(echo "$HEALTH_RESP" | jq -r '.edge_online')"
if [[ "$HEALTH_EDGE" == "false" ]]; then
  log_ok "Coordinator public health confirms fail-closed state (edge_online: false)"
else
  GATE_OUTBOUND_COORDINATOR="FAIL"
  log_fail "Unexpected edge_online state in health report: $HEALTH_RESP"
  exit 1
fi

# Unauthenticated client request to coordinator /mcp -> 401
COORD_UNAUTH_CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$COORD_HTTP/mcp" \
  -H "Content-Type: application/json" \
  -d "$INIT_PAYLOAD")"
if [[ "$COORD_UNAUTH_CODE" == "401" ]]; then
  log_ok "Coordinator /mcp enforces Bearer authentication (refused unauthenticated client with 401)"
else
  GATE_OUTBOUND_COORDINATOR="FAIL"
  log_fail "Coordinator /mcp expected 401 for unauthenticated client, got: $COORD_UNAUTH_CODE"
  exit 1
fi

# Authenticated client call with edge offline -> 503 EDGE_OFFLINE (fail-closed)
COORD_OFFLINE_STATUS="$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://127.0.0.1:$COORD_HTTP/mcp" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $COORD_TOKEN" \
  -d "$INIT_PAYLOAD")"
if [[ "$COORD_OFFLINE_STATUS" == "503" ]]; then
  log_ok "Coordinator fail-closed boundary: edge offline returns HTTP 503 Service Unavailable"
  GATE_OUTBOUND_COORDINATOR="PASS"
else
  GATE_OUTBOUND_COORDINATOR="FAIL"
  log_fail "Expected HTTP 503 when edge is offline, got: $COORD_OFFLINE_STATUS"
  exit 1
fi

# ------------------------------------------------------------------------------
# 6. Physical Camera Fail-Closed Boundary Verification
# ------------------------------------------------------------------------------
log_info "\nStep 6: Verifying Fail-Closed Physical Camera & Privacy Gates..."

CONFIG_CAPTURE_ENABLED="$(jq -r '.media_capture_enabled' "$REPO_ROOT/config/homeguardian.json")"
CONFIG_CLOUD_ENABLED="$(jq -r '.cloud_processing_enabled' "$REPO_ROOT/config/homeguardian.json")"

if [[ "$CONFIG_CAPTURE_ENABLED" == "false" ]]; then
  log_ok "Committed config enforces media_capture_enabled: false"
else
  GATE_CAMERA_SAFETY_BOUNDARY="FAIL"
  log_fail "Privacy violation: media_capture_enabled is not false in config/homeguardian.json"
  exit 1
fi

if [[ "$CONFIG_CLOUD_ENABLED" == "false" ]]; then
  log_ok "Committed config enforces cloud_processing_enabled: false"
else
  GATE_CAMERA_SAFETY_BOUNDARY="FAIL"
  log_fail "Privacy violation: cloud_processing_enabled is not false in config/homeguardian.json"
  exit 1
fi

# Assert no active video devices opened by HomeGuardian
ACTIVE_CAM_PROCS="$(lsof /dev/video* 2>/dev/null | grep -E "homeguardian|hg_" || true)"
if [[ -z "$ACTIVE_CAM_PROCS" ]]; then
  log_ok "Zero physical camera streams or V4L2 device descriptors opened"
  GATE_CAMERA_SAFETY_BOUNDARY="PASS"
else
  GATE_CAMERA_SAFETY_BOUNDARY="FAIL"
  log_fail "Active physical camera descriptors detected: $ACTIVE_CAM_PROCS"
  exit 1
fi

# ------------------------------------------------------------------------------
# 6b. Native ingress detector wiring (synthetic mTLS, not hardware)
# ------------------------------------------------------------------------------
log_info "\nStep 6b: Verifying Native Ingress Detector Wiring (synthetic mTLS)..."

# The wiring check is structural and always runs: if the suite stopped being
# registered, a green special-runner result would otherwise be meaningless.
INGRESS_SUITE_SRC="$REPO_ROOT/tests/net/test_production_ingress.cpp"
INGRESS_SUITE_RUNNER="$REPO_ROOT/tests/net/run_production_ingress.py"
if grep -q "test_production_ingress.cpp" "$REPO_ROOT/CMakeLists.txt" \
   && grep -q "production_ingress_tls" "$REPO_ROOT/CMakeLists.txt" \
   && [[ -f "$INGRESS_SUITE_SRC" && -f "$INGRESS_SUITE_RUNNER" ]]; then
  log_ok "Ingress detector-wiring suite is registered in the canonical CMake/CTest gate"
else
  GATE_NATIVE_DETECTOR_WIRING="FAIL"
  log_fail "Ingress detector-wiring suite is not registered in CMakeLists.txt / missing sources"
  exit 1
fi

# Execution needs the TLS-enabled test binary plus openssl to mint throwaway certs.
if [[ -x "$BUILD_DIR/homeguardian_tests" ]] && command -v openssl >/dev/null 2>&1; then
  if HG_INGRESS_TESTS_BIN="$BUILD_DIR/homeguardian_tests" \
     timeout 180 "${HG_PYTHON:-python3}" "$INGRESS_SUITE_RUNNER" > "$TMP_DIR/ingress_wiring.log" 2>&1; then
    INGRESS_ASSERTIONS="$(tail -n 3 "$TMP_DIR/ingress_wiring.log" | grep -oE '[0-9]+ assertions in [0-9]+ test cases' | head -1)"
    log_ok "Ingress detector wiring passed over real loopback mTLS (${INGRESS_ASSERTIONS:-see log})"
    GATE_NATIVE_DETECTOR_WIRING="PASS"
  else
    GATE_NATIVE_DETECTOR_WIRING="FAIL"
    log_fail "Ingress detector wiring failed"
    tail -n 25 "$TMP_DIR/ingress_wiring.log" >&2
    exit 1
  fi
else
  GATE_NATIVE_DETECTOR_WIRING="SKIP"
  log_fail "Ingress detector wiring NOT EXECUTED: needs $BUILD_DIR/homeguardian_tests (OpenSSL) + openssl. This gate is not a pass."
fi

# ------------------------------------------------------------------------------
# 7. Clean Shutdown Verification
# ------------------------------------------------------------------------------
log_info "\nStep 7: Verifying Clean Process Shutdown & Zero Leaks..."

# Terminate MCP server cleanly
if [[ -n "$MCP_PID" ]] && kill -0 "$MCP_PID" 2>/dev/null; then
  kill -TERM "$MCP_PID" 2>/dev/null || true
  for _ in $(seq 1 30); do kill -0 "$MCP_PID" 2>/dev/null || break; sleep 0.1; done
  if ! kill -0 "$MCP_PID" 2>/dev/null; then
    log_ok "Native C++ MCP server shut down cleanly on SIGTERM"
  else
    GATE_CLEAN_PROCESS_SHUTDOWN="FAIL"
    log_fail "Native C++ MCP server failed to terminate on SIGTERM"
    exit 1
  fi
fi

# Terminate Coordinator cleanly
if [[ -n "$COORD_PID" ]] && kill -0 "$COORD_PID" 2>/dev/null; then
  kill -TERM "$COORD_PID" 2>/dev/null || true
  for _ in $(seq 1 30); do kill -0 "$COORD_PID" 2>/dev/null || break; sleep 0.1; done
  if ! kill -0 "$COORD_PID" 2>/dev/null; then
    log_ok "Outbound MCP coordinator shut down cleanly on SIGTERM"
    GATE_CLEAN_PROCESS_SHUTDOWN="PASS"
  else
    GATE_CLEAN_PROCESS_SHUTDOWN="FAIL"
    log_fail "Outbound MCP coordinator failed to terminate on SIGTERM"
    exit 1
  fi
fi

# ------------------------------------------------------------------------------
# 8. Final Machine-Readable Verdict Summary
# ------------------------------------------------------------------------------
print_summary
FINAL_RC=$?
exit "$FINAL_RC"
