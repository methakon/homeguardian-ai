#!/usr/bin/env bash
# provision_ca.sh - certificate lifecycle for the D-03 device transport.
#
# TWO DIRECTORIES, ON PURPOSE
#
#   --ca-dir (OFFLINE)      the CA private signing key, the CA certificate and the
#                           issuance serial. This is the authority: it mints
#                           certificates. It is never mounted into a container and
#                           never committed, and the key belongs on removable media
#                           once provisioning is done.
#
#   --runtime-dir (DEPLOYED) only what a running container must be able to READ:
#                           the PUBLIC CA certificate, the server certificate and
#                           key, the device registry, the revocation list and the
#                           ingress config. Docker mounts THIS directory, and it
#                           must never contain signing material - `check-runtime`
#                           asserts that and the ingress entrypoint refuses to
#                           start if it is ever violated.
#
# That separation is the point: compromising the ingress container must not hand an
# attacker the ability to mint certificates for new devices.
#
# FILE MODES: files the container reads are chowned to the container uid (10001)
# with mode 0640 - readable by the process that needs them and NOT world-readable.
# Directories stay operable by the operator so re-provisioning needs no root. If the
# chown cannot be performed (not root, no matching group) the script says exactly
# what to run; it never silently falls back to a world-readable key.
#
# Commands:
#   init    [--server-names "DNS:...,IP:..."]   create the CA + server certificate
#   issue   <device-id> [...]                   issue device certificates
#   server  [--server-names "..."]              (re)issue the server certificate
#   revoke  <device-id> [...]                   add devices to the revocation list
#   show    <device-id> [...]                   print a device's credential id
#   check-runtime                               READ-ONLY audit of the runtime dir
#
# Options (also via environment):
#   --ca-dir DIR        default $HG_CA_DIR      or ~/.homeguardian/ca
#   --runtime-dir DIR   default $HG_CERTS_DIR   or <repo>/deploy/docker/certs
#   --device-dir DIR    default $HG_DEVICE_DIR  or ~/.homeguardian/devices
#                       (device PRIVATE keys; never mounted anywhere)
#   --server-names LIST default "DNS:localhost,IP:127.0.0.1" (or $HG_SERVER_NAMES)
#   --force             allow replacing existing material (rotation)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CA_DIR="${HG_CA_DIR:-$HOME/.homeguardian/ca}"
RUNTIME_DIR="${HG_CERTS_DIR:-$REPO_ROOT/deploy/docker/certs}"
# DEVICE private keys are the phone's material. They are written OUTSIDE the runtime
# directory by default, because that directory is what Docker mounts: a device key
# inside it would be handed to the ingress container, which has no use for it.
DEVICE_DIR="${HG_DEVICE_DIR:-$HOME/.homeguardian/devices}"
SERVER_NAMES="${HG_SERVER_NAMES:-DNS:localhost,IP:127.0.0.1}"
DAYS="${HG_CERT_DAYS:-825}"
CONTAINER_UID="${HG_CONTAINER_UID:-10001}"
FORCE=0

ACTION=""
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ca-dir)       CA_DIR="$2"; shift 2 ;;
    --runtime-dir)  RUNTIME_DIR="$2"; shift 2 ;;
    --device-dir)   DEVICE_DIR="$2"; shift 2 ;;
    --server-names) SERVER_NAMES="$2"; shift 2 ;;
    --force)        FORCE=1; shift ;;
    -*)             echo "unknown option: $1" >&2; exit 2 ;;
    *)              if [[ -z "$ACTION" ]]; then ACTION="$1"; else ARGS+=("$1"); fi; shift ;;
  esac
done

die() { echo "provision_ca: $*" >&2; exit 1; }
note() { echo "  $*"; }

fingerprint() {
  openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^.*=//' | tr -d ':' | tr 'A-Z' 'a-z'
}

# Hand a file to the container uid: readable by that identity, by nothing else.
publish_file() {
  local target="$1"
  [[ -e "$target" ]] || return 0
  chmod 0640 "$target"
  if ! chown "$CONTAINER_UID:$CONTAINER_UID" "$target" 2>/dev/null; then
    note "NOTE: $target is 0640 but still owned by $(stat -c '%u:%g' "$target")."
    note "      The container runs as uid $CONTAINER_UID and will NOT be able to read it."
    note "      Fix with: sudo chown $CONTAINER_UID:$CONTAINER_UID $target"
    note "      Do NOT chmod 0644: a world-readable private key is not a workaround."
  fi
}

publish_dir() { chmod 0755 "$1"; }

require_ca() {
  [[ -s "$CA_DIR/ca.key" && -s "$CA_DIR/ca.crt" ]] \
    || die "no CA in $CA_DIR - run: $0 init"
}

# A runtime directory must never carry signing authority.
assert_runtime_has_no_ca_key() {
  local candidate
  for candidate in "$RUNTIME_DIR/ca/ca.key" "$RUNTIME_DIR/ca.key"; do
    if [[ -e "$candidate" ]]; then
      die "REFUSING: the runtime directory contains a CA private key ($candidate). \
Signing material belongs in the offline directory ($CA_DIR); a compromised container \
must not be able to mint certificates for new devices."
    fi
  done
  if [[ -d "$RUNTIME_DIR/ca" ]]; then
    for candidate in "$RUNTIME_DIR"/ca/*.key; do
      if [[ -e "$candidate" ]]; then
        die "REFUSING: $candidate is signing material inside the runtime directory."
      fi
    done
  fi
  return 0
}

issue_server_certificate() {
  mkdir -p "$RUNTIME_DIR/ca"
  # The container uid must be able to TRAVERSE into ca/ to read the public
  # certificate; the directory mode, not the file mode, is what allows that.
  chmod 0755 "$RUNTIME_DIR/ca"
  umask 077
  openssl genrsa -out "$RUNTIME_DIR/server.key" 2048 2>/dev/null
  openssl req -new -key "$RUNTIME_DIR/server.key" -out "$RUNTIME_DIR/server.csr" \
    -subj "/CN=homeguardian-hub"
  {
    echo "basicConstraints=CA:FALSE"
    echo "keyUsage=critical,digitalSignature,keyEncipherment"
    echo "extendedKeyUsage=serverAuth"
    # NONE is the deliberate "no SAN at all" case: it exists so the test suite can
    # prove that a certificate carrying ONLY a Common Name is refused. Never deploy it.
    if [[ "$SERVER_NAMES" != "NONE" ]]; then
      echo "subjectAltName=$SERVER_NAMES"
    fi
  } > "$RUNTIME_DIR/server.ext"
  openssl x509 -req -in "$RUNTIME_DIR/server.csr" -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" \
    -CAcreateserial -out "$RUNTIME_DIR/server.crt" -days "$DAYS" -sha256 \
    -extfile "$RUNTIME_DIR/server.ext" 2>/dev/null
  rm -f "$RUNTIME_DIR/server.csr" "$RUNTIME_DIR/server.ext"
  chmod 0600 "$RUNTIME_DIR/server.key"
  publish_file "$RUNTIME_DIR/ca/ca.crt"
  publish_file "$RUNTIME_DIR/server.crt"
  publish_file "$RUNTIME_DIR/server.key"
}

case "$ACTION" in
  init)
    if [[ -s "$CA_DIR/ca.key" && "$FORCE" != "1" ]]; then
      die "a CA already exists in $CA_DIR (use --force to replace it, or 'server' to re-issue just the server certificate)"
    fi
    mkdir -p "$CA_DIR" "$RUNTIME_DIR"
    publish_dir "$CA_DIR"; publish_dir "$RUNTIME_DIR"
    if [[ "$FORCE" == "1" ]]; then rm -f "$CA_DIR/ca.key" "$CA_DIR/ca.crt" "$CA_DIR/ca.srl"; fi

    umask 077
    if [[ ! -s "$CA_DIR/ca.key" ]]; then
      echo "==> generating the CA signing key (4096-bit, $DAYS days) - OFFLINE material"
      openssl genrsa -out "$CA_DIR/ca.key" 4096 2>/dev/null
      openssl req -x509 -new -nodes -key "$CA_DIR/ca.key" -sha256 -days "$DAYS" \
        -out "$CA_DIR/ca.crt" -subj "/CN=HomeGuardian D-03 Device CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
    fi
    chmod 0600 "$CA_DIR/ca.key"

    echo "==> publishing the PUBLIC CA certificate into the runtime directory"
    mkdir -p "$RUNTIME_DIR/ca"
    cp "$CA_DIR/ca.crt" "$RUNTIME_DIR/ca/ca.crt"

    echo "==> issuing the hub server certificate (SAN: $SERVER_NAMES)"
    issue_server_certificate
    assert_runtime_has_no_ca_key

    [[ -s "$RUNTIME_DIR/revocations.json" ]] || printf '[]\n' > "$RUNTIME_DIR/revocations.json"
    publish_file "$RUNTIME_DIR/revocations.json"
    if [[ ! -s "$RUNTIME_DIR/ingress.json" ]]; then
      cp "$REPO_ROOT/deploy/docker/config/ingress.json.example" "$RUNTIME_DIR/ingress.json"
    fi
    publish_file "$RUNTIME_DIR/ingress.json"
    if [[ -s "$RUNTIME_DIR/devices.json" ]]; then publish_file "$RUNTIME_DIR/devices.json"; fi

    echo
    echo "OFFLINE - never mounted, keep the key on removable media:"
    echo "  $CA_DIR/ca.key    signing key, mode 0600: the ONLY way to mint a device"
    echo "  $CA_DIR/ca.crt    CA certificate"
    echo "DEPLOYED - this is the directory Docker mounts read-only:"
    echo "  $RUNTIME_DIR"
    echo "    ca/ca.crt               public CA certificate (the trust anchor)"
    echo "    server.crt, server.key  hub identity; key mode 0640, uid $CONTAINER_UID"
    echo "    devices.json, revocations.json, ingress.json"
    echo
    echo "The server certificate's identity - this is what a client verifies, and a"
    echo "Common Name that is not in this list does NOT authorize anything:"
    openssl x509 -in "$RUNTIME_DIR/server.crt" -noout -ext subjectAltName | sed 's/^/  /'
    echo
    echo "NEXT: add the address the phones will actually dial, e.g."
    echo "  $0 server --server-names \"DNS:homeguardian.local,IP:192.168.1.10,IP:127.0.0.1\""
    ;;

  server)
    require_ca
    mkdir -p "$RUNTIME_DIR/ca"
    [[ -s "$RUNTIME_DIR/ca/ca.crt" ]] || cp "$CA_DIR/ca.crt" "$RUNTIME_DIR/ca/ca.crt"
    echo "==> (re)issuing the hub server certificate (SAN: $SERVER_NAMES)"
    issue_server_certificate
    assert_runtime_has_no_ca_key
    openssl x509 -in "$RUNTIME_DIR/server.crt" -noout -ext subjectAltName | sed 's/^/  /'
    note "restart the ingress so it serves the new certificate"
    ;;

  issue)
    [[ ${#ARGS[@]} -gt 0 ]] || die "usage: $0 issue <device-id> [<device-id>...]"
    require_ca
    # Certificates (public) go into the runtime directory; the phone's private key
    # goes into DEVICE_DIR, which is never mounted.
    mkdir -p "$RUNTIME_DIR/devices" "$RUNTIME_DIR/ca" "$DEVICE_DIR"
    [[ -s "$RUNTIME_DIR/ca/ca.crt" ]] || cp "$CA_DIR/ca.crt" "$RUNTIME_DIR/ca/ca.crt"
    publish_dir "$RUNTIME_DIR/devices"
    chmod 0700 "$DEVICE_DIR"
    for id in "${ARGS[@]}"; do
      case "$id" in
        *[!A-Za-z0-9._-]*) die "device id must be [A-Za-z0-9._-] only (got '$id')" ;;
      esac
      if [[ -s "$DEVICE_DIR/$id.key" && "$FORCE" != "1" ]]; then
        die "a certificate already exists for $id (--force replaces it and invalidates the old one)"
      fi
      umask 077
      openssl genrsa -out "$DEVICE_DIR/$id.key" 2048 2>/dev/null
      openssl req -new -key "$DEVICE_DIR/$id.key" -out "$RUNTIME_DIR/devices/$id.csr" \
        -subj "/CN=$id"
      {
        echo "basicConstraints=CA:FALSE"
        echo "keyUsage=critical,digitalSignature"
        echo "extendedKeyUsage=clientAuth"
      } > "$RUNTIME_DIR/devices/$id.ext"
      openssl x509 -req -in "$RUNTIME_DIR/devices/$id.csr" -CA "$CA_DIR/ca.crt" \
        -CAkey "$CA_DIR/ca.key" -CAcreateserial -out "$RUNTIME_DIR/devices/$id.crt" \
        -days "$DAYS" -sha256 -extfile "$RUNTIME_DIR/devices/$id.ext" 2>/dev/null
      rm -f "$RUNTIME_DIR/devices/$id.csr" "$RUNTIME_DIR/devices/$id.ext"
      # The device key stays in DEVICE_DIR (0700, never mounted); only the PUBLIC
      # certificate is published for the hub to check fingerprints against.
      chmod 0600 "$DEVICE_DIR/$id.key"
      chmod 0644 "$RUNTIME_DIR/devices/$id.crt"

      cred="$(fingerprint "$RUNTIME_DIR/devices/$id.crt")"
      echo "==> issued $id"
      note "credential_id : $cred"
      note "certificate   : $RUNTIME_DIR/devices/$id.crt"
      note "private key   : $DEVICE_DIR/$id.key (0600, OUTSIDE the mounted directory - export it to"
      note "                the phone, then delete it; the hub never needs a device key)"
      echo
      echo "    Registry entry for $RUNTIME_DIR/devices.json:"
      echo "      {\"device_id\":\"$id\",\"display_name\":\"$id\",\"device_type\":\"android\","
      echo "       \"credential_id\":\"$cred\",\"paired\":true,\"revoked\":false,"
      echo "       \"capabilities\":[\"camera\",\"microphone\"]}"
      echo "    Consent and capture stay OFF until the operator writes"
      echo "    \"consent_granted\":true and \"capture_enabled\":true for that device."
    done
    if [[ -s "$RUNTIME_DIR/devices.json" ]]; then publish_file "$RUNTIME_DIR/devices.json"; fi
    assert_runtime_has_no_ca_key
    ;;

  revoke)
    [[ ${#ARGS[@]} -gt 0 ]] || die "usage: $0 revoke <device-id> [<device-id>...]"
    REVOCATIONS="$RUNTIME_DIR/revocations.json"
    mkdir -p "$RUNTIME_DIR"
    [[ -s "$REVOCATIONS" ]] || printf '[]\n' > "$REVOCATIONS"
    python3 - "$REVOCATIONS" "${ARGS[@]}" <<'PY'
import json, sys
path, ids = sys.argv[1], sys.argv[2:]
try:
    with open(path) as handle:
        entries = json.load(handle)
    if not isinstance(entries, list):
        entries = []
except Exception:
    entries = []
for device_id in ids:
    if not any((e == device_id) or (isinstance(e, dict) and e.get("device_id") == device_id)
               for e in entries):
        entries.append({"device_id": device_id, "reason": "revoked by operator"})
with open(path, "w") as handle:
    json.dump(entries, handle, indent=2)
print("revocation list now holds %d device(s)" % len(entries))
PY
    publish_file "$REVOCATIONS"
    echo "==> revoked: ${ARGS[*]}"
    echo "    The ingress re-reads $REVOCATIONS on every observation, so the next"
    echo "    message from that device is refused with device_revoked - no restart."
    echo "    Removing an id lifts the ban but does NOT restore pairing or consent:"
    echo "    the operator states those again."
    ;;

  show)
    [[ ${#ARGS[@]} -gt 0 ]] || die "usage: $0 show <device-id> [<device-id>...]"
    for id in "${ARGS[@]}"; do
      [[ -s "$RUNTIME_DIR/devices/$id.crt" ]] || die "no certificate for $id"
      echo "device_id     : $id"
      echo "credential_id : $(fingerprint "$RUNTIME_DIR/devices/$id.crt")"
      echo "private key   : $DEVICE_DIR/$id.key"
    done
    ;;

  check-runtime)
    # READ-ONLY: safe to run against a live deployment.
    assert_runtime_has_no_ca_key
    problems=""
    [[ -s "$RUNTIME_DIR/ca/ca.crt" ]] || problems="$problems ca/ca.crt-missing"
    [[ -s "$RUNTIME_DIR/server.crt" ]] || problems="$problems server.crt-missing"
    [[ -s "$RUNTIME_DIR/server.key" ]] || problems="$problems server.key-missing"
    [[ -n "$problems" ]] && die "runtime directory is incomplete:$problems"
    for secret in "$RUNTIME_DIR/server.key" "$RUNTIME_DIR/ca/ca.crt"; do
      local_mode="$(stat -c '%a' "$secret")"
      case "$local_mode" in
        *[4567]) die "$secret is world-readable (mode $local_mode); 0640 at most" ;;
      esac
      owner="$(stat -c '%u' "$secret")"
      if [[ "$owner" != "$CONTAINER_UID" ]]; then
        echo "WARNING: $secret is owned by uid $owner, not $CONTAINER_UID." >&2
        echo "         The ingress container will not be able to read it." >&2
        echo "         Fix with: sudo chown $CONTAINER_UID:$CONTAINER_UID $secret" >&2
      fi
    done
    for key in "$RUNTIME_DIR"/devices/*.key; do
      if [[ -e "$key" ]]; then
        die "REFUSING: $key is a DEVICE private key inside the mounted runtime directory. \
Device keys belong in the device directory (--device-dir, default $DEVICE_DIR), which is \
never mounted; the hub checks fingerprints, it does not need keys."
      fi
    done
    echo "runtime directory is key-free and complete: $RUNTIME_DIR"
    openssl x509 -in "$RUNTIME_DIR/server.crt" -noout -ext subjectAltName | sed 's/^/  /'
    ;;

  *)
    sed -n '2,30p' "$0"
    exit 2
    ;;
esac
