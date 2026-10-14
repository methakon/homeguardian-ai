#!/usr/bin/env bash
# D-03 device ingestion entrypoint.
#
# Fail-closed, and deliberately dumb: it checks that the three things the ingress
# cannot work without are present and readable, then hands over. It does NOT parse
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

[ -r "$HG_INGRESS_CONFIG" ] || die "ingress config not readable: $HG_INGRESS_CONFIG (mount it read-only)"
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
