#!/usr/bin/env bash
# OWNER-RUN LOCAL MCP ENDPOINT.
#
# Starts the HomeGuardian MCP server on loopback against a store you name, waits until it answers, runs a
# few real protocol calls and prints them, then leaves it running for you to poke at - with the exact
# command to stop it.
#
# It is a READER. It opens the store read-only (SQLITE_OPEN_READONLY + PRAGMA query_only), never creates a
# missing one, and reports an unopenable store as UNAVAILABLE rather than as "no alerts". There is nothing
# in these tools that can enable capture, grant consent, change configuration or read media.
#
# THE TOKEN: the credential file must be mode 0600 and owned by the uid running this script (the server
# refuses anything else). If you do not have one, this script creates one under ~/.homeguardian with mode
# 0600 and NEVER prints the token: the commands it prints read the file themselves, so the secret does not
# land in your shell history or in this output.
#
#   bash scripts/hg_mcp_owner_endpoint.sh --store /path/to/alerts.db
#   bash scripts/hg_mcp_owner_endpoint.sh --store /path/to/alerts.db --port 8790
#   bash scripts/hg_mcp_owner_endpoint.sh --stop          # stop a previously started endpoint
#
# Options:
#   --store PATH       the alert store to read (REQUIRED unless HG_MCP_ALERTS_DB is set)
#   --port N           loopback port (default 8790)
#   --token-file PATH  MCP credential file, 0600 (default ~/.homeguardian/mcp_credentials.json)
#   --bin PATH         hg_mcp_server binary (default $HG_MCP_BIN, then ./build/hg_mcp_server)
#   --foreground       run in the foreground instead of detaching
#   --stop             stop the endpoint recorded in the pidfile and exit
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
STORE="${HG_MCP_ALERTS_DB:-}"
PORT=8790
TOKEN_FILE="${HOME}/.homeguardian/mcp_credentials.json"
BIN="${HG_MCP_BIN:-$REPO/build/hg_mcp_server}"
CONFIG="${HG_MCP_CONFIG:-$REPO/config/homeguardian.demo.json}"
STATE_DIR="${HOME}/.homeguardian"
PIDFILE="$STATE_DIR/mcp_endpoint.pid"
LOGFILE="$STATE_DIR/mcp_endpoint.log"
FOREGROUND=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --store)      STORE="${2:-}"; shift 2 ;;
    --port)       PORT="${2:-}"; shift 2 ;;
    --token-file) TOKEN_FILE="${2:-}"; shift 2 ;;
    --bin)        BIN="${2:-}"; shift 2 ;;
    --foreground) FOREGROUND=1; shift ;;
    --stop)
      if [[ -f "$PIDFILE" ]]; then
        pid="$(cat "$PIDFILE")"
        kill -TERM "$pid" 2>/dev/null && echo "stopped endpoint pid $pid" || echo "no process for pid $pid"
        rm -f "$PIDFILE"
      else
        echo "nothing to stop (no $PIDFILE)"
      fi
      exit 0 ;;
    -h|--help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
done

mkdir -p "$STATE_DIR"; chmod 700 "$STATE_DIR"

if [[ -z "$STORE" ]]; then
  cat >&2 <<'MSG'
The store to read must be named explicitly - this endpoint refuses to guess which file holds your alerts.

  --store /path/to/alerts.db            (or set HG_MCP_ALERTS_DB)

Where is it? It is the file the application writes (the ingress config's "audit_db_path"). For the compose
deployment that file is inside the hg_data volume, e.g.
  docker compose -f deploy/docker/docker-compose.yml exec ingress sh -c \
      "grep audit_db_path /etc/homeguardian/certs/ingress.json"
MSG
  exit 2
fi

if [[ ! -x "$BIN" ]]; then
  echo "the MCP binary is not executable: $BIN" >&2
  echo "build it with:  cmake --build build --target hg_mcp_server -j4" >&2
  exit 2
fi

if [[ ! -f "$STORE" ]]; then
  # Said plainly, because the alternative (creating it) is how "no storage" becomes "no alerts".
  echo "the store does not exist: $STORE" >&2
  echo "This endpoint will NOT create it. Point --store at the file the application writes." >&2
  exit 2
fi

if [[ ! -f "$TOKEN_FILE" ]]; then
  tok="owner-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
  printf '{"%s":{"principal":"local-owner","allow_status":true,"allow_alert_summary":true}}' "$tok" \
    > "$TOKEN_FILE"
  chmod 600 "$TOKEN_FILE"
  echo "created $TOKEN_FILE (mode 0600, token not shown)"
fi
perms="$(stat -c '%a' "$TOKEN_FILE" 2>/dev/null || stat -f '%Lp' "$TOKEN_FILE" 2>/dev/null)"
if [[ "$perms" != "600" ]]; then
  echo "refusing to start: $TOKEN_FILE has mode $perms; the credential store requires 0600" >&2
  exit 2
fi
if [[ "$(id -u)" != "$(stat -c '%u' "$TOKEN_FILE" 2>/dev/null || echo "$(id -u)")" ]]; then
  echo "refusing to start: $TOKEN_FILE is not owned by uid $(id -u) (the credential store requires it)" >&2
  exit 2
fi

# Refuse to start a second endpoint on top of a live one. The bind would fail and the process would exit
# after the port check, which reads as "the endpoint is up" while an OLD reader (with an older view of the
# store) answers. One endpoint at a time, named in the pidfile.
if [[ -f "$PIDFILE" ]]; then
  prev="$(cat "$PIDFILE" 2>/dev/null)"
  if [[ -n "$prev" ]] && kill -0 "$prev" 2>/dev/null; then
    echo "an endpoint is already running (pid $prev). Stop it first:" >&2
    echo "  bash scripts/hg_mcp_owner_endpoint.sh --stop" >&2
    exit 2
  fi
  rm -f "$PIDFILE"
fi

URL="http://127.0.0.1:${PORT}/mcp"
AUTH="Authorization: Bearer $(cat "$TOKEN_FILE" | python3 -c 'import json,sys;print(list(json.load(sys.stdin).keys())[0])')"
JSON="Content-Type: application/json"
ACCEPT="Accept: application/json, text/event-stream"

echo "== HomeGuardian MCP endpoint (read-only) =="
echo "  store   : $STORE"
echo "  binary  : $BIN"
echo "  endpoint: $URL   (loopback only; nothing is published)"
echo "  token   : read from $TOKEN_FILE by the commands below - not printed here"
echo

if (( FOREGROUND )); then
  echo "  running in the foreground; Ctrl-C to stop"
  exec env HG_MCP_PORT="$PORT" HG_MCP_ALERTS_DB="$STORE" "$BIN" "$CONFIG" "$TOKEN_FILE"
fi

env HG_MCP_PORT="$PORT" HG_MCP_ALERTS_DB="$STORE" "$BIN" "$CONFIG" "$TOKEN_FILE" \
  >>"$LOGFILE" 2>&1 &
pid=$!
echo "$pid" > "$PIDFILE"
for _ in $(seq 1 60); do
  if curl -s -o /dev/null --max-time 1 "$URL"; then break; fi
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "  the endpoint exited; its last words:" >&2
    tail -5 "$LOGFILE" >&2
    exit 1
  fi
  sleep 0.25
done

echo "  pid     : $pid    (stop with: bash scripts/hg_mcp_owner_endpoint.sh --stop)"
echo "  log     : $LOGFILE"
grep -o "alert store .*" "$LOGFILE" | tail -1 | sed 's/^/  startup : /'
echo

call() {  # $1 = session id (may be empty), $2 = json body
  local sid="$1" body="$2" args=(-s -H "$AUTH" -H "$JSON" -H "$ACCEPT")
  [[ -n "$sid" ]] && args+=(-H "Mcp-Session-Id: $sid")
  curl "${args[@]}" -X POST "$URL" -d "$body"
}
show() { python3 -c "
import json,sys
t=sys.stdin.read()
if 'data:' in t: t=[l[6:] for l in t.splitlines() if l.startswith('data: ')][0]
d=json.loads(t)
if 'error' in d: print('  ERROR: '+str(d['error'].get('message') or d['error'].get('code'))); raise SystemExit
c=d.get('result',{}).get('content',[{}])
print('  '+(c[0].get('text','') if c else json.dumps(d)))"; }

echo "== a real exchange, so you can see it working =="
SID="$(curl -s -D - -o /dev/null -H "$AUTH" -H "$JSON" -H "$ACCEPT" -X POST "$URL" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"owner","version":"1"}}}' \
      | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')"
echo "  session : ${SID:0:12}..."
echo "  tools/list:"
call "$SID" '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' | python3 -c "
import json,sys
t=sys.stdin.read()
if 'data:' in t: t=[l[6:] for l in t.splitlines() if l.startswith('data: ')][0]
d=json.loads(t)
if 'error' in d: print('  ERROR: '+str(d['error'])); raise SystemExit
for x in d.get('result',{}).get('tools',[]): print('   -', x['name'])"
echo "  get_homeguardian_status:"
call "$SID" '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_homeguardian_status","arguments":{}}}' | show
echo "  get_alert_summary:"
call "$SID" '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}' | show
echo

# Quoted heredoc on purpose: nothing here is expanded, so the printed example contains a COMMAND that reads
# the token file - never the token itself. An earlier version of this script interpolated the Authorization
# header into this text and printed the secret while promising it would not.
cat <<'MSG'
== call it yourself ==
  # The token is read FROM THE FILE by the commands below, so it never appears here and never lands in your
  # shell history. TOKEN_FILE is the credential path printed at the top of this output.
  TOKEN="$(python3 -c 'import json,sys;print(list(json.load(open(sys.argv[1])).keys())[0])' "$TOKEN_FILE")"
  URL="http://127.0.0.1:PORT/mcp"

  SID=$(curl -s -D - -o /dev/null -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
        -H 'Accept: application/json, text/event-stream' -X POST "$URL" \
        -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"owner","version":"1"}}}' \
        | grep -i '^mcp-session-id:' | tr -d '\r' | awk '{print $2}')

  curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
       -H 'Accept: application/json, text/event-stream' -H "Mcp-Session-Id: $SID" -X POST "$URL" \
       -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_alert_summary","arguments":{}}}'

== what to expect ==
  * alert_count tracks the alerts the APPLICATION wrote: it rises when an authorized observation is admitted
    and the release rule delivers an alert.
  * If the store is wrong or unreadable, the tools answer with an error ("the request could not be
    authorized") and never with a zero count. Zero is a fact; unavailable is not.
  * The listener is loopback-only. Nothing here can enable capture, grant consent, change configuration or
    read media.
MSG
echo "  (TOKEN_FILE for the block above: $TOKEN_FILE)"
