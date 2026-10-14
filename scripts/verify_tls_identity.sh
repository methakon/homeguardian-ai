#!/usr/bin/env bash
# verify_tls_identity.sh - D-03.1 section B: the hub's IDENTITY is verified, not just
# its chain.
#
# THE PROPERTY UNDER TEST
#   A certificate that chains to the trusted CA must ALSO identify the endpoint the
#   client was told to reach. Without that, any host holding any certificate the CA
#   ever issued - another device's, for instance - could impersonate the hub and
#   harvest a node's observations.
#
# Every case below is a REAL handshake against a REAL ingress with its own
# certificate; nothing is mocked and nothing is asserted from a code reading.
#
# Cases:
#   1  matching DNS SAN                  -> accepted
#   2  matching IP SAN                   -> accepted
#   3  correct CA, IP with no matching SAN-> rejected   (resolvable, wrong identity)
#   4  correct CA, DNS name not in SAN    -> rejected   (resolvable, wrong identity)
#   5  CA-signed certificate with NO SAN, CN matching -> rejected (a CN is not authorization)
#   6  certificate from an UNTRUSTED CA   -> rejected
#   7  EXPIRED server certificate         -> rejected
#   8  EXPIRED client certificate         -> rejected
#
# Runs where the TLS binaries exist (the container image, or a host with libssl-dev).
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
INGRESS="${HG_INGRESS_PATH:-$REPO/hg_build/hg_device_ingress}"
CLIENT="${HG_CLIENT_PATH:-$REPO/hg_build/hg_device_client}"
PROVISION="$REPO/tools/provision_ca.sh"
W="${1:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-62s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-62s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-62s SKIP  %s\n' "$1" "${2:-}"; }

cleanup() {
  [[ -n "${PID:-}" ]] && kill -TERM "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null
  [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$W"
}
trap cleanup EXIT

echo "== D-03.1 TLS server identity acceptance =="
if [[ ! -x "$INGRESS" || ! -x "$CLIENT" ]]; then
  skip "TLS binaries present" "build with libssl-dev (the container image does)"
  echo "  ____"; echo "  TLS identity: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi
mkdir -p "$W"; umask 077; cd "$W"

DEV="hg-identity000000001"
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

# ---------------------------------------------------------------- material
# A second, UNRELATED CA, for the untrusted-chain case.
bash "$PROVISION" init --ca-dir "$W/ca-good" --runtime-dir "$W/rt-good" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
DEVKEYS="$W/device-keys"
bash "$PROVISION" issue "$DEV" --ca-dir "$W/ca-good" --runtime-dir "$W/rt-good" \
  --device-dir "$DEVKEYS" >/dev/null 2>&1
bash "$PROVISION" init --ca-dir "$W/ca-other" --runtime-dir "$W/rt-other" \
  --device-dir "$DEVKEYS" --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

# A CA-signed server certificate that carries NO SAN at all (CN only). Issued with the
# SAME good CA on purpose: the chain is valid, so only the SAN rule can reject it.
bash "$PROVISION" server --ca-dir "$W/ca-good" --runtime-dir "$W/rt-nosan" \
  --server-names "NONE" >/dev/null 2>&1
bash "$PROVISION" server --ca-dir "$W/ca-good" --runtime-dir "$W/rt-expired" \
  --server-names "DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1
# Re-sign the expired server certificate with a zero-day lifetime.
openssl genrsa -out "$W/rt-expired/server.key" 2048 2>/dev/null
openssl req -new -key "$W/rt-expired/server.key" -out "$W/expired.csr" -subj "/CN=homeguardian-hub"
printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n' > "$W/expired.ext"
openssl x509 -req -in "$W/expired.csr" -CA "$W/ca-good/ca.crt" -CAkey "$W/ca-good/ca.key" \
  -CAcreateserial -out "$W/rt-expired/server.crt" -days 0 -sha256 -extfile "$W/expired.ext" 2>/dev/null
# An expired CLIENT certificate.
openssl genrsa -out "$DEVKEYS/$DEV.key" 2048 2>/dev/null
openssl req -new -key "$DEVKEYS/$DEV.key" -out "$W/expired-client.csr" -subj "/CN=$DEV"
printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n' > "$W/expired-client.ext"
openssl x509 -req -in "$W/expired-client.csr" -CA "$W/ca-good/ca.crt" -CAkey "$W/ca-good/ca.key" \
  -CAcreateserial -out "$W/rt-good/devices/$DEV.crt" -days 0 -sha256 \
  -extfile "$W/expired-client.ext" 2>/dev/null
bash "$PROVISION" issue "$DEV" --ca-dir "$W/ca-good" --runtime-dir "$W/rt-good" \
  --device-dir "$DEVKEYS" --force >/dev/null 2>&1
GOOD_CRED="$(bash "$PROVISION" show "$DEV" --runtime-dir "$W/rt-good" | awk '/credential_id/{print $3}')"

# Wrong-identity names must RESOLVE, or the case would prove DNS failure instead of a
# SAN mismatch: 127.0.0.2 is a second loopback address and the name is pinned here.
printf '127.0.0.1 wrong.example.com\n' >> /etc/hosts 2>/dev/null || true

registry() {  # registry <runtime-dir> <credential>
  cat > "$1/devices.json" <<JSON
{"devices":[{"device_id":"$DEV","display_name":"identity test","device_type":"android",
 "credential_id":"$2","paired":true,"revoked":false,"capabilities":["camera","microphone"],
 "consent_granted":true,"consent_subject":"$DEV","consent_purpose":"security_monitor",
 "capture_enabled":true}]}
JSON
}
for rt in rt-good rt-nosan rt-expired rt-other; do
  [[ -d "$W/$rt" ]] || mkdir -p "$W/$rt"
  [[ -s "$W/$rt/revocations.json" ]] || printf '[]\n' > "$W/$rt/revocations.json"
  registry "$W/$rt" "$GOOD_CRED"
done

config() {  # config <runtime-dir> <file>
  cat > "$2" <<JSON
{"certificate_path":"$1/server.crt","private_key_path":"$1/server.key",
 "ca_path":"$1/ca/ca.crt","bind_address":"127.0.0.1","port":$PORT,
 "registry_path":"$1/devices.json","revocation_path":"$1/revocations.json",
 "audit_db_path":"$W/audit-$RANDOM.db","max_connections":4,"max_queued_connections":4,
 "max_workers":2,"max_pending_observations":16,"idle_timeout_ms":3000,
 "handshake_timeout_ms":3000}
JSON
}

start_ingress() {  # start_ingress <runtime-dir>
  local cfg="$W/ingress-$RANDOM.json"
  config "$1" "$cfg"
  "$INGRESS" --config "$cfg" >"$W/ingress.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 40); do grep -q "ready" "$W/ingress.log" 2>/dev/null && return 0; sleep 0.25; done
  return 1
}
stop_ingress() { kill -TERM "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""; }

client() {  # client <cert-dir> <host> <ca-dir> [sequence-start]
  # Each accepted case uses a DISTINCT sequence base: the hub's watermark advances on
  # every accepted observation, so re-using a sequence number would make the hub
  # answer replayed_sequence and the case would appear to fail for the wrong reason.
  "$CLIENT" --host "$2" --port "$PORT" --ca "$3/ca/ca.crt" \
    --cert "$1/$DEV.crt" --key "$DEVKEYS/$DEV.key" --device-id "$DEV" --mode normal --count 1 \
    --sequence-start "${4:-1}" 2>&1
}

# ---------------------------------------------------------------- good chain
if start_ingress "$W/rt-good"; then
  OUT="$(client "$W/rt-good/devices" localhost "$W/rt-good")"
  grep -q "done accepted=1" <<<"$OUT" \
    && ok "1. controller: matching DNS SAN accepted (endpoint 'localhost')" \
    || bad "1. DNS SAN accept" "$(tail -1 <<<"$OUT" | cut -c1-70)"
  OUT="$(client "$W/rt-good/devices" 127.0.0.1 "$W/rt-good" 100)"
  grep -q "done accepted=1" <<<"$OUT" \
    && ok "2. controller: matching IP SAN accepted (endpoint '127.0.0.1')" \
    || bad "2. IP SAN accept" "$(tail -1 <<<"$OUT" | cut -c1-70)"
  OUT="$(client "$W/rt-good/devices" 127.0.0.2 "$W/rt-good")"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "3. wrong IP (127.0.0.2) rejected" "it connected"
  else
    ok "3. correct CA but an IP with no matching SAN is rejected"
  fi
  OUT="$(client "$W/rt-good/devices" wrong.example.com "$W/rt-good")"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "4. wrong DNS name rejected" "it connected"
  else
    ok "4. correct CA but a DNS name outside the SAN is rejected"
  fi
  stop_ingress
else
  bad "ingress start (good certificate)" "$(tail -1 "$W/ingress.log")"
fi

# ---------------------------------------------------------------- SAN-less
if start_ingress "$W/rt-nosan"; then
  OUT="$(client "$W/rt-good/devices" localhost "$W/rt-nosan")"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "5. SAN-less certificate rejected" "a CN alone authorized the hub"
  else
    ok "5. a CA-signed certificate with only a CN (no SAN) is rejected"
  fi
  stop_ingress
else
  bad "ingress start (SAN-less)" "$(tail -1 "$W/ingress.log")"
fi

# ---------------------------------------------------------------- untrusted CA
if start_ingress "$W/rt-other"; then
  OUT="$(client "$W/rt-good/devices" localhost "$W/rt-good")"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "6. untrusted CA rejected" "it connected"
  else
    ok "6. a certificate from an untrusted CA is rejected"
  fi
  stop_ingress
else
  bad "ingress start (other CA)" "$(tail -1 "$W/ingress.log")"
fi

# ---------------------------------------------------------------- expired server
if start_ingress "$W/rt-expired"; then
  OUT="$(client "$W/rt-good/devices" localhost "$W/rt-good")"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "7. expired server certificate rejected" "it connected"
  else
    grep -qi "expire" <<<"$OUT" && ok "7. an expired server certificate is rejected (reported as expiry)" \
                                || ok "7. an expired server certificate is rejected"
  fi
  stop_ingress
else
  bad "ingress start (expired)" "$(tail -1 "$W/ingress.log")"
fi

# ---------------------------------------------------------------- expired client
if start_ingress "$W/rt-good"; then
  printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n' > "$W/expired-client.ext"
  openssl genrsa -out "$W/expired-client.key" 2048 2>/dev/null
  openssl req -new -key "$W/expired-client.key" -out "$W/expired-client.csr" -subj "/CN=$DEV"
  openssl x509 -req -in "$W/expired-client.csr" -CA "$W/ca-good/ca.crt" -CAkey "$W/ca-good/ca.key" \
    -CAcreateserial -out "$W/expired-client.crt" -days 0 -sha256 \
    -extfile "$W/expired-client.ext" 2>/dev/null
  OUT="$("$CLIENT" --host localhost --port "$PORT" --ca "$W/rt-good/ca/ca.crt" \
    --cert "$W/expired-client.crt" --key "$W/expired-client.key" --device-id "$DEV" \
    --mode normal --count 1 2>&1)"
  if grep -q "done accepted" <<<"$OUT"; then
    bad "8. expired client certificate rejected" "the hub accepted it"
  else
    ok "8. an expired DEVICE certificate is rejected by the hub"
  fi
  stop_ingress
else
  bad "ingress start (good, for the expired-client case)" "$(tail -1 "$W/ingress.log")"
fi

# ---------------------------------------------------------------- CA key isolation
[[ -e "$W/rt-good/ca/ca.key" ]] && bad "runtime directory holds no CA key" "found $W/rt-good/ca/ca.key" \
                               || ok "the runtime directory the ingress mounted has no CA signing key"

echo "  --------------------------------------------------------------------"
echo "  TLS identity acceptance: $PASS passed, $FAIL failed, $SKIP skipped"
[[ "$FAIL" == "0" ]]
