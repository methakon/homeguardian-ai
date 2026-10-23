#!/usr/bin/env bash
# D-03 device ingestion entrypoint.
#
# Fail-closed, and deliberately specific: it checks that every file the ingress needs is
# present and readable, refuses with the FULL list of what is missing plus the exact
# provisioning commands, and only then hands over. It does NOT parse
# the JSON config -- the binary does that, with the same fail-closed result and a
# better message. Keeping the preflight dependency-free (no interpreter in this
# image) is the point: a smaller runtime is a smaller attack surface.
#
# Certificate material is mounted READ-ONLY from the host; nothing sensitive is
# baked into an image layer, and the container never writes to the cert tree.
set -euo pipefail

HG_INGRESS_CONFIG="${HG_INGRESS_CONFIG:-/etc/homeguardian/ingress.json}"
HG_INGRESS_DATA="${HG_INGRESS_DATA:-/data/ingress}"
HG_INGRESS_CERTS="${HG_INGRESS_CERTS:-/etc/homeguardian/certs}"

die() { echo "homeguardian-ingress: $*" >&2; exit 78; }   # 78 = EX_CONFIG

# PROVISIONING PREFLIGHT. An unprovisioned mount must fail with a diagnostic the operator can act
# on, not with the name of whichever file the script happened to check first. Every required file is
# checked, ALL problems are reported together, and the provisioning commands are printed verbatim.
# Nothing is created, guessed or substituted here: this is a refusal (exit 78 = EX_CONFIG).
unreadable=""
for required in "$HG_INGRESS_CONFIG" \
                "$HG_INGRESS_CERTS/server.crt" \
                "$HG_INGRESS_CERTS/server.key" \
                "$HG_INGRESS_CERTS/ca/ca.crt" \
                "$HG_INGRESS_CERTS/devices.json"; do
  [ -r "$required" ] || unreadable="${unreadable}${unreadable:+, }${required}"
done
if [ -n "$unreadable" ]; then
  echo "homeguardian-ingress: REFUSING to start - the mounted credential directory is NOT provisioned." >&2
  echo "  mount          : HG_CERTS_DIR (host)  ->  ${HG_INGRESS_CERTS} (container, read-only)" >&2
  echo "  running as uid : $(id -u)   (every file above must be READABLE by this uid)" >&2
  echo "  missing or unreadable: ${unreadable}" >&2
  echo "  provision it with:" >&2
  echo "    bash tools/provision_ca.sh init  --ca-dir <offline-dir> --runtime-dir <this-dir> --device-dir <device-key-dir>" >&2
  echo "    bash tools/provision_ca.sh issue <device-id> --ca-dir <offline-dir> --runtime-dir <this-dir> --device-dir <device-key-dir>" >&2
  echo "    cp deploy/docker/config/ingress.json.example <this-dir>/ingress.json   # then set ca_path / certificate_path / private_key_path" >&2
  echo "    sudo chown 10001:10001 <this-dir>/*   # or stage dirs 0755 / files 0644 so uid $(id -u) can read them" >&2
  echo "  note: the directory must contain NO CA signing key (the ingress refuses it on purpose)." >&2
  echo "  see docs/DISTRIBUTED_DEPLOYMENT.md and docs/ORACLE_RELAY_H04_PLAN.md" >&2
  exit 78
fi
# The ingress now shares the hub's persistent volume so that an accepted
# observation's alert lands in the SAME alert history the dashboard serves. The
# directory is created here (rather than demanded) because a fresh named volume
# starts empty, and refusing to start on an empty volume would be a bug, not a
# safety property. Readability/writability are still required.
if [ ! -d "$HG_INGRESS_DATA" ]; then
  mkdir -p "$HG_INGRESS_DATA" 2>/dev/null || die "cannot create ingress data directory: $HG_INGRESS_DATA"
fi
[ -w "$HG_INGRESS_DATA" ]   || die "ingress data directory not writable: $HG_INGRESS_DATA"
[ -d "$HG_INGRESS_CERTS" ]  || die "certificate directory missing: $HG_INGRESS_CERTS"

# The mounted directory must carry NO signing authority. The CA private key belongs
# in the operator's offline directory; if it ever appears here, the container would
# be one compromise away from minting certificates for arbitrary devices, so this
# refuses to start rather than running with that exposure.
for candidate in "$HG_INGRESS_CERTS/ca/ca.key" "$HG_INGRESS_CERTS/ca.key"; do
  if [ -e "$candidate" ]; then
    die "REFUSING to start: $candidate is a CA PRIVATE KEY in the mounted credential directory. \
Move it to the offline provisioning directory (tools/provision_ca.sh --ca-dir) and mount only \
the deployable material. See docs/DISTRIBUTED_DEPLOYMENT.md."
  fi
done
if [ -d "$HG_INGRESS_CERTS/ca" ]; then
  for stray in "$HG_INGRESS_CERTS"/ca/*.key; do
    [ -e "$stray" ] || continue
    die "REFUSING to start: $stray is signing material inside the mounted credential directory."
  done
fi
[ -x /usr/local/bin/hg_device_ingress ] || die "the ingress binary is missing from the image"

# Warn (do not fail) on world/group-readable key material: the ingress itself
# refuses a permissive private key, so failing here would only obscure its reason.
for key in "$HG_INGRESS_CERTS/server.key" "$HG_INGRESS_CERTS/ca/ca.key"; do
  if [ -e "$key" ]; then
    perms="$(stat -c '%a' "$key" 2>/dev/null || echo '?')"
    case "$perms" in
      600|400) : ;;
      *) echo "homeguardian-ingress: WARNING: $key is mode $perms (expected 600)" >&2 ;;
    esac
  fi
done

echo "homeguardian-ingress: starting (config=${HG_INGRESS_CONFIG}, data=${HG_INGRESS_DATA})"
cd "$HG_INGRESS_DATA"
# exec form: the ingress is PID 1 and receives SIGTERM directly for a clean stop.
exec /usr/local/bin/hg_device_ingress --config "$HG_INGRESS_CONFIG"
