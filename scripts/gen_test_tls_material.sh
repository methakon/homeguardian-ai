#!/usr/bin/env bash
# gen_test_tls_material.sh — generate TEST-ONLY TLS material for the Android handshake E2E test.
#
# WHAT THIS IS
#   A throwaway CA, a server certificate whose SAN matches 127.0.0.1 (which the transport's HTTPS
#   endpoint identification requires), and a client certificate. All passwords are the literal string
#   "testonly" and every file is written under a temp directory.
#
# WHAT THIS IS NOT
#   Not a production credential. Not a device identity. Not enrolled anywhere. It cannot authenticate
#   to the real hub because the real hub does not trust this CA, and the material is regenerated on
#   every run. Nothing here is committed: the output directory is temporary and is printed, not stored.
#
# WHY A REAL TLS PEER IS WORTH THE TROUBLE
#   The extracted decision functions (welcomeVerdict, resumeWatermark) can be tested in isolation, but
#   only a socket shows that the production transport actually CALLS them, loads the keystores, frames
#   correctly on a stream, and refuses a peer it does not trust. So the material is generated here and
#   the test is skipped loudly - never silently passed - when it is absent.
#
# USAGE
#   scripts/gen_test_tls_material.sh [output-dir]      # prints the directory on stdout
set -euo pipefail

OUT="${1:-${TMPDIR:-/tmp}/hg_test_tls_material}"
export JAVA_HOME="${JAVA_HOME:-$HOME/android-jdk}"
export PATH="$JAVA_HOME/bin:$PATH"

PW="testonly"

command -v keytool >/dev/null 2>&1 || { echo "keytool not found (set JAVA_HOME)" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"

# 1. a throwaway CA
keytool -genkeypair -alias testca -keyalg RSA -keysize 2048 -validity 2 \
    -dname "CN=HomeGuardian TEST-ONLY CA, O=Test, C=US" \
    -ext bc:c -keystore ca.p12 -storetype PKCS12 -storepass "$PW" -keypass "$PW" >/dev/null 2>&1

# 2. server cert. The SAN is load-bearing: openSocket() sets
#    endpointIdentificationAlgorithm=HTTPS and enforceServerIdentity() then requires a SAN matching
#    config.host, which is 127.0.0.1 here.
keytool -genkeypair -alias server -keyalg RSA -keysize 2048 -validity 2 \
    -dname "CN=127.0.0.1, O=Test, C=US" -ext "SAN=IP:127.0.0.1,DNS:localhost" \
    -keystore server.p12 -storetype PKCS12 -storepass "$PW" -keypass "$PW" >/dev/null 2>&1
keytool -certreq -alias server -keystore server.p12 -storetype PKCS12 -storepass "$PW" \
    -file server.csr >/dev/null 2>&1
keytool -gencert -alias testca -keystore ca.p12 -storetype PKCS12 -storepass "$PW" \
    -infile server.csr -outfile server.crt -validity 2 \
    -ext "SAN=IP:127.0.0.1,DNS:localhost" >/dev/null 2>&1
keytool -exportcert -alias testca -keystore ca.p12 -storetype PKCS12 -storepass "$PW" \
    -rfc -file ca.crt >/dev/null 2>&1
keytool -importcert -alias testca -file ca.crt -keystore server.p12 -storetype PKCS12 \
    -storepass "$PW" -noprompt >/dev/null 2>&1
keytool -importcert -alias server -file server.crt -keystore server.p12 -storetype PKCS12 \
    -storepass "$PW" -noprompt >/dev/null 2>&1

# 3. client cert, signed by the same CA
keytool -genkeypair -alias client -keyalg RSA -keysize 2048 -validity 2 \
    -dname "CN=hg-android-test, O=Test, C=US" \
    -keystore client.p12 -storetype PKCS12 -storepass "$PW" -keypass "$PW" >/dev/null 2>&1
keytool -certreq -alias client -keystore client.p12 -storetype PKCS12 -storepass "$PW" \
    -file client.csr >/dev/null 2>&1
keytool -gencert -alias testca -keystore ca.p12 -storetype PKCS12 -storepass "$PW" \
    -infile client.csr -outfile client.crt -validity 2 >/dev/null 2>&1
keytool -importcert -alias testca -file ca.crt -keystore client.p12 -storetype PKCS12 \
    -storepass "$PW" -noprompt >/dev/null 2>&1
keytool -importcert -alias client -file client.crt -keystore client.p12 -storetype PKCS12 \
    -storepass "$PW" -noprompt >/dev/null 2>&1

# 4. the client's truststore: the test CA ONLY
keytool -importcert -alias testca -file ca.crt -keystore trust.p12 -storetype PKCS12 \
    -storepass "$PW" -noprompt >/dev/null 2>&1

# 5. an EMPTY truststore, for the untrusted-peer negative control
keytool -genkeypair -alias throwaway -keyalg RSA -keysize 2048 -validity 2 \
    -dname "CN=throwaway, O=Test, C=US" -keystore trust-empty.p12 -storetype PKCS12 \
    -storepass "$PW" -keypass "$PW" >/dev/null 2>&1
keytool -delete -alias throwaway -keystore trust-empty.p12 -storetype PKCS12 \
    -storepass "$PW" >/dev/null 2>&1

# 6. passwords in separate files, as Config expects (0600, never logged)
printf '%s' "$PW" > client.pw
printf '%s' "$PW" > trust.pw
chmod 600 client.pw trust.pw
rm -f server.csr client.csr

# Sanity: the material must actually be usable, or the test would skip for the wrong reason.
for f in client.p12 trust.p12 trust-empty.p12 server.p12 client.pw trust.pw; do
    [ -s "$f" ] || { echo "material generation failed: $f missing or empty" >&2; exit 3; }
done
keytool -list -keystore trust.p12 -storetype PKCS12 -storepass "$PW" 2>/dev/null \
    | grep -q testca || { echo "truststore does not contain the test CA" >&2; exit 3; }

echo "$OUT"
