#!/usr/bin/env bash
# HG-LAN-MTLS-07 - DEV-ONLY PKI fixtures. NOT production identities.
#
# Mirrors the production model documented in docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md
# and implemented in tools/provision_ca.sh:
#   * private CA, 4096-bit, basicConstraints=critical,CA:TRUE, keyUsage=critical,keyCertSign,cRLSign
#   * hub/server cert: extendedKeyUsage=serverAuth, SANs only (CN fallback is refused)
#   * one cert per device: Common Name IS the device id, extendedKeyUsage=clientAuth
#   * private keys 0600 inside a 0700 tree
#
# It additionally builds the NEGATIVE fixtures the validation matrix needs.
set -euo pipefail
OUT="${1:-/tmp/hg-mtls-07-fixtures}"
rm -rf "$OUT"; mkdir -p "$OUT"; chmod 700 "$OUT"
CA="$OUT/dev-ca"; R="$OUT/runtime"; D="$OUT/devices"; OTHER="$OUT/other-ca"
mkdir -p "$CA" "$R" "$D" "$OTHER"; chmod 700 "$CA" "$D" "$OTHER"
DAYS=30
SAN_GOOD="IP:127.0.0.1,DNS:hub.local"
SAN_WRONG="IP:127.0.0.2,DNS:not-the-hub.local"

gen_ca() { # $1=dir $2=CN
  openssl genrsa -out "$1/ca.key" 4096 2>/dev/null
  openssl req -x509 -new -nodes -key "$1/ca.key" -sha256 -days 3650 -out "$1/ca.crt" \
    -subj "/CN=$2" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
  chmod 600 "$1/ca.key"
}
gen_ca "$CA"    "HG-LAN-MTLS-07 DEV CA"
gen_ca "$OTHER" "HG-LAN-MTLS-07 UNTRUSTED CA"

sign_server() { # $1=name $2=san ("" = deliberately no SAN) $3=ca-dir
  local n=$1 san=$2 ca=$3
  openssl genrsa -out "$R/$n.key" 2048 2>/dev/null
  openssl req -new -key "$R/$n.key" -out "$R/$n.csr" -subj "/CN=homeguardian-hub"
  {
    echo "basicConstraints=CA:FALSE"
    echo "keyUsage=critical,digitalSignature,keyEncipherment"
    echo "extendedKeyUsage=serverAuth"
    [ -n "$san" ] && echo "subjectAltName=$san"
  } > "$R/$n.ext"
  openssl x509 -req -in "$R/$n.csr" -CA "$ca/ca.crt" -CAkey "$ca/ca.key" -CAcreateserial \
    -out "$R/$n.crt" -days "$DAYS" -sha256 -extfile "$R/$n.ext" 2>/dev/null
  chmod 600 "$R/$n.key"
}
sign_server server-good-SAN  "$SAN_GOOD"  "$CA"
sign_server server-wrong-SAN "$SAN_WRONG" "$CA"
sign_server server-no-SAN    ""           "$CA"

sign_client() { # $1=file-id $2=ca-dir $3=CN (defaults to $1)
  local id=$1 ca=$2 cn=${3:-$1}
  openssl genrsa -out "$D/$id.key" 2048 2>/dev/null
  openssl req -new -key "$D/$id.key" -out "$R/$id.csr" -subj "/CN=$cn"
  {
    echo "basicConstraints=CA:FALSE"
    echo "keyUsage=critical,digitalSignature"
    echo "extendedKeyUsage=clientAuth"
  } > "$R/$id.ext"
  openssl x509 -req -in "$R/$id.csr" -CA "$ca/ca.crt" -CAkey "$ca/ca.key" -CAcreateserial \
    -out "$D/$id.crt" -days "$DAYS" -sha256 -extfile "$R/$id.ext" 2>/dev/null
  chmod 600 "$D/$id.key"
}
sign_client eluga-i7-01 "$CA"      # the initiating device
sign_client mi-a3-01    "$CA"
sign_client foreign-01  "$OTHER"   # NEGATIVE: signed by an untrusted CA
sign_client unscanned-01 "$CA"     # NEGATIVE: trusted CA, but absent from the registry
sign_client eluga-dup   "$CA" eluga-i7-01   # NEGATIVE: CN=eluga-i7-01, DIFFERENT key/fingerprint

# NEGATIVE: an EXPIRED client certificate, minted with explicit past validity dates.
mkdir -p "$CA/db/newcerts"; : > "$CA/db/index.txt"; echo 1000 > "$CA/db/serial"
cat > "$CA/ca.cnf" <<EOF
[ ca ]
default_ca = DEV
[ DEV ]
dir             = $CA/db
database        = \$dir/index.txt
new_certs_dir   = \$dir/newcerts
serial          = \$dir/serial
certificate     = $CA/ca.crt
private_key     = $CA/ca.key
default_md      = sha256
policy          = pol
x509_extensions = cli
unique_subject  = no
[ pol ]
commonName = supplied
[ cli ]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
EOF
openssl genrsa -out "$D/expired-01.key" 2048 2>/dev/null
openssl req -new -key "$D/expired-01.key" -out "$R/expired-01.csr" -subj "/CN=expired-01"
openssl ca -config "$CA/ca.cnf" -batch -notext -in "$R/expired-01.csr" -out "$D/expired-01.crt" \
  -startdate 20200101000000Z -enddate 20200102000000Z 2>/dev/null || echo "WARN: expired fixture could not be minted"

# Registry: credential_id is the certificate SHA-256 fingerprint, exactly as the
# production registry binds a device credential.
python3 - "$OUT" <<'PY'
import json, os, subprocess, sys
out = sys.argv[1]
def fp(p):
    r = subprocess.run(["openssl","x509","-in",p,"-noout","-fingerprint","-sha256"],
                       capture_output=True, text=True).stdout
    return r.split("=")[-1].strip().replace(":","").lower()
def entry(did, revoked=False):
    return {"id": did, "credential_id": fp(os.path.join(out,"devices",did+".crt")),
            "paired": True, "revoked": revoked}
base = [entry("eluga-i7-01"), entry("mi-a3-01")]
json.dump({"dev_only": True, "devices": base},
          open(os.path.join(out,"runtime","registry-trusted.json"),"w"), indent=2)
json.dump({"dev_only": True, "devices": [entry("eluga-i7-01", revoked=True), entry("mi-a3-01")]},
          open(os.path.join(out,"runtime","registry-eluga-revoked.json"),"w"), indent=2)
# credential-binding mismatch: registry holds a fingerprint that is NOT the presented cert's
wrong = {"dev_only": True, "devices": [entry("eluga-i7-01"), entry("mi-a3-01")]}
wrong["devices"][0]["credential_id"] = "0"*64
json.dump(wrong, open(os.path.join(out,"runtime","registry-mismatch.json"),"w"), indent=2)
print("registry written; eluga credential_id =", base[0]["credential_id"][:16]+"…")
PY
echo "fixtures at $OUT"
find "$OUT" -name '*.crt' | sort | sed 's/^/  /'
