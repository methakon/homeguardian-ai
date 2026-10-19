#!/usr/bin/env bash
# verify_glibc_mcp_in_container.sh — run the genuine glibc aarch64 MCP engine inside an arm64
# Ubuntu 24.04 container and exercise the real MCP 2025-11-25 protocol surface.
#
# Why a container: the engine is aarch64/glibc and this workstation is x86-64, so it cannot execute
# natively. Running it under an arm64 Ubuntu container exercises the SAME libc and architecture the
# Mi A3's PRoot Ubuntu would use. That makes this real verification of the artifact, not a mock and
# not a claim about hardware.
#
# IMPORTANT: this is EMULATED ARM64 verification. It is NOT physical Mi A3 acceptance and must never
# be reported as such.
#
# Read-only w.r.t. the device: the device is not touched at all here.
set -u

BIN="${1:-/tmp/glibc_arm64/hg_mcp_server}"
OUT="${2:-/tmp/glibc_verify}"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$BIN" "$OUT/hg_mcp_server"

echo "########## Phase C: genuine glibc aarch64 MCP engine verification ##########"
echo "  artifact: $BIN"
echo "  size    : $(stat -c%s "$BIN") bytes"
echo "  sha256  : $(sha256sum "$BIN" | awk '{print $1}')"
echo

docker run --rm --platform linux/arm64 \
  -v "$OUT":/work \
  ubuntu:24.04 bash -c '
set -u
export DEBIAN_FRONTEND=noninteractive
# The bare ubuntu image lacks the probe tools; the first run failed only because of that.
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq file curl xxd iproute2 >/dev/null 2>&1
cd /work
chmod +x hg_mcp_server

pass=0; fail=0
ok()  { pass=$((pass+1)); printf "  [PASS] %s\n" "$1"; }
bad() { fail=$((fail+1)); printf "  [FAIL] %s -- %s\n" "$1" "$2"; }
chk() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1" "got=$2 want=$3"; fi; }

echo "== 1. executable architecture and dependency compatibility =="
uname -m | sed "s/^/  host arch: /"
file hg_mcp_server | sed "s/^/  /"
echo "  dynamic linker: $(readelf -l hg_mcp_server 2>/dev/null | grep -o "/lib/ld-linux-aarch64[^ ]*" | head -1)"
echo "  NEEDED:"; readelf -d hg_mcp_server 2>/dev/null | grep NEEDED | sed "s/^/    /"
echo "  interpreter present on this system: $([ -e /lib/ld-linux-aarch64.so.1 ] && echo yes || echo NO)"
echo "  ldd:"; ldd hg_mcp_server 2>&1 | sed "s/^/    /"

echo
echo "== 2. service startup =="
TOK=$(head -c 32 /dev/urandom | xxd -p -c 64)
printf "{ \"%s\": { \"principal\": \"glibc-verify\", \"allow_status\": true, \"allow_alert_summary\": true } }\n" "$TOK" > creds.json
chmod 600 creds.json
cat > cfg.json <<JSON
{ "log_level": "warn", "server_host": "127.0.0.1", "server_port": 8080,
  "data_dir": "/tmp/hgdata", "max_event_history": 1000, "correlation_window_ms": 5000,
  "alert_confidence_threshold": 0.5, "media_capture_enabled": false,
  "cloud_processing_enabled": false, "consent_policy_version": "1.0.0",
  "routine_retention_days": 730, "profile_retention_days": 1825, "alert_retention_days": 30 }
JSON
mkdir -p /tmp/hgdata
HG_MCP_PORT=8790 HG_MCP_BIND=127.0.0.1 HG_MCP_ALLOW_REMOTE=0 ./hg_mcp_server cfg.json creds.json > eng.log 2>&1 &
EP=$!
for _ in $(seq 1 100); do grep -q "listening on" eng.log 2>/dev/null && break; sleep 0.1; done
if grep -q "listening on 127.0.0.1" eng.log; then ok "engine started, loopback only"; else bad "startup" "$(tail -3 eng.log)"; fi
echo "  RSS: $(awk "/VmRSS/{print \$2\" kB\"}" /proc/$EP/status 2>/dev/null)"

U=http://127.0.0.1:8790/mcp
A=(-H "Authorization: Bearer $TOK" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream")

echo
echo "== 3. MCP initialize (2025-11-25) =="
curl -s -D h.txt -o init.json -X POST "$U" "${A[@]}" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"v\",\"version\":\"1\"}}}"
grep -q "homeguardian-mcp" init.json && ok "initialize returns serverInfo" || bad "initialize" "$(head -c 120 init.json)"
grep -q "2025-11-25" init.json && ok "protocolVersion 2025-11-25" || bad "protocol" "$(head -c 120 init.json)"
SID=$(grep -i "^Mcp-Session-Id:" h.txt | tr -d "\r" | awk "{print \$2}")
[ -n "$SID" ] && ok "session id issued (${SID:0:12}...)" || bad "session" "none"

echo
echo "== 4. tools/list exposes EXACTLY two read-only tools =="
curl -s -o tools.json -X POST "$U" "${A[@]}" -H "Mcp-Session-Id: $SID" -d "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"
N=$(grep -o "\"name\":\"[^\"]*\"" tools.json | wc -l)
chk "tool count" "$N" "2"
grep -q "get_homeguardian_status" tools.json && ok "get_homeguardian_status present" || bad "tool1" "missing"
grep -q "get_alert_summary"      tools.json && ok "get_alert_summary present"      || bad "tool2" "missing"

echo
echo "== 5/6. both read-only tools/call paths =="
S=$(curl -s -X POST "$U" "${A[@]}" -H "Mcp-Session-Id: $SID" -d "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get_homeguardian_status\",\"arguments\":{}}}")
printf "%s" "$S" | grep -q "capture_enabled" && ok "get_homeguardian_status -> real engine state" || bad "status call" "$(printf "%s" "$S" | head -c 120)"
printf "%s" "$S" | grep -q "read_only"       && ok "status reports read_only"           || bad "read_only" "absent"
A2=$(curl -s -X POST "$U" "${A[@]}" -H "Mcp-Session-Id: $SID" -d "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_alert_summary\",\"arguments\":{}}}")
printf "%s" "$A2" | grep -q "alert_count" && ok "get_alert_summary -> real engine state" || bad "alert call" "$(printf "%s" "$A2" | head -c 120)"
echo "  status payload: $(printf "%s" "$S" | head -c 130)"

echo
echo "== 7. unauthorized refusal =="
c=$(curl -s -m 8 -o /dev/null -w "%{http_code}" -X POST "$U" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -d "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}")
chk "no Authorization header" "$c" "401"
c=$(curl -s -m 8 -o /dev/null -w "%{http_code}" -X POST "$U" -H "Authorization: Bearer wrong" -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" -d "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}")
chk "bogus bearer" "$c" "401"

echo
echo "== 8. session isolation =="
r=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$U" "${A[@]}" -d "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}")
chk "call with no session id" "$r" "400"
r=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$U" "${A[@]}" -H "Mcp-Session-Id: bogus" -d "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/list\"}")
chk "call with bogus session id" "$r" "404"

echo
echo "== 10. no public unauthenticated listener =="
ss -ltn 2>/dev/null | awk "NR>1 {print \$4}" | sed "s/^/    listener: /"
if ss -ltn 2>/dev/null | awk "NR>1 {print \$4}" | grep -qE "^0\.0\.0\.0|^\[::\]"; then
  bad "loopback-only" "a wildcard listener is present"
else
  ok "all listeners loopback-only"
fi

echo
echo "== 9. safe process shutdown =="
kill -TERM $EP 2>/dev/null
sleep 2
if kill -0 $EP 2>/dev/null; then bad "shutdown" "process survived SIGTERM"; kill -9 $EP 2>/dev/null
else ok "clean SIGTERM shutdown"; fi
echo "  engine server-side accounting:"; grep -o "requests=.*errors=[0-9]*" eng.log | tail -1 | sed "s/^/    /"

echo
echo "== resource measurement of the genuine engine (glibc aarch64) =="
echo "  binary: $(stat -c%s hg_mcp_server) bytes"
echo "  (RSS measured above during the run)"

echo
echo "########## RESULT: $pass passed, $fail failed ##########"
[ "$fail" -eq 0 ] || exit 1
'
echo "### container exit=$? ###"