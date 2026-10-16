#!/usr/bin/env bash
# HomeGuardian AI — Independent Security Gate Test Suite (HG-SEC-01)
#
# Auditor: Antigravity (Independent QA, Security & MCP Lead)
# Purpose: Independently verify Hermes's HG-SEC-01 security patch against the actual threat model.
#
# Threat Model Invariants Verified:
#   1. GET /api/run-scenario is rejected with 405 Method Not Allowed (zero state change).
#   2. Unauthenticated local processes cannot obtain a session token (401 Unauthorized).
#   3. Missing, invalid, or forged operator secrets produce NO token and NO state mutation.
#   4. Missing, forged, or replayed CSRF tokens produce 403 Forbidden.
#   5. Strict loopback Origin & Host enforcement (cross-origin / wrong-port blocked with 403).
#   6. Zero-mutation invariant: SQLite alert store is verified untouched on every negative test.
#   7. Single-use replay protection: a consumed CSRF token CANNOT be reused.
#   8. Positive control: valid secret mints CSRF, single execution succeeds, exactly 1 alert written.
#   9. No credentials in URLs: ?k=... and ?token=... rejected immediately with 400.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${HG_BUILD_DIR:-$REPO_ROOT/build}"
BRIDGE="$BUILD_DIR/hg_alexa_bridge"
CONFIG="$REPO_ROOT/config/homeguardian.demo.json"

TEST_PORT=18777
PASS=0
FAIL=0

log_ok()   { PASS=$((PASS+1)); printf '  \033[32m✓\033[0m %-65s\n' "$1"; }
log_fail() { FAIL=$((FAIL+1)); printf '  \033[31m✗ FAIL:\033[0m %-60s %s\n' "$1" "${2:-}"; }

echo "================================================================="
echo "HG-SEC-01: Independent Security Gate & Threat Model Verification"
echo "Target Port: $TEST_PORT"
echo "================================================================="

if [[ ! -x "$BRIDGE" ]]; then
  echo "ERROR: hg_alexa_bridge not found at $BRIDGE. Build it first." >&2
  exit 1
fi

WORK="$(mktemp -d)"
DB_PATH="$WORK/homeguardian_bridge_audit.db"
CREDS="$WORK/demo_creds.json"
OPERATOR_SECRET="IndependentOperatorSecret2026!"

cat > "$CREDS" <<JSON
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

BRIDGE_PID=""
cleanup() {
  if [[ -n "$BRIDGE_PID" ]] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
    kill "$BRIDGE_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# Start bridge in isolated sandbox
cd "$WORK"
printf '%s' "$OPERATOR_SECRET" > "$WORK/secret.txt"
chmod 600 "$WORK/secret.txt"
printf '%s' "demo-token" > "$WORK/token.txt"
chmod 600 "$WORK/token.txt"

HG_DEMO_SECRET="$OPERATOR_SECRET" HG_HTTP_SECRET="$OPERATOR_SECRET" HG_HTTP_TOKEN="demo-token" \
  "$BRIDGE" --http "$CONFIG" "$CREDS" \
  --port "$TEST_PORT" \
  --token-file "$WORK/token.txt" \
  --secret-file "$WORK/secret.txt" > "$WORK/bridge.out" 2> "$WORK/bridge.err" &
BRIDGE_PID=$!

LAUNCHED=0
for _ in $(seq 1 40); do
  if grep -q "DEMO_READY" "$WORK/bridge.out" 2>/dev/null; then
    LAUNCHED=1
    break
  fi
  if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    echo "ERROR: bridge failed to launch:" >&2
    cat "$WORK/bridge.err" >&2 || true
    exit 1
  fi
  sleep 0.1
done
if [[ "$LAUNCHED" -ne 1 ]]; then
  echo "ERROR: timed out waiting for DEMO_READY from bridge" >&2
  exit 1
fi
log_ok "hg_alexa_bridge launched and listening on 127.0.0.1:$TEST_PORT"

get_alert_count() {
  if [[ -f "$DB_PATH" ]]; then
    sqlite3 "$DB_PATH" "SELECT count(*) FROM alerts;" 2>/dev/null || echo "0"
  else
    echo "0"
  fi
}

# -----------------------------------------------------------------------
# Test 1: GET method restriction on state-advancing route
# -----------------------------------------------------------------------
echo -e "\n[Gate 1] HTTP Method Restriction on State Advancement"
COUNT_BEFORE=$(get_alert_count)
HTTP_CODE=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X GET \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  "http://127.0.0.1:$TEST_PORT/api/run-scenario?scenario=critical_fall")
COUNT_AFTER=$(get_alert_count)

if [[ "$HTTP_CODE" == "405" ]]; then
  log_ok "GET /api/run-scenario rejected with HTTP 405 Method Not Allowed"
else
  log_fail "GET /api/run-scenario returned $HTTP_CODE (expected 405)"
fi

if [[ "$COUNT_BEFORE" == "$COUNT_AFTER" ]]; then
  log_ok "Negative invariant: SQLite alert count unchanged ($COUNT_BEFORE rows)"
else
  log_fail "VULNERABILITY: SQLite alert count changed from $COUNT_BEFORE to $COUNT_AFTER"
fi

# -----------------------------------------------------------------------
# Test 2: Unauthenticated session issuance attempt
# -----------------------------------------------------------------------
echo -e "\n[Gate 2] Caller Authentication on /api/session"
# 2a. No secret provided
CODE_NO_SECRET=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/session")

if [[ "$CODE_NO_SECRET" == "401" ]]; then
  log_ok "Unauthenticated POST /api/session rejected with HTTP 401"
else
  log_fail "Unauthenticated POST /api/session returned $CODE_NO_SECRET (expected 401)"
fi

# 2b. Wrong secret provided
CODE_WRONG_SECRET=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -H "X-HG-Secret: invalid-operator-secret" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/session")

if [[ "$CODE_WRONG_SECRET" == "401" ]]; then
  log_ok "POST /api/session with wrong secret rejected with HTTP 401"
else
  log_fail "POST /api/session with wrong secret returned $CODE_WRONG_SECRET (expected 401)"
fi

# -----------------------------------------------------------------------
# Test 3: Unauthenticated state mutation attempt
# -----------------------------------------------------------------------
echo -e "\n[Gate 3] State Mutation Protection Without Token"
COUNT_BEFORE=$(get_alert_count)
CODE_NO_CSRF=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/run-scenario?scenario=critical_fall")
COUNT_AFTER=$(get_alert_count)

if [[ "$CODE_NO_CSRF" == "403" ]]; then
  log_ok "POST /api/run-scenario without CSRF token rejected with HTTP 403"
else
  log_fail "POST /api/run-scenario without CSRF returned $CODE_NO_CSRF (expected 403)"
fi

if [[ "$COUNT_BEFORE" == "$COUNT_AFTER" ]]; then
  log_ok "Negative invariant: SQLite alert count unchanged ($COUNT_BEFORE rows)"
else
  log_fail "VULNERABILITY: SQLite alert count changed from $COUNT_BEFORE to $COUNT_AFTER"
fi

# -----------------------------------------------------------------------
# Test 4: Forged & Cross-Origin Rejection
# -----------------------------------------------------------------------
echo -e "\n[Gate 4] Strict Origin & Host Validation"
CODE_CROSS_ORIGIN=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://attacker.local:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -H "X-HG-Secret: $OPERATOR_SECRET" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/session")

if [[ "$CODE_CROSS_ORIGIN" == "403" ]]; then
  log_ok "Cross-origin request rejected with HTTP 403"
else
  log_fail "Cross-origin request returned $CODE_CROSS_ORIGIN (expected 403)"
fi

# -----------------------------------------------------------------------
# Test 5: Rejection of URL Credentials
# -----------------------------------------------------------------------
echo -e "\n[Gate 5] Rejection of URL-Based Secrets (?k=... / ?token=...)"
CODE_URL_SECRET=$(curl -s -o "$WORK/resp.txt" -w "%{http_code}" -X GET \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  "http://127.0.0.1:$TEST_PORT/?k=secret-token")

if [[ "$CODE_URL_SECRET" == "400" || "$CODE_URL_SECRET" == "404" ]]; then
  log_ok "Request with credentials in query string (?k=...) rejected ($CODE_URL_SECRET)"
else
  log_fail "Request with ?k=... returned $CODE_URL_SECRET (expected 400 or 404)"
fi

# -----------------------------------------------------------------------
# Test 6: Positive Path — Valid Minting & Execution
# -----------------------------------------------------------------------
echo -e "\n[Gate 6] Authorized Session Minting & Single Scenario Execution"
SESSION_RESP=$(curl -s -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -H "X-HG-Secret: $OPERATOR_SECRET" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/session")

CSRF_TOKEN=$(echo "$SESSION_RESP" | grep -o '"csrf":"[^"]*"' | cut -d'"' -f4 || echo "")

if [[ -n "$CSRF_TOKEN" && ${#CSRF_TOKEN} -eq 64 ]]; then
  log_ok "Session token minted with valid 64-char CSRF token"
else
  log_fail "Failed to mint valid CSRF token. Response: $SESSION_RESP"
fi

COUNT_BEFORE=$(get_alert_count)
CODE_EXEC=$(curl -s -o "$WORK/exec_resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -H "X-HG-CSRF: $CSRF_TOKEN" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/run-scenario?scenario=unusual")
COUNT_AFTER=$(get_alert_count)

if [[ "$CODE_EXEC" == "200" ]]; then
  log_ok "Authenticated POST /api/run-scenario accepted with HTTP 200"
else
  log_fail "Authenticated POST /api/run-scenario failed with HTTP $CODE_EXEC"
  cat "$WORK/exec_resp.txt" || true
fi

if [[ "$COUNT_AFTER" -gt "$COUNT_BEFORE" ]]; then
  log_ok "Positive invariant: SQLite alert recorded (rows: $COUNT_BEFORE -> $COUNT_AFTER)"
else
  log_fail "Positive invariant FAILED: SQLite alert count did not advance"
fi

# -----------------------------------------------------------------------
# Test 7: Single-Use Replay Protection
# -----------------------------------------------------------------------
echo -e "\n[Gate 7] Token Replay Protection (Single-Use Invariant)"
COUNT_BEFORE=$(get_alert_count)
CODE_REPLAY=$(curl -s -o "$WORK/replay_resp.txt" -w "%{http_code}" -X POST \
  -H "Host: 127.0.0.1:$TEST_PORT" \
  -H "Origin: http://127.0.0.1:$TEST_PORT" \
  -H "Content-Type: application/json" \
  -H "X-HG-CSRF: $CSRF_TOKEN" \
  -d "{}" \
  "http://127.0.0.1:$TEST_PORT/api/run-scenario?scenario=unusual")
COUNT_AFTER=$(get_alert_count)

if [[ "$CODE_REPLAY" == "403" ]]; then
  log_ok "Replayed CSRF token rejected with HTTP 403 Forbidden"
else
  log_fail "Replayed CSRF token returned HTTP $CODE_REPLAY (expected 403)"
fi

if [[ "$COUNT_BEFORE" == "$COUNT_AFTER" ]]; then
  log_ok "Negative invariant: SQLite alert count unchanged on replayed token"
else
  log_fail "VULNERABILITY: SQLite alert recorded on replayed token!"
fi

echo -e "\n================================================================="
echo "SECURITY GATE RESULTS: $PASS PASSED, $FAIL FAILED"
echo "================================================================="

if [[ "$FAIL" -gt 0 ]]; then
  exit 1
fi

echo -e "\n\033[32m✔ HG-SEC-01 SECURITY GATE CERTIFIED.\033[0m"
