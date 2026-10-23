#!/usr/bin/env bash
# HomeGuardian AI — one-command reproducible demo (simulated Alexa+ experience).
#
# Builds the C++ bridge if needed, generates synthetic test credentials, starts
# the local web demo, and prints the URL. Everything is loopback-only; no
# network, cloud, or physical sensor is used.
#
# Usage:  bash demo.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${HG_DEMO_BUILD_DIR:-$REPO_ROOT/hg_build}"
DEMO_PORT="${HG_DEMO_PORT:-0}"   # 0 = pick a free port
BRIDGE="$BUILD_DIR/hg_alexa_bridge"
CONFIG="$REPO_ROOT/config/homeguardian.demo.json"
DEMO_SERVER="$REPO_ROOT/src/bridge/hg_demo_server.js"

echo "==> HomeGuardian AI — simulated Alexa+ web demo"

# --- 1. Build the bridge if missing (reuse an existing build dir; never delete) ---
if [[ ! -x "$BRIDGE" ]]; then
  echo "==> Bridge not found at $BRIDGE; configuring + building (this fetches deps once)…"
  mkdir -p "$BUILD_DIR"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD_DIR" --target hg_alexa_bridge -j4 >/dev/null
fi
if [[ ! -x "$BRIDGE" ]]; then
  echo "ERROR: bridge still not built at $BRIDGE" >&2
  exit 1
fi
echo "==> Using bridge: $BRIDGE"

# --- 2. Synthetic test credentials (isolated temp; never committed) ---
WORK="$(mktemp -d)"
CREDS="$WORK/demo_creds.json"
cat > "$CREDS" <<'JSON'
{
  "demo-token": {"principal":"demo","allow_status":true,"allow_alert_summary":true,"expires_at_ms":0,"revoked":false}
}
JSON
chmod 600 "$CREDS"

# --- 3. Run the demo from the temp dir so its SQLite DB stays out of the repo ---
cd "$WORK"

cleanup() {
  if [[ -n "${DEMO_PID:-}" ]] && kill -0 "$DEMO_PID" 2>/dev/null; then
    kill "$DEMO_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

echo "==> Starting native demo server (loopback 127.0.0.1 only, no Node.js)…"
# Default: ONE native C++ process serves the embedded dashboard + JSON API.
# Set HG_DEMO_LEGACY_NODE=1 to use the historical Node.js demo server instead
# (kept only as a reference for A/B footprint comparison; not required).
if [[ "${HG_DEMO_LEGACY_NODE:-0}" == "1" ]]; then
  node "$DEMO_SERVER" --bridge "$BRIDGE" --config "$CONFIG" --creds "$CREDS" \
    --port "$DEMO_PORT" > "$WORK/demo.out" 2> "$WORK/demo.err" &
else
  "$BRIDGE" --http "$CONFIG" "$CREDS" \
    --port "$DEMO_PORT" > "$WORK/demo.out" 2> "$WORK/demo.err" &
fi
DEMO_PID=$!

# --- 4. Wait for readiness, then print the URL ---
PORT=""
for _ in $(seq 1 50); do
  if grep -q "DEMO_READY" "$WORK/demo.out" 2>/dev/null; then
    PORT="$(grep -oE 'DEMO_READY [0-9]+' "$WORK/demo.out" | grep -oE '[0-9]+')"
    break
  fi
  if ! kill -0 "$DEMO_PID" 2>/dev/null; then
    echo "ERROR: demo server exited unexpectedly:" >&2
    cat "$WORK/demo.err" >&2 || true
    exit 1
  fi
  sleep 0.2
done

if [[ -z "$PORT" ]]; then
  echo "ERROR: demo server did not become ready in time:" >&2
  cat "$WORK/demo.err" >&2 || true
  exit 1
fi

echo ""
echo "  ┌──────────────────────────────────────────────────────────┐"
echo "  │  HomeGuardian AI — Simulated Alexa+ Experience           │"
echo "  │                                                          │"
echo "  │  Open in your browser:  http://127.0.0.1:$PORT            │"
echo "  │                                                          │"
echo "  │  1. Click \"Run synthetic scenario\"                        │"
echo "  │  2. Ask \"Is everything okay?\"                              │"
echo "  │  3. Watch the alert timeline update                       │"
echo "  │                                                          │"
echo "  │  Synthetic data only · loopback only · no sensors         │"
echo "  └──────────────────────────────────────────────────────────┘"
echo ""
echo "Press Ctrl+C to stop the demo."

# Keep running until interrupted.
wait "$DEMO_PID"
