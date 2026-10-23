#!/usr/bin/env bash
# Service-specific health check for the `ingress` container (milestone H-03 follow-up).
#
# WHY THIS EXISTS: the runtime image's own HEALTHCHECK probes the HUB's loopback HTTP API on
# $HG_PORT (8080). This container runs hg_device_ingress on 8443 and nothing else, so it inherited
# a probe that could never pass and reported "unhealthy" while serving mutual TLS correctly.
# Overriding it in docker-compose is what makes the probe service-specific; the hub keeps the
# image-level one.
#
# WHAT IT CHECKS: that the ingress endpoint is up AND that its server certificate chains to the
# configured private CA and covers the expected name/address, via a real TLS handshake
# (hg_ingress_probe). It is deliberately NOT a raw TCP port check - a port check passes against any
# process holding the port, TLS or not, and would report healthy for exactly the faults a health
# check exists to catch.
#
# WHAT IT DOES NOT CHECK: device authentication. This container holds no device private key by
# design, so the probe presents no client certificate and changes no replay state; the handshake is
# expected to end at the server's client-certificate demand, which the probe reports as
# mutual_tls_demanded=yes. Device authentication is proven by the device-transport acceptance suite
# with a real device certificate.
#
# Exit codes: 0 healthy, 1 unhealthy (Docker records the status without restarting; see the
# restart policy notes), 2 usage.
set -uo pipefail

PROBE="${HG_INGRESS_PROBE:-/usr/local/bin/hg_ingress_probe}"
CERTS="${HG_INGRESS_CERTS:-/etc/homeguardian/certs}"
# Same rule as the CA: the config is the authority, the environment can override, and 8443 is only
# the last resort. A hardcoded 8443 passed in production by coincidence and failed the test.
PORT="${HG_INGRESS_HEALTH_PORT:-}"
if [ -z "$PORT" ] && [ -r "${HG_INGRESS_CONFIG:-${CERTS}/ingress.json}" ]; then
    PORT="$(sed -n 's/.*"port"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
            "${HG_INGRESS_CONFIG:-${CERTS}/ingress.json}" | tail -1)"
fi
PORT="${PORT:-8443}"
TIMEOUT_S="${HG_INGRESS_HEALTH_TIMEOUT:-4}"

# The name that must be covered by the server certificate's SAN. Defaults to the loopback name
# every provisioning profile issues; a deployment whose SAN differs sets HG_INGRESS_HEALTH_HOST (or
# HG_INGRESS_HEALTH_IP) to a name it really carries. There is NO option to skip the check.
VERIFY_NAME="${HG_INGRESS_HEALTH_HOST:-localhost}"
VERIFY_IP="${HG_INGRESS_HEALTH_IP:-}"

# The CA path is NOT guessed. The ingress config names it (ca_path) and is the authority for this
# deployment; the two layouts provision_ca.sh can produce are fallbacks. A live run caught the
# original hardcoded `${CERTS}/ca.crt` default: the real layout is `${CERTS}/ca/ca.crt`, so the
# container reported "CA certificate not readable" and stayed unhealthy while serving TLS correctly.
CONFIG="${HG_INGRESS_CONFIG:-${CERTS}/ingress.json}"
if [ -n "${HG_INGRESS_HEALTH_CA:-}" ]; then
    CA="$HG_INGRESS_HEALTH_CA"
elif [ -r "$CONFIG" ]; then
    CA="$(sed -n 's/.*"ca_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG" | tail -1)"
fi
if [ -z "${CA:-}" ]; then
    for candidate in "${CERTS}/ca.crt" "${CERTS}/ca/ca.crt"; do
        if [ -r "$candidate" ]; then CA="$candidate"; break; fi
    done
fi

[ -x "$PROBE" ] || { echo "ingress-healthcheck: probe not executable: $PROBE" >&2; exit 1; }
[ -n "${CA:-}" ] && [ -r "$CA" ] || {
    echo "ingress-healthcheck: CA certificate not readable - tried HG_INGRESS_HEALTH_CA, ca_path in ${CONFIG}, ${CERTS}/ca.crt, ${CERTS}/ca/ca.crt" >&2
    exit 1
}

echo "ingress-healthcheck: ca=$CA config=$CONFIG" >&2
args=(--ca "$CA" --host 127.0.0.1 --port "$PORT" --timeout-ms "$((TIMEOUT_S * 1000))")
if [ -n "$VERIFY_IP" ]; then
    args+=(--verify-ip "$VERIFY_IP")
else
    args+=(--verify-name "$VERIFY_NAME")
fi

"$PROBE" "${args[@]}"
exit $?
