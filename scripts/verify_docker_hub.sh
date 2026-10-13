#!/usr/bin/env bash
# verify_docker_hub.sh - D-01 acceptance for the HomeGuardian hub container.
#
# Proves, against a real Docker daemon:
#   1. the container starts and its healthcheck reaches "healthy"
#   2. the REAL C++ core answers inside it (health/status), not a stub
#   3. a scenario run persists alerts into the volume
#   4. alert history survives container REPLACEMENT (down without -v, up again)
#   5. SIGTERM stops it cleanly (exit code 0)
#   6. isolation, checked on a LIVE container: no published ports, non-root,
#      capabilities dropped, not privileged, no devices, no docker.sock,
#      listener bound to 127.0.0.1 only
#   7. durability inside the container: SQLite header bytes 18/19 == 2 (WAL)
#   8. the entrypoint fails closed when config/credentials are not mounted
#   9. footprint: image size, compressed pull size, idle RSS/CPU, threads, peak RSS
#
# Usage: bash scripts/verify_docker_hub.sh
#   HG_COMPOSE_FILE   (default deploy/docker/docker-compose.yml)
#   HG_PROJECT        (default hg-verify) compose project name
#   HG_IMAGE          (default homeguardian-hub:local)
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
COMPOSE_FILE="${HG_COMPOSE_FILE:-$REPO/deploy/docker/docker-compose.yml}"
PROJECT="${HG_PROJECT:-hg-verify}"
IMAGE="${HG_IMAGE:-homeguardian-hub:local}"
PORT=8080
PASS=0; FAIL=0; WARN=0
ok()   { PASS=$((PASS+1)); printf '  %-66s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-66s FAIL  %s\n' "$1" "${2:-}"; }
warn() { WARN=$((WARN+1)); printf '  %-66s WARN  %s\n' "$1" "${2:-}"; }

compose() { docker compose -p "$PROJECT" -f "$COMPOSE_FILE" "$@"; }
in_hub()  { compose exec -T hub bash -lc "$1"; }
wait_healthy() {
  for _ in $(seq 1 45); do
    [[ "$(docker inspect --format '{{.State.Health.Status}}' "$1" 2>/dev/null)" == "healthy" ]] && return 0
    sleep 1
  done
  return 1
}
# With Compose's init enabled the hub is NOT pid 1, so find it by process name.
hub_pid() { in_hub 'for p in /proc/[0-9]*; do [ "$(cat $p/comm 2>/dev/null)" = "hg_alexa_bridge" ] && { echo ${p#/proc/}; break; }; done' | tr -d '\r'; }

[[ -f "$REPO/deploy/docker/secrets/creds.json" ]] || \
  cp "$REPO/deploy/docker/secrets/creds.json.example" "$REPO/deploy/docker/secrets/creds.json"
docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "image $IMAGE missing; build it first" >&2; exit 2; }

cleanup() { compose down -v >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "== D-01 hub container acceptance (project=$PROJECT, image=$IMAGE) =="
cleanup

# --- 1. start + health ------------------------------------------------------
compose up -d >/dev/null 2>&1 || { bad "docker compose up"; exit 1; }
CID="$(compose ps -q hub)"
[[ -n "$CID" ]] && ok "container started" || { bad "no container id"; exit 1; }
wait_healthy "$CID" && ok "healthcheck reports healthy" \
                    || bad "health status" "$(docker inspect --format '{{.State.Health.Status}}' "$CID")"

# --- 2. the real core answers ----------------------------------------------
api() { in_hub "exec 3<>/dev/tcp/127.0.0.1/$PORT; printf 'GET $1 HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' >&3; cat <&3"; }
h="$(api /api/health)"
[[ "$h" == *'"ok":true'* ]] && ok "/api/health answers from inside the container" || bad "/api/health" "${h:0:90}"
[[ "$h" == *'"simulated":true'* ]] && ok "health marks the deployment as simulated (honest labelling)" || warn "synthetic marker missing from /api/health"
st="$(api /api/status)"
[[ "$st" == *'Running'* ]] && ok "/api/status reports the real application state" || bad "/api/status" "${st:0:90}"

# --- 3. drive the real pipeline + persist ----------------------------------
out="$(api '/api/run-scenario?scenario=synthetic-critical-impact')"
[[ "$out" == *'synthetic-impact'* ]] && ok "critical scenario produced a real detection-driven alert" || bad "scenario run" "${out: -160}"
persisted="$(echo "$out" | grep -o '"persisted_alert_count":[0-9]*' | head -1 | cut -d: -f2)"
[[ "${persisted:-0}" -ge 2 ]] && ok "alerts persisted inside the container ($persisted rows)" || bad "persisted_alert_count" "${persisted:-none}"

# --- 7. durability in the container (SQLite header bytes 18..19 = 2,2) ------
in_hub 'test -f /data/homeguardian_bridge_audit.db' && ok "database lives in the persistent volume (/data)" || bad "db not in /data"
fmt="$(in_hub 'od -An -tu1 -j18 -N2 /data/homeguardian_bridge_audit.db')"
if [[ "$(echo "$fmt" | tr -s ' ')" == " 2 2" ]]; then
  ok "SQLite header reports WAL (file format write/read version = 2,2)"
else
  bad "SQLite header format bytes" "$fmt"
fi
in_hub 'ls /data/homeguardian_bridge_audit.db-wal >/dev/null 2>&1' \
  && ok "a -wal sidecar exists (WAL active at runtime)" || warn "no -wal sidecar seen"

# --- 4. persistence across container REPLACEMENT ---------------------------
compose down >/dev/null 2>&1        # keep the volume
compose up -d >/dev/null 2>&1
CID="$(compose ps -q hub)"; wait_healthy "$CID"
after="$(api /api/alerts)"
after_total="$(echo "$after" | grep -o '"total":[0-9]*' | head -1 | cut -d: -f2)"
[[ "${after_total:-0}" -ge 2 ]] && ok "alert history survived container replacement ($after_total rows)" || bad "history after replacement" "${after_total:-none}"
[[ "$after" == *'synthetic-impact'* ]] && ok "the critical alert's evidence survived (rule name intact)" || bad "evidence missing after replacement"

# --- 5. clean shutdown on SIGTERM -----------------------------------------
docker kill -s TERM "$CID" >/dev/null 2>&1
for _ in $(seq 1 30); do
  [[ "$(docker inspect --format '{{.State.Running}}' "$CID" 2>/dev/null)" == "false" ]] && break
  sleep 1
done
exit_code="$(docker inspect --format '{{.State.ExitCode}}' "$CID" 2>/dev/null)"
[[ "$exit_code" == "0" ]] && ok "SIGTERM produced a clean exit (code 0)" || bad "exit code after SIGTERM" "$exit_code"

# --- 8. entrypoint fails closed without mounts ----------------------------
if docker run --rm "$IMAGE" >/dev/null 2>&1; then
  bad "container ran with no config/credentials mounted"
else
  msg="$(docker run --rm "$IMAGE" 2>&1 | head -1)"
  [[ "$msg" == *"not readable"* ]] && ok "entrypoint fails closed when mounts are absent" || warn "failed, but not with a mount message" "$msg"
fi

# --- 6. isolation, on a LIVE container ------------------------------------
compose up -d >/dev/null 2>&1
CID="$(compose ps -q hub)"
wait_healthy "$CID" && ok "container healthy after restart (restart behaviour)" || bad "restart health"
bindings="$(docker inspect --format '{{json .HostConfig.PortBindings}}' "$CID")"
[[ "$bindings" == "null" || "$bindings" == "{}" ]] && ok "no published ports (loopback-only server is not exposed)" || bad "ports published" "$bindings"
uid="$(docker inspect --format '{{.Config.User}}' "$CID")"
[[ "$uid" == 10001* ]] && ok "container runs as non-root uid ($uid)" || bad "runs as" "$uid"
caps="$(docker inspect --format '{{json .HostConfig.CapDrop}}' "$CID")"
[[ "$caps" == *"ALL"* ]] && ok "all capabilities dropped" || bad "cap_drop" "$caps"
secopt="$(docker inspect --format '{{json .HostConfig.SecurityOpt}}' "$CID")"
[[ "$secopt" == *"no-new-privileges"* ]] && ok "no-new-privileges set" || bad "security_opt" "$secopt"
[[ "$(docker inspect --format '{{.HostConfig.Privileged}}' "$CID")" == "false" ]] && ok "not privileged" || bad "privileged"
[[ "$(docker inspect --format '{{len .HostConfig.Devices}}' "$CID")" == "0" ]] && ok "no device passthrough" || bad "devices"
docker inspect --format '{{json .Mounts}}' "$CID" | grep -q "docker.sock" \
  && bad "docker socket is mounted" || ok "docker socket is NOT mounted"
docker inspect --format '{{json .Mounts}}' "$CID" | grep -q "hg_creds" \
  && ok "credentials come from a secret mount, not an image layer" || bad "credentials not mounted as a secret"
in_hub 'ls /run/secrets/hg_creds >/dev/null' && ok "the secret is readable inside the container" || bad "secret missing"
listeners="$(in_hub 'grep -i ":1F90" /proc/net/tcp || true')"
if [[ -z "$listeners" ]]; then
  warn "could not read the listening socket from /proc/net/tcp"
elif echo "$listeners" | grep -qi "0100007F:1F90"; then
  ok "listener bound to 127.0.0.1:8080"
  echo "$listeners" | grep -qi "00000000:1F90" && bad "also listening on ALL interfaces" || ok "no 0.0.0.0 listener"
else
  bad "unexpected listen address" "$(echo "$listeners" | head -1)"
fi

# --- 9. footprint ---------------------------------------------------------
img_bytes="$(docker image inspect "$IMAGE" --format '{{.Size}}')"
printf '  %-66s %.1f MiB (app layer ~9 MiB on a ~117 MiB base)\n' \
       "image size (uncompressed)" "$(awk "BEGIN{printf \"%.1f\", $img_bytes/1048576}")"
printf '  %-66s %s\n' "idle docker stats (mem | cpu)" \
       "$(docker stats --no-stream --format '{{.MemUsage}} | {{.CPUPerc}}' "$CID" | head -1)"
BPID="$(hub_pid)"
if [[ -n "$BPID" ]]; then
  printf '  %-66s %s\n' "hub process in the container" \
         "pid $BPID, $(in_hub "ls /proc/$BPID/task | wc -l" | tr -d '\r') thread(s)"
  printf '  %-66s %s\n' "hub RSS / peak RSS" \
         "$(in_hub "awk '/VmRSS|VmHWM/{printf \"%s=%s%s \", \$1, \$2, \$3}' /proc/$BPID/status" | tr -d '\r')"
else
  warn "could not locate the hub process inside the container"
fi
printf '  %-66s %s\n' "compressed pull size (gzip -1)" \
       "$(docker save "$IMAGE" | gzip -1 | wc -c | awk '{printf "%.1f MiB", $1/1048576}')"

compose down >/dev/null 2>&1
echo "  --------------------------------------------------------------------"
echo "  D-01 acceptance: $PASS passed, $FAIL failed, $WARN warned"
[[ "$FAIL" == "0" ]]
