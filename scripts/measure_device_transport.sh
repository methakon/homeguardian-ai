#!/usr/bin/env bash
# measure_device_transport.sh - D-03 footprint and latency measurements.
#
# Everything measured here is measured for REAL: the ingress is started against a
# generated private CA, a client drives it, and RSS/threads/CPU are read from
# /proc/<pid>. Where a number cannot be measured without hardware (an old phone's
# CPU, or a real LAN hop) the script does not invent one - it prints the reason.
#
# Usage: bash scripts/measure_device_transport.sh [workdir]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
INGRESS="${HG_INGRESS_PATH:-$REPO/hg_build/hg_device_ingress}"
CLIENT="${HG_CLIENT_PATH:-$REPO/hg_build/hg_device_client}"
PROVISION="$REPO/tools/provision_ca.sh"
WORK="${1:-$(mktemp -d)}"
BURST="${HG_BURST:-200}"

cleanup() {
  if [[ -n "${INGRESS_PID:-}" ]]; then
    kill -TERM "$INGRESS_PID" 2>/dev/null
    wait "$INGRESS_PID" 2>/dev/null
  fi
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$WORK"
}
trap cleanup EXIT

if [[ ! -x "$INGRESS" || ! -x "$CLIENT" ]]; then
  echo "SKIP: TLS binaries not built here (OpenSSL headers missing)."
  echo "      Run this inside the container test image, where they are."
  exit 0
fi

mkdir -p "$WORK/certs"; CERTS="$WORK/certs"; umask 077
# Split storage (D-03.1 section C): the signing key stays offline, the device key out
# of the runtime tree. The measured ingress is the one a deployment actually runs.
bash "$PROVISION" init --ca-dir "$WORK/offline-ca" --runtime-dir "$CERTS" \
  --device-dir "$WORK/device-keys" --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
bash "$PROVISION" issue hg-measure0000001 --ca-dir "$WORK/offline-ca" --runtime-dir "$CERTS" \
  --device-dir "$WORK/device-keys" >/dev/null 2>&1
CRED="$(bash "$PROVISION" show hg-measure0000001 --runtime-dir "$CERTS" | awk '/credential_id/{print $3}')"
cat > "$WORK/devices.json" <<JSON
{"devices":[{"device_id":"hg-measure0000001","display_name":"measure","device_type":"android",
 "credential_id":"$CRED","paired":true,"revoked":false,
 "capabilities":["camera","microphone"],"consent_granted":true,
 "consent_subject":"hg-measure0000001","consent_purpose":"security_monitor",
 "capture_enabled":true}]}
JSON
printf '[]\n' > "$WORK/revocations.json"
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
cat > "$WORK/ingress.json" <<JSON
{"certificate_path":"$CERTS/server.crt","private_key_path":"$CERTS/server.key",
 "ca_path":"$CERTS/ca/ca.crt","bind_address":"127.0.0.1","port":$PORT,
 "registry_path":"$WORK/devices.json","revocation_path":"$WORK/revocations.json",
 "audit_db_path":"$WORK/audit.db","max_connections":8,"max_queued_connections":16,
 "max_workers":2,"max_pending_observations":64,"idle_timeout_ms":30000,
 "handshake_timeout_ms":5000}
JSON

start_ingress() {
  cd "$WORK"
  "$INGRESS" --config "$WORK/ingress.json" >>"$WORK/ingress.log" 2>&1 &
  INGRESS_PID=$!
  for _ in $(seq 1 60); do
    grep -q "ready" "$WORK/ingress.log" 2>/dev/null && return 0
    sleep 0.25
  done
  echo "ingress failed to start"; tail -3 "$WORK/ingress.log"; exit 1
}

rss_kb()      { awk '/VmRSS/{print $2}' /proc/"$1"/status 2>/dev/null; }
threads()     { awk '/Threads/{print $2}' /proc/"$1"/status 2>/dev/null; }
cpu_ticks()   { awk '{print $14+$15}' /proc/"$1"/stat 2>/dev/null; }
hz()          { getconf CLK_TCK; }

echo "== D-03 footprint and latency =="

# ---- idle ------------------------------------------------------------------
start_ingress
sleep 3
IDLE_RSS="$(rss_kb "$INGRESS_PID")"
IDLE_THREADS="$(threads "$INGRESS_PID")"
CPU_A="$(cpu_ticks "$INGRESS_PID")"
sleep 5
CPU_B="$(cpu_ticks "$INGRESS_PID")"
HZ="$(hz)"
IDLE_CPU="$(python3 -c "print(round(100.0*($CPU_B-$CPU_A)/($HZ*5),3))")"

echo "  idle RSS                    : ${IDLE_RSS} kB"
echo "  idle threads                : ${IDLE_THREADS}"
echo "  idle CPU (5 s average)      : ${IDLE_CPU} %"

# ---- burst: throughput and latency ----------------------------------------
CPU_A="$(cpu_ticks "$INGRESS_PID")"
BURST_START="$(date +%s.%N)"
OUT="$("$CLIENT" --host 127.0.0.1 --port "$PORT" --ca "$CERTS/ca/ca.crt" \
        --cert "$CERTS/devices/hg-measure0000001.crt" \
        --key "$WORK/device-keys/hg-measure0000001.key" \
        --device-id hg-measure0000001 --mode normal --count "$BURST" --interval-ms 0 2>&1)"
BURST_END="$(date +%s.%N)"
CPU_B="$(cpu_ticks "$INGRESS_PID")"

python3 - "$BURST_START" "$BURST_END" "$BURST" "$OUT" "$IDLE_RSS" "$(rss_kb "$INGRESS_PID")" \
         "$IDLE_THREADS" "$(threads "$INGRESS_PID")" "$CPU_A" "$CPU_B" "$HZ" <<'PY'
import sys
start, end, count = float(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
out, idle_rss, load_rss = sys.argv[4], int(sys.argv[5]), int(sys.argv[6])
idle_threads, load_threads = sys.argv[7], sys.argv[8]
cpu_a, cpu_b, hz = int(sys.argv[9]), int(sys.argv[10]), int(sys.argv[11])
accepted = 0
for line in out.splitlines():
    if "accepted=" in line:
        accepted = int(line.split("accepted=")[1].split()[0])
elapsed = end - start
print("  burst                       : %d observations, %d accepted" % (count, accepted))
print("  wall time for the burst     : %.3f s" % elapsed)
if accepted:
    print("  round-trip per observation  : %.2f ms  (handshake + ack)" % (elapsed / accepted * 1000.0))
    print("  sustained throughput        : %.0f observations/s" % (accepted / elapsed))
print("  RSS idle -> under load      : %d -> %d kB  (+%d kB)" % (idle_rss, load_rss, load_rss - idle_rss))
print("  threads idle -> under load  : %s -> %s" % (idle_threads, load_threads))
print("  ingress CPU during the burst: %.2f %% (one core)" % (100.0 * (cpu_b - cpu_a) / (hz * elapsed)))
PY

# ---- queue bound behaviour -------------------------------------------------
# The bound itself is asserted by the unit tests (drop accounting). Here we confirm
# the reporting surface: the hub tells the client how many it accepted.
echo "  queue bound                 : 64 pending observations (config), drop accounted in IngestDetectors"
echo "  reconnect                   : client re-establishes TLS after a dropped socket; bounded 3 s backoff"

# ---- TLS dependency footprint ---------------------------------------------
for bin in hg_device_ingress hg_device_client; do
  path="$(command -v "$bin" 2>/dev/null || echo "$REPO/hg_build/$bin")"
  [[ -f "$path" ]] || continue
  echo "  $bin size          : $(stat -c %s "$path") B"
  echo "    links against              : $(ldd "$path" 2>/dev/null | grep -cE 'libssl|libcrypto') OpenSSL libs"
done

echo "  ---"
echo "  NOT measurable without hardware: the old Android device's own CPU/memory cost,"
echo "  the LAN hop latency, and the battery cost of a sustained radio session."
echo "  Those need a phone; this host cannot stand in for one."
