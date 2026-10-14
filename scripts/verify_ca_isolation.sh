#!/usr/bin/env bash
# verify_ca_isolation.sh - D-03.1 section C: the CA signing key is not in the
# deployment.
#
# THE PROPERTY UNDER TEST
#   Compromising the ingress container must not hand an attacker the ability to mint
#   certificates for arbitrary devices. That holds only if the signing key is not in
#   the mounted directory, not in the image, and not in any running container - and
#   if the deployment REFUSES to start when someone puts it there by mistake.
#
# Runs on a host with bash + python3; the Docker-dependent cases skip cleanly.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PROVISION="$REPO/tools/provision_ca.sh"
ENTRYPOINT="$REPO/deploy/docker/ingress-entrypoint.sh"
IMAGE="${HG_IMAGE:-homeguardian-hub:local}"
W="${1:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); printf '  %-62s OK\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  %-62s FAIL  %s\n' "$1" "${2:-}"; }
skip() { SKIP=$((SKIP+1)); printf '  %-62s SKIP  %s\n' "$1" "${2:-}"; }

cleanup() { [[ "${HG_KEEP_WORK:-0}" == "1" ]] || rm -rf "$W"; }
trap cleanup EXIT

echo "== D-03.1 CA / private-key isolation =="
mkdir -p "$W"; umask 077
CA="$W/offline-ca"; RT="$W/runtime"; DEV="$W/device-keys"

# ---------------------------------------------------------------- provisioning
if ! bash "$PROVISION" init --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     --server-names "DNS:localhost,IP:127.0.0.1" >"$W/init.log" 2>&1; then
  bad "provisioning init" "$(tail -1 "$W/init.log")"; echo "  -----"; exit 1
fi
bash "$PROVISION" issue hg-iso000000000001 --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
  >"$W/issue.log" 2>&1

# 1. layout
[[ -s "$CA/ca.key" ]] && ok "1. the CA signing key exists in the OFFLINE directory" \
                      || bad "1. CA key missing" "$CA/ca.key"
[[ ! -e "$RT/ca/ca.key" && ! -e "$RT/ca.key" ]] \
  && ok "2. the runtime directory holds NO CA signing key" \
  || bad "2. CA key present in the runtime dir"
[[ -s "$RT/ca/ca.crt" ]] && ok "3. the runtime directory carries the PUBLIC CA certificate" \
                         || bad "3. public CA certificate missing"
[[ -s "$DEV/hg-iso000000000001.key" ]] \
  && ok "4. the DEVICE private key was written outside the runtime directory" \
  || bad "4. device key not in the device directory"
if compgen -G "$RT/devices/*.key" >/dev/null; then
  bad "5. no device private key inside the runtime directory"
else
  ok "5. no device private key inside the runtime directory"
fi
[[ -s "$RT/devices/hg-iso000000000001.crt" ]] \
  && ok "6. the device PUBLIC certificate IS published for fingerprint checks" \
  || bad "6. device certificate not published"

# 7. check-runtime accepts a correct deployment
if bash "$PROVISION" check-runtime --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     >"$W/check.log" 2>&1; then
  ok "7. check-runtime accepts a correct deployment"
else
  bad "7. check-runtime rejected a correct deployment" "$(tail -1 "$W/check.log")"
fi

# 8. a world-readable server key is refused
cp "$RT/server.key" "$W/server.key.backup"; chmod 0644 "$RT/server.key"
if bash "$PROVISION" check-runtime --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     >"$W/check2.log" 2>&1; then
  bad "8. a world-readable private key is refused" "check-runtime accepted mode 0644"
else
  ok "8. a world-readable private key is refused"
fi
cp "$W/server.key.backup" "$RT/server.key"; chmod 0640 "$RT/server.key"

# 9. idempotency: a second init must refuse rather than replace the CA
if bash "$PROVISION" init --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     >"$W/init2.log" 2>&1; then
  bad "9. a second init refuses to replace an existing CA"
else
  grep -qi "already exists" "$W/init2.log" && ok "9. a second init refuses to replace the existing CA" \
                                          || ok "9. a second init refused (reason not matched)"
fi
# and re-issuing an existing device certificate must refuse without --force
if bash "$PROVISION" issue hg-iso000000000001 --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     >>"$W/issue.log" 2>&1; then
  bad "10. re-issuing an existing device certificate refuses without --force"
else
  ok "10. re-issuing an existing device certificate refuses without --force"
fi

# ---------------------------------------------------------------- runtime guards
# 11. the ENTRYPOINT refuses to start when the mounted directory carries a CA key
mkdir -p "$RT/ca"
cp "$CA/ca.key" "$RT/ca/ca.key"
OUT="$(HG_INGRESS_CONFIG="$RT/ingress.json" HG_INGRESS_DATA="$W/data" HG_INGRESS_CERTS="$RT" \
       bash "$ENTRYPOINT" 2>&1)"
RC=$?
if [[ "$RC" != "0" ]] && grep -qi "CA PRIVATE KEY\|signing material" <<<"$OUT"; then
  ok "11. the entrypoint REFUSES to start with a CA key in the mounted directory"
else
  bad "11. entrypoint accepted a mounted CA key" "rc=$RC $(head -1 <<<"$OUT" | cut -c1-60)"
fi
rm -f "$RT/ca/ca.key"

# 12. provisioning refuses to work against a runtime dir that carries signing material
mkdir -p "$RT/ca"; cp "$CA/ca.key" "$RT/ca/ca.key"
if bash "$PROVISION" issue hg-iso000000000002 --ca-dir "$CA" --runtime-dir "$RT" --device-dir "$DEV" \
     >"$W/poison.log" 2>&1; then
  bad "12. provisioning refuses a runtime dir holding a CA key" "it proceeded"
else
  grep -qi "REFUSING" "$W/poison.log" && ok "12. provisioning refuses a runtime dir holding a CA key" \
                                      || ok "12. provisioning refused (reason not matched)"
fi
rm -f "$RT/ca/ca.key"

# 13. missing or invalid required credentials fail closed
mv "$RT/server.crt" "$W/server.crt.hold"
OUT="$(HG_INGRESS_CONFIG="$RT/ingress.json" HG_INGRESS_DATA="$W/data" HG_INGRESS_CERTS="$RT" \
       bash "$ENTRYPOINT" 2>&1)"
RC=$?
if [[ "$RC" != "0" ]]; then
  ok "13. a missing server certificate fails closed at the container entrypoint"
else
  bad "13. entrypoint started without a server certificate"
fi
mv "$W/server.crt.hold" "$RT/server.crt"

# ---------------------------------------------------------------- image checks
if docker image inspect "$IMAGE" >/dev/null 2>&1; then
  KEYS="$(docker run --rm --entrypoint /bin/sh "$IMAGE" -c \
    'find / -name "*.key" -not -path "/proc/*" -not -path "/sys/*" 2>/dev/null' 2>/dev/null)"
  if [[ -z "$KEYS" ]]; then
    ok "14. the runtime IMAGE contains no private key of any kind"
  else
    bad "14. private key material inside the image" "$(tr '\n' ' ' <<<"$KEYS" | cut -c1-80)"
  fi
  CA_ANY="$(docker run --rm --entrypoint /bin/sh "$IMAGE" -c \
    'find / -name "ca.key" -o -name "*.p12" 2>/dev/null | head -5' 2>/dev/null)"
  [[ -z "$CA_ANY" ]] && ok "15. the image contains no CA signing key and no PKCS#12" \
                     || bad "15. CA material in the image" "$CA_ANY"
else
  skip "14/15. runtime image checks" "image $IMAGE not built here"
fi

# 16. the build context excludes the credential paths (isolated fixtures)
if [[ -x "$REPO/scripts/verify_build_context_exclusions.sh" ]]; then
  if bash "$REPO/scripts/verify_build_context_exclusions.sh" >"$W/context.log" 2>&1; then
    ok "16. build-context exclusions hold, using isolated fixtures"
  else
    bad "16. build-context exclusion probe" "$(tail -1 "$W/context.log")"
  fi
else
  skip "16. build-context exclusion probe" "script not present"
fi

echo "  --------------------------------------------------------------------"
echo "  CA isolation: $PASS passed, $FAIL failed, $SKIP skipped"
[[ "$FAIL" == "0" ]]
