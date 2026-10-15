#!/usr/bin/env bash
# HomeGuardian hub container entrypoint (milestone D-01).
#
# Validates that configuration and credentials are mounted, selects the data
# directory (the persistent volume), then `exec`s the native C++ hub so that the
# process is PID 1 and receives SIGTERM/SIGINT directly for a clean shutdown.
#
# Fail-closed: a missing/unreadable config or credential file aborts startup with
# a clear message instead of running without them.
set -euo pipefail

HG_PORT="${HG_PORT:-8080}"
HG_DATA_DIR="${HG_DATA_DIR:-/data}"
HG_CONFIG="${HG_CONFIG:-/etc/homeguardian/hub.json}"
HG_CREDS="${HG_CREDS:-/run/secrets/hg_creds}"
# The synthetic demo token. This fallback exists ONLY so a bare local `docker run` of the demo still
# works; a real deployment sets HG_TOKEN (docker-compose.yml does). It is NOT baked into the image, so
# an image consumer is not handed a credential-shaped default silently - and using it says so.
if [ -z "${HG_TOKEN:-}" ]; then
    HG_TOKEN="demo-token"
    echo "homeguardian-hub: NOTE: HG_TOKEN is unset - using the synthetic demo token (demo only; set HG_TOKEN for a real deployment)" >&2
fi

die() { echo "homeguardian-hub: $*" >&2; exit 78; }   # 78 = EX_CONFIG

[ -r "$HG_CONFIG" ] || die "configuration not readable: $HG_CONFIG (mount it read-only)"
[ -r "$HG_CREDS" ]  || die "credentials not readable: $HG_CREDS (mount them read-only; never bake them into an image)"
[ -d "$HG_DATA_DIR" ] || die "data directory missing: $HG_DATA_DIR"
[ -w "$HG_DATA_DIR" ] || die "data directory not writable: $HG_DATA_DIR (check volume ownership)"

case "$HG_PORT" in
    ''|*[!0-9]*) die "HG_PORT must be a number, got '$HG_PORT'" ;;
esac

echo "homeguardian-hub: starting (port=${HG_PORT} loopback-only inside the container," \
     "data=${HG_DATA_DIR}, config=${HG_CONFIG})"

# Persistence (homeguardian_bridge_audit.db) and homeguardian.log live in the
# data directory so they survive container replacement.
cd "$HG_DATA_DIR"

# The hub's HTTP server binds 127.0.0.1 only. Nothing here publishes it.
exec /usr/local/bin/hg_alexa_bridge \
    --http "$HG_CONFIG" "$HG_CREDS" \
    --port "$HG_PORT" \
    --token "$HG_TOKEN"
