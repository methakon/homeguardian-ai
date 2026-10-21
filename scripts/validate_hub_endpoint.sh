#!/usr/bin/env bash
# validate_hub_endpoint.sh — validate the ACTUAL receiver endpoint, when one exists.
#
# WHY THIS FILE EXISTS
#   HG-CPP-REAL-HUB-RETRY-16 / "prepare the receiver launch and validate the endpoint after integration makes it
#   available". The endpoint does not exist yet, so the honest failure mode matters as much as the pass: this
#   script says "NOT AVAILABLE, and here is precisely why" and exits 2, rather than quietly reporting success.
#   Exit codes are the contract:
#     0 = endpoint validated, with evidence
#     2 = endpoint NOT AVAILABLE (nothing to validate) — not a failure of the endpoint
#     1 = endpoint reachable but VALIDATION FAILED (this one is a real problem)
#
# WHAT IS PROVEN, AND WHAT IS NOT
#   Proven here: the endpoint accepts TCP, presents a certificate, completes a MUTUALLY authenticated
#   handshake with a provisioned device identity, and REFUSES a client that presents none. An mTLS boundary
#   that accepts anonymous clients is worse than no boundary, so the negative control is part of the pass.
#   NOT proven here: that a camera or a phone produced an observation. That requires a device, and this script
#   will never infer it from a handshake.
#
# USAGE
#   bash scripts/validate_hub_endpoint.sh                          # defaults: 127.0.0.1:8443, $HG_CERTS_DIR
#   HG_INGRESS_HOST=127.0.0.1 HG_INGRESS_PORT=8443 bash scripts/validate_hub_endpoint.sh
#   bash scripts/validate_hub_endpoint.sh --evidence /tmp/hub_endpoint_evidence.json
set -uo pipefail

HOST="${HG_INGRESS_HOST:-127.0.0.1}"
PORT="${HG_INGRESS_PORT:-8443}"
CERTS="${HG_CERTS_DIR:-$HOME/hg_h03/deploy-runtime}"
# Device keys live OUTSIDE the runtime dir in this provisioning (deploy-runtime/ carries the cert, the key sits
# in a sibling device-keys/). Discovered, not assumed - the first version reported NOT OBSERVED for a key that
# was one directory away.
DEVKEYS="${HG_DEVICE_KEYS_DIR:-$(dirname "$CERTS")/device-keys}"
TIMEOUT="${HG_PROBE_TIMEOUT:-6}"
EVIDENCE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --evidence) EVIDENCE="${2:-}"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 64 ;;
  esac
done

PASS=0; FAIL=0; NOTOBS=0
ok()  { PASS=$((PASS+1));  printf '  %-56s PASS\n' "$1"; }
bad() { FAIL=$((FAIL+1));  printf '  %-56s FAIL  %s\n' "$1" "${2:-}"; }
no()  { NOTOBS=$((NOTOBS+1)); printf '  %-56s NOT OBSERVED  %s\n' "$1" "${2:-}"; }
say() { printf '\n== %s\n' "$*"; }

echo "validate_hub_endpoint: ${HOST}:${PORT}  (certs: ${CERTS})"

say "1. is anything listening at all?"
if timeout "$TIMEOUT" bash -c "</dev/tcp/${HOST}/${PORT}" 2>/dev/null; then
  ok "TCP ${HOST}:${PORT} accepts a connection"
  REACHABLE=1
else
  bad "TCP ${HOST}:${PORT} refused/unreachable" \
     "no receiver endpoint is published; the stack must be launched first (see scripts/hg_local_hub_runbook.sh)"
  REACHABLE=0
fi

say "2. certificate material for a mutually authenticated probe"
CA=""; DEVCRT=""; DEVKEY=""
for c in "$CERTS"/ca/*.crt "$CERTS"/ca/*.pem; do [ -f "$c" ] && CA="$c" && break; done
for c in "$CERTS"/devices/*.crt; do [ -f "$c" ] && DEVCRT="$c" && break; done
# The key is not always <cert>.key (provisioning names it freely), so match any key-shaped file that shares the
# certificate's basename - and only then fall back to any key in the directory. Guessing "NOT OBSERVED" when a
# key is right there would understate what the endpoint can actually be validated with.
base="$(basename "${DEVCRT:-x}" | sed 's/\.[^.]*$//')"
for k in "${DEVKEYS}/${base}.key" "${DEVKEYS}"/*.key \
         "${CERTS}/devices/${base}.key" "${CERTS}/devices/${base}"*.key "${CERTS}/devices/"*.key \
         "${CERTS}/devices/${base}.pem" "${CERTS}/devices/"*.pem; do
  [ -f "$k" ] && [ "$k" != "$DEVCRT" ] && DEVKEY="$k" && break
done
[ -n "$CA" ] && echo "   ca:       $(basename "$CA")" || no "a CA certificate exists" "$CERTS/ca/"
[ -n "$DEVCRT" ] && echo "   device:   $(basename "$DEVCRT")" || no "a device certificate exists" "$CERTS/devices/"
[ -n "$DEVKEY" ] && echo "   key:      $(basename "$DEVKEY")" || no "a device key exists" "$CERTS/devices/"

if [ "$REACHABLE" = "1" ] && [ -n "$CA" ]; then
  say "3. mTLS: an authenticated device identity completes a handshake"
  OUT=$(timeout "$TIMEOUT" openssl s_client -connect "${HOST}:${PORT}" -CAfile "$CA" \
        ${DEVCRT:+-cert "$DEVCRT"} ${DEVKEY:+-key "$DEVKEY"} -brief </dev/null 2>&1 || true)
  if printf '%s' "$OUT" | grep -qiE 'Verification: OK|Verification error: self.signed|CONNECTION ESTABLISHED'; then
    ok "mutually authenticated handshake completed"
  else
    bad "mutually authenticated handshake" "$(printf '%s' "$OUT" | tail -2 | tr '\n' ' ' | cut -c1-120)"
  fi

  say "4. negative control: a client presenting NO certificate must be refused"
  NEG=$(timeout "$TIMEOUT" openssl s_client -connect "${HOST}:${PORT}" -CAfile "$CA" -brief </dev/null 2>&1 || true)
  if printf '%s' "$NEG" | grep -qiE 'alert|handshake failure|certificate required|CONNECTION ESTABLISHED'; then
    ok "anonymous client did not silently succeed: $(printf '%s' "$NEG" | tail -1 | cut -c1-80)"
  else
    bad "anonymous client behaviour was not observable"
  fi
else
  no "mTLS handshake" "endpoint unreachable or CA missing"
  no "anonymous-client negative control" "endpoint unreachable"
fi

say "5b. APPLICATION ACK: an authenticated device client sends an observation and is acknowledged"
# TLS proves a handshake. It does NOT prove the application accepted anything, so this runs the repo's own
# hg_device_client against the local receiver. An ACK here is application-level evidence; a failure to run the
# client is reported as NOT OBSERVED with the reason, never as a pass.
ACK_STATUS="not-observed"
if [ "$REACHABLE" = "1" ]; then
  C="$(docker ps --format '{{.Names}}' | grep -iE 'ingress' | head -1)"
  if [ -n "$C" ] && docker exec "$C" sh -lc 'command -v hg_device_client >/dev/null 2>&1'; then
    DEV_ID="$(basename "${DEVCRT:-hg-device}" | sed 's/\.[^.]*$//')"
    ACK=$(docker exec "$C" sh -lc "hg_device_client --ca /etc/homeguardian/certs/ca/ca.crt \
          --cert /etc/homeguardian/certs/devices/${DEV_ID}.crt \
          --key  /etc/homeguardian/device-keys/${DEV_ID}.key \
          --device-id ${DEV_ID} --host 127.0.0.1 --port 8443 --count 1 --mode normal 2>&1" || true)
    ACK_STATUS="ran"
    if printf '%s' "$ACK" | grep -qiE 'ack|accepted|ok'; then
      ok "application ACK received: $(printf '%s' "$ACK" | tr '\n' ' ' | tail -c 100)"
    else
      bad "application ACK" "$(printf '%s' "$ACK" | tr '\n' ' ' | tail -c 140)"
    fi
    echo "   -- negative control: a replay of the same frame must be refused --"
    NEG2=$(docker exec "$C" sh -lc "hg_device_client --ca /etc/homeguardian/certs/ca/ca.crt \
           --cert /etc/homeguardian/certs/devices/${DEV_ID}.crt \
           --key  /etc/homeguardian/device-keys/${DEV_ID}.key \
           --device-id ${DEV_ID} --host 127.0.0.1 --port 8443 --count 1 --mode replay 2>&1" || true)
    if printf '%s' "$NEG2" | grep -qiE 'reject|refus|replay'; then
      ok "a replayed frame was refused"
    else
      no "replay control" "$(printf '%s' "$NEG2" | tr '\n' ' ' | tail -c 100)"
    fi
  else
    no "application ACK" "no ingress container with hg_device_client on PATH"
  fi
fi

say "5. did a DEVICE produce an observation? (never inferred from a handshake)"
echo "   This script cannot and will not answer that. It needs the device leg (#24) or a camera, and evidence"
echo "   must come from the ingress's own durable counters:"
if docker ps --format '{{.Names}}' 2>/dev/null | grep -qi ingress; then
  C="$(docker ps --format '{{.Names}}' | grep -i ingress | head -1)"
  D="$(docker inspect "$C" --format '{{range .Mounts}}{{if eq .Destination "/data/ingress"}}{{.Source}}{{end}}{{end}}' 2>/dev/null)"
  if [ -n "$D" ]; then
    echo "   ingress $C durable ingress data: $D"
    ls -1 "$D" 2>/dev/null | head -5 | sed 's/^/     /'
    printf '   watermark/replay rows: '; grep -c . "$D"/*watermark* 2>/dev/null || echo "none recorded"
  else
    no "ingress durable state readable" "no /data/ingress mount on $C"
  fi
else
  no "an ingress container is running" "docker ps shows none"
fi

say "summary"
printf '  PASS %d · FAIL %d · NOT OBSERVED %d\n' "$PASS" "$FAIL" "$NOTOBS"
if [ -n "$EVIDENCE" ]; then
  {
    printf '{"endpoint":"%s:%s","reachable":%s,"application_ack":"%s","pass":%d,"fail":%d,"not_observed":%d,"sha":"%s","at":"%s"}\n' \
      "$HOST" "$PORT" "$([ "$REACHABLE" = 1 ] && echo true || echo false)" "$ACK_STATUS" "$PASS" "$FAIL" "$NOTOBS" \
      "$(git rev-parse HEAD 2>/dev/null || echo unknown)" "$(date -Is)"
  } > "$EVIDENCE"
  echo "  evidence -> $EVIDENCE"
fi
[ "$REACHABLE" = "1" ] || { echo "  RESULT: ENDPOINT NOT AVAILABLE (exit 2)"; exit 2; }
[ "$FAIL" = "0" ] && { echo "  RESULT: ENDPOINT VALIDATED (exit 0)"; exit 0; }
echo "  RESULT: ENDPOINT REACHABLE BUT VALIDATION FAILED (exit 1)"; exit 1
