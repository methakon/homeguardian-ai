#!/usr/bin/env python3
"""HG-LAN-MTLS-07 - dev-only validation matrix for the device mTLS contract.

Mirrors the production rules (docs/DISTRIBUTED_DEVICE_TRANSPORT_D03.md, src/net/TlsTransport.cpp):

  * TLS 1.2 minimum, verification never disabled
  * server requires a client certificate (VERIFY_PEER | FAIL_IF_NO_PEER_CERT)
  * server identity is checked against SANs ONLY - a CN is never substituted
  * a device credential is bound to the certificate fingerprint (credential_id)
  * every failure must be FAIL-CLOSED: no fallback, no plaintext

HARNESS NOTE (a defect this file had and no longer has): an earlier revision returned
"success" as soon as the client's handshake completed and one send() returned. Under TLS 1.3
the client completes its handshake BEFORE the server has validated the client certificate, so a
server-side rejection is invisible to a client that never reads a response. That version reported
a FALSE PASS on three security-critical cases. Every case here now requires an application-level
ACK from the server, and every client-side negative is exercised on BOTH TLS 1.2 and TLS 1.3.

Stdlib only. Run:  python3 test_mtls_validation.py [fixture-dir]
Exit code 0 only if every case behaves as required.
"""
import json
import os
import socket
import ssl
import subprocess
import sys
import threading

FX = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hg-mtls-07-fixtures"
CA, R, D = f"{FX}/dev-ca", f"{FX}/runtime", f"{FX}/devices"


def fp(path):
    """SHA-256 fingerprint of a certificate, lowercase hex - the registry's credential_id."""
    out = subprocess.run(["openssl", "x509", "-in", path, "-noout", "-fingerprint", "-sha256"],
                         capture_output=True, text=True).stdout
    return out.split("=")[-1].strip().replace(":", "").lower()


def serve_once(server_crt, server_key, client_ca, max_version, ready, outcome):
    """One-shot TLS server requiring a verified client certificate; ACKs only on success."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if max_version:
        ctx.maximum_version = max_version
    ctx.load_cert_chain(server_crt, server_key)
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(client_ca)
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(1)
    ready.append(s.getsockname()[1])
    try:
        conn, _ = s.accept()
        with ctx.wrap_socket(conn, server_side=True) as tls:
            outcome.append(("accepted", tls.version()))
            tls.recv(16)
            tls.sendall(b"OK")          # the ACK the client now requires
    except Exception as exc:              # noqa: BLE001
        outcome.append(("rejected", f"{type(exc).__name__}"))
    finally:
        s.close()


def attempt(server_crt, server_key, client_crt=None, client_key=None,
            client_ca=f"{CA}/ca.crt", server_hostname="127.0.0.1", max_version=None):
    """Client half. Returns (connected: bool, detail). Requires an application ACK."""
    ready, outcome = [], []
    t = threading.Thread(target=serve_once,
                         args=(server_crt, server_key, client_ca, max_version, ready, outcome), daemon=True)
    t.start()
    for _ in range(200):
        if ready:
            break
        threading.Event().wait(0.02)
    if not ready:
        return False, "harness: server never bound"
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if max_version:
        ctx.maximum_version = max_version
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True              # SAN-only: never falls back to CN
    ctx.load_verify_locations(f"{CA}/ca.crt")
    if client_crt:
        ctx.load_cert_chain(client_crt, client_key)
    try:
        with socket.create_connection(("127.0.0.1", ready[0]), timeout=5) as sock:
            with ctx.wrap_socket(sock, server_hostname=server_hostname) as tls:
                tls.sendall(b"x")
                ack = tls.recv(2)          # THE GATE: a rejected client cert surfaces here
                if ack != b"OK":
                    return False, f"no application ACK (got {ack!r}) - rejected after handshake"
                return True, f"handshake+ACK OK ({tls.version()})"
    except Exception as exc:               # noqa: BLE001
        return False, f"{type(exc).__name__}: {exc}"
    finally:
        t.join(timeout=5)


def registry(path):
    return {d["id"]: d for d in json.load(open(path))["devices"]}


def bound(reg_path, device_id, cert_path):
    """Credential binding: the presented cert's fingerprint must equal the registry entry."""
    dev = registry(reg_path).get(device_id)
    if not dev or dev.get("revoked"):
        return False, "device_revoked_or_absent"
    if dev["credential_id"] != fp(cert_path):
        return False, "credential_id_mismatch"
    return True, "bound"


T12, T13 = ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3
GOOD_SRV, GOOD_KEY = f"{R}/server-good-SAN.crt", f"{R}/server-good-SAN.key"
EL, ELK = f"{D}/eluga-i7-01.crt", f"{D}/eluga-i7-01.key"

CASES = [
    ("valid device (eluga) -> hub [TLS1.2]", True,
     lambda: attempt(GOOD_SRV, GOOD_KEY, EL, ELK, max_version=T12)),
    ("valid device (eluga) -> hub [TLS1.3]", True,
     lambda: attempt(GOOD_SRV, GOOD_KEY, EL, ELK, max_version=T13)),
    ("NEG no client certificate [TLS1.2]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, None, None, max_version=T12)),
    ("NEG no client certificate [TLS1.3]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, None, None, max_version=T13)),
    ("NEG client cert from UNTRUSTED CA [TLS1.2]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, f"{D}/foreign-01.crt", f"{D}/foreign-01.key", max_version=T12)),
    ("NEG client cert from UNTRUSTED CA [TLS1.3]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, f"{D}/foreign-01.crt", f"{D}/foreign-01.key", max_version=T13)),
    ("NEG EXPIRED client certificate [TLS1.2]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, f"{D}/expired-01.crt", f"{D}/expired-01.key", max_version=T12)),
    ("NEG EXPIRED client certificate [TLS1.3]", False,
     lambda: attempt(GOOD_SRV, GOOD_KEY, f"{D}/expired-01.crt", f"{D}/expired-01.key", max_version=T13)),
    ("NEG server cert with NO SAN (CN must not be substituted) [TLS1.2]", False,
     lambda: attempt(f"{R}/server-no-SAN.crt", f"{R}/server-no-SAN.key", EL, ELK, max_version=T12)),
    ("NEG server cert with NO SAN (CN must not be substituted) [TLS1.3]", False,
     lambda: attempt(f"{R}/server-no-SAN.crt", f"{R}/server-no-SAN.key", EL, ELK, max_version=T13)),
    ("NEG server cert with WRONG SAN [TLS1.2]", False,
     lambda: attempt(f"{R}/server-wrong-SAN.crt", f"{R}/server-wrong-SAN.key", EL, ELK, max_version=T12)),
    ("NEG server cert with WRONG SAN [TLS1.3]", False,
     lambda: attempt(f"{R}/server-wrong-SAN.crt", f"{R}/server-wrong-SAN.key", EL, ELK, max_version=T13)),
    ("NEG registry says the device is REVOKED", False,
     lambda: bound(f"{R}/registry-eluga-revoked.json", "eluga-i7-01", EL)),
    ("NEG credential_id does not match the presented cert", False,
     lambda: bound(f"{R}/registry-mismatch.json", "eluga-i7-01", EL)),
]


def main():
    print(f"dev-only fixtures: {FX}\n")
    print(f"{'case':66} {'required':9} {'observed':9} result")
    print("-" * 104)
    failures = []
    for name, expect_ok, fn in CASES:
        try:
            ok, detail = fn()
        except Exception as exc:           # noqa: BLE001
            ok, detail = False, f"harness error {type(exc).__name__}"
        good = (ok == expect_ok)
        if not good:
            failures.append(f"{name}: expected {'ALLOW' if expect_ok else 'REJECT'}, got {detail}")
        print(f"{name:66} {'ALLOW' if expect_ok else 'REJECT':9} "
              f"{'ALLOW' if ok else 'REJECT':9} {'ok' if good else 'VIOLATION'}  ({detail[:36]})")
    print("-" * 104)
    if failures:
        print(f"FAIL: {len(failures)} case(s) violated the contract")
        for f in failures:
            print("  -", f)
        return 1
    print(f"PASS: all {len(CASES)} cases match the contract (fail-closed verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
