#!/usr/bin/env bash
# provision_ca.sh - create a private CA and issue per-device certificates for the
# D-03 device ingestion boundary.
#
# WHY A PRIVATE CA: the brief accepts a private CA with per-device certificates for
# a local installation, provided provisioning, identity checks, secure storage and
# revocation are real. This script makes the first three mechanical:
#
#   * one offline CA key, used ONLY to sign (never present on the hub's disk in
#     production - keep it on removable media),
#   * one server certificate for the hub,
#   * ONE CERTIFICATE PER DEVICE whose Common Name IS the device id, so the hub can
#     bind "the certificate chains to my CA" AND "the fingerprint is the one the
#     registry recorded for this device id" - two independent bindings, because
#     either alone would let one phone act as another.
#
# Storage: every private key is written 0600 inside a 0700 tree. Nothing is copied
# into the repository, and .gitignore excludes certs/.
#
# Revocation: certificates are short-lived by design (default 825 days) and the
# hub's registry/revocation file is the operational control - see
# docs/DISTRIBUTED_DEPLOYMENT.md. `revoke` here prints the exact registry edit and
# appends the id to a revocation list the ingress re-reads per connection.
#
# Usage:
#   bash tools/provision_ca.sh init [outdir]              # create the CA + server cert
#   bash tools/provision_ca.sh issue <device-id> [outdir] # issue a device certificate
#   bash tools/provision_ca.sh revoke <device-id> [outdir]# add to the revocation list
#   bash tools/provision_ca.sh show <device-id> [outdir]  # print the fingerprint to register
set -euo pipefail

# Argument mapping differs per action, and getting it wrong silently wrote the CA
# to the default directory while the caller thought it had chosen one:
#   init   [outdir]         -> $2 is the outdir
#   issue  <id> [outdir]    -> $3 is the outdir
#   revoke <id> [outdir]    -> $3 is the outdir
ACTION="${1:-}"
ID="${2:-}"
OUT="${3:-${HG_CERT_DIR:-$HOME/.homeguardian/certs}}"
DAYS="${HG_CERT_DAYS:-825}"
if [[ "$ACTION" == "init" && -n "${2:-}" ]]; then
  OUT="$2"
fi
REVOCATION_FILE="$OUT/revocations.json"

umask 077
mkdir -p "$OUT/ca"
chmod 700 "$OUT" "$OUT/ca"

fingerprint() {
  openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^.*=//' | tr -d ':' | tr 'A-Z' 'a-z'
}

require_ca() {
  [[ -f "$OUT/ca/ca.crt" && -f "$OUT/ca/ca.key" ]] || {
    echo "no CA at $OUT/ca - run: bash tools/provision_ca.sh init" >&2
    exit 2
  }
}

case "$ACTION" in
  init)
    require=""
    if [[ -f "$OUT/ca/ca.key" ]]; then
      read -r -p "CA already exists at $OUT/ca. Overwrite? [y/N] " answer
      [[ "$answer" == "y" || "$answer" == "Y" ]] || exit 1
    fi
    echo "==> generating the private CA (4096-bit, $DAYS days)"
    openssl genrsa -out "$OUT/ca/ca.key" 4096 2>/dev/null
    openssl req -x509 -new -nodes -key "$OUT/ca/ca.key" -sha256 -days "$DAYS" \
      -out "$OUT/ca/ca.crt" \
      -subj "/CN=HomeGuardian D-03 Device CA" \
      -addext "basicConstraints=critical,CA:TRUE" \
      -addext "keyUsage=critical,keyCertSign,cRLSign"

    echo "==> issuing the hub server certificate"
    openssl genrsa -out "$OUT/server.key" 2048 2>/dev/null
    openssl req -new -key "$OUT/server.key" -out "$OUT/server.csr" -subj "/CN=hg-hub"
    cat > "$OUT/server.ext" <<'EXT'
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:127.0.0.1,DNS:localhost
EXT
    openssl x509 -req -in "$OUT/server.csr" -CA "$OUT/ca/ca.crt" -CAkey "$OUT/ca/ca.key" \
      -CAcreateserial -out "$OUT/server.crt" -days "$DAYS" -sha256 -extfile "$OUT/server.ext" 2>/dev/null
    rm -f "$OUT/server.csr"
    chmod 600 "$OUT/server.key" "$OUT/ca/ca.key"

    echo "==> CA and server material written to $OUT (keys 0600, tree 0700)"
    echo "    CA certificate : $OUT/ca/ca.crt"
    echo "    server cert/key: $OUT/server.crt / $OUT/server.key"
    echo "    KEEP $OUT/ca/ca.key OFFLINE when not issuing certificates."
    echo "    Note: the hub verifies that a device certificate chains to this CA and"
    echo "    matches the fingerprint recorded in its registry. Hostname verification is"
    echo "    not used because the trust anchor is the private CA, not a public name."
    echo
    echo "    CONTAINER DEPLOYMENT: this tree is written 0700/0600 for host safety, but"
    echo "    the ingress runs as uid 10001 and must be able to TRAVERSE the mounted"
    echo "    directory and READ the files it is given. Before mounting it, either"
    echo "      chmod 755 \$OUT && chmod 644 \$OUT/*.crt \$OUT/ca/ca.crt \$OUT/server.key"
    echo "    or use a real secret store. Protection then comes from the host path (keep"
    echo "    the parent directory operator-only) and not from the file modes, because a"
    echo "    bind mount hands the container whatever mode the file has."
    ;;

  issue)
    [[ -n "$ID" ]] || { echo "usage: bash tools/provision_ca.sh issue <device-id> [outdir]" >&2; exit 2; }
    require_ca
    case "$ID" in
      *[!A-Za-z0-9._-]*) echo "device id must be [A-Za-z0-9._-] only (got '$ID')" >&2; exit 2 ;;
    esac
    mkdir -p "$OUT/devices"
    chmod 700 "$OUT/devices"
    echo "==> issuing a client certificate for $ID"
    openssl genrsa -out "$OUT/devices/$ID.key" 2048 2>/dev/null
    openssl req -new -key "$OUT/devices/$ID.key" -out "$OUT/devices/$ID.csr" -subj "/CN=$ID"
    cat > "$OUT/devices/$ID.ext" <<'EXT'
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EXT
    openssl x509 -req -in "$OUT/devices/$ID.csr" -CA "$OUT/ca/ca.crt" -CAkey "$OUT/ca/ca.key" \
      -CAcreateserial -out "$OUT/devices/$ID.crt" -days "$DAYS" -sha256 \
      -extfile "$OUT/devices/$ID.ext" 2>/dev/null
    rm -f "$OUT/devices/$ID.csr" "$OUT/devices/$ID.ext"
    chmod 600 "$OUT/devices/$ID.key"

    FP="$(fingerprint "$OUT/devices/$ID.crt")"
    echo "    device id      : $ID"
    echo "    certificate    : $OUT/devices/$ID.crt"
    echo "    key            : $OUT/devices/$ID.key (0600)"
    echo "    CREDENTIAL_ID  : $FP"
    echo
    echo "    Register the device by adding this entry to the hub's registry file:"
    echo "      {\"device_id\":\"$ID\",\"display_name\":\"$ID\",\"device_type\":\"android\","
    echo "       \"credential_id\":\"$FP\",\"paired\":true,\"revoked\":false,"
    echo "       \"capabilities\":[\"camera\",\"microphone\"]}"
    echo "    Then grant consent and enable capture: set \"consent_granted\":true and"
    echo "    \"capture_enabled\":true for that device (the ingress still refuses capture"
    echo "    until the operator enables it)."
    ;;

  revoke)
    [[ -n "$ID" ]] || { echo "usage: bash tools/provision_ca.sh revoke <device-id> [outdir]" >&2; exit 2; }
    mkdir -p "$(dirname "$REVOCATION_FILE")"
    if [[ ! -f "$REVOCATION_FILE" ]]; then
      printf '[]\n' > "$REVOCATION_FILE"
      chmod 600 "$REVOCATION_FILE"
    fi
    python3 - "$REVOCATION_FILE" "$ID" <<'PY'
import json, sys
path, device_id = sys.argv[1], sys.argv[2]
try:
    with open(path) as handle:
        entries = json.load(handle)
    if not isinstance(entries, list):
        entries = []
except Exception:
    entries = []
if not any((e == device_id) or (isinstance(e, dict) and e.get("device_id") == device_id)
           for e in entries):
    entries.append({"device_id": device_id, "reason": "revoked by operator"})
with open(path, "w") as handle:
    json.dump(entries, handle, indent=2)
print("revocation list now holds %d device(s)" % len(entries))
PY
    echo "==> $ID revoked in $REVOCATION_FILE"
    echo "    The ingress re-reads this file on every connection, so the next message"
    echo "    from that device is refused with 'device_revoked' - no restart needed."
    echo "    Optionally also set \"revoked\": true for the device in the registry file."
    ;;

  show)
    [[ -n "$ID" ]] || { echo "usage: bash tools/provision_ca.sh show <device-id> [outdir]" >&2; exit 2; }
    [[ -f "$OUT/devices/$ID.crt" ]] || { echo "no certificate for $ID" >&2; exit 2; }
    echo "device_id     : $ID"
    echo "credential_id : $(fingerprint "$OUT/devices/$ID.crt")"
    ;;

  *)
    sed -n '2,30p' "$0"
    exit 2
    ;;
esac
