#!/usr/bin/env python3
"""HG-LAN-MTLS-07 - revocation and identity-isolation tests.

These probe the SECURITY PROPERTY rather than a helper function:

  A. revocation is live - the receiver re-reads its registry PER CONNECTION, so revoking a device
     takes effect on the next connection with no restart, and lifting the revocation also takes
     effect (a revocation that cannot be lifted is itself a defect).
  B. identity isolation - identity comes from the certificate and is bound to its fingerprint:
     a trusted-CA certificate for an unenrolled device is refused, and a certificate carrying a
     KNOWN device's CN but a different key is refused (a CN is never sufficient).

The mock receiver mirrors the shipped model: TLS requires and verifies the client certificate
against the private CA, then the application layer matches the presented certificate's SHA-256
fingerprint against a registry entry that is paired and not revoked - reloading the registry file
on every connection.

Stdlib only. Run: python3 test_revocation_and_identity.py [fixture-dir]
Exit code 0 only if every case behaves as required.
"""
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import threading

FX = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/.hermes/cache/scratch/hg-mtls-07-fixtures")
CA, R, D = f"{FX}/dev-ca", f"{FX}/runtime", f"{FX}/devices"
LIVE_REG = f"{R}/live-registry.json"


def fp(path):
    out = subprocess.run(["openssl", "x509", "-in", path, "-noout", "-fingerprint", "-sha256"],
                         capture_output=True, text=True).stdout
    return out.split("=")[-1].strip().replace(":", "").lower()


def fingerprint_of_der(der):
    """SHA-256 of a DER certificate - what the receiver computes from the presented peer cert."""
    import hashlib
    return hashlib.sha256(der).hexdigest()


class Receiver(threading.Thread):
    """Per-connection registry re-read; identity bound to the certificate fingerprint."""

    def __init__(self, crt, key, registry_path):
        super().__init__(daemon=True)
        self.registry_path, self.registry = registry_path, registry_path
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        self.ctx.load_cert_chain(crt, key)
        self.ctx.verify_mode = ssl.CERT_REQUIRED
        self.ctx.load_verify_locations(f"{CA}/ca.crt")
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", 0))
        self.s.listen(8)
        self.s.settimeout(0.3)
        self.port = self.s.getsockname()[1]
        self.stop = threading.Event()
        self.log: list[str] = []

    def _authorize(self, der):
        """Re-read the registry for EVERY connection - no caching across connections."""
        try:
            reg = json.load(open(self.registry_path))
        except Exception:                                   # noqa: BLE001
            return False, "registry_unreadable"
        presented = fingerprint_of_der(der)
        for dev in reg.get("devices", []):
            if dev.get("credential_id") == presented:
                if dev.get("revoked"):
                    return False, f"device_revoked:{dev['id']}"
                if not dev.get("paired"):
                    return False, f"device_unpaired:{dev['id']}"
                return True, f"accepted:{dev['id']}"
        return False, "device_unknown"      # canonical (was credential_not_enrolled)

    def run(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.s.accept()
            except (socket.timeout, OSError):
                continue
            try:
                with self.ctx.wrap_socket(conn, server_side=True) as tls:
                    der = tls.getpeercert(binary_form=True)
                    ok, why = self._authorize(der)
                    self.log.append(why)
                    if not ok:
                        continue                                # refuse THIS connection, keep serving
                    tls.recv(16)
                    tls.sendall(b"OK")
            except Exception as exc:                            # noqa: BLE001
                self.log.append(f"tls_refused:{type(exc).__name__}")

    def close(self):
        self.stop.set()
        self.s.close()


def connect(port, cert=None, key=None):
    """Returns (authorized, detail). Authorization requires the application ACK."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True
    ctx.load_verify_locations(f"{CA}/ca.crt")
    if cert:
        ctx.load_cert_chain(cert, key)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=6) as sock:
            with ctx.wrap_socket(sock, server_hostname="127.0.0.1") as tls:
                tls.sendall(b"obs")
                ack = tls.recv(2)
                return (ack == b"OK"), ("accepted" if ack == b"OK" else f"no ACK ({ack!r})")
    except Exception as exc:                                    # noqa: BLE001
        return False, f"{type(exc).__name__}"


def write_registry(devices):
    json.dump({"dev_only": True, "devices": devices}, open(LIVE_REG, "w"), indent=2)


def entry(cert_path, device_id, revoked=False, paired=True):
    return {"id": device_id, "credential_id": fp(cert_path), "paired": paired, "revoked": revoked}


def main():
    el_crt, el_key = f"{D}/eluga-i7-01.crt", f"{D}/eluga-i7-01.key"
    # seed the live registry from the generated fixtures
    base = json.load(open(f"{R}/registry-trusted.json"))["devices"]
    write_registry(base)

    srv_crt, srv_key = f"{R}/server-good-SAN.crt", f"{R}/server-good-SAN.key"
    rx = Receiver(srv_crt, srv_key, LIVE_REG)
    rx.start()

    results, required = [], []

    def case(name, expect_ok, cert, key, note=""):
        ok, detail = connect(rx.port, cert, key)
        results.append((name, expect_ok, ok, detail, note))
        required.append(expect_ok == ok)

    # --- A. live revocation, no restart ---------------------------------------------------
    case("A1 enrolled device, registry not revoked", True, el_crt, el_key)
    write_registry([entry(el_crt, "eluga-i7-01", revoked=True)] + [d for d in base if d["id"] != "eluga-i7-01"])
    case("A2 SAME receiver, after revoking in the file (no restart)", False, el_crt, el_key,
         "proves the registry is re-read per connection")
    write_registry(base)
    case("A3 SAME receiver, revocation lifted", True, el_crt, el_key,
         "revocation must be liftable, not permanent")
    write_registry([entry(el_crt, "eluga-i7-01", paired=False)] + [d for d in base if d["id"] != "eluga-i7-01"])
    case("A4 SAME receiver, device unpaired", False, el_crt, el_key)
    write_registry(base)

    # --- B. identity isolation ------------------------------------------------------------
    case("B1 trusted CA, device ABSENT from the registry", False,
         f"{D}/unscanned-01.crt", f"{D}/unscanned-01.key")
    case("B2 certificate with a KNOWN device CN but a different key", False,
         f"{D}/eluga-dup.crt", f"{D}/eluga-dup.key",
         "CN must not be sufficient - the binding is the fingerprint")
    case("B3 another enrolled device cannot impersonate: own cert still works", True,
         f"{D}/mi-a3-01.crt", f"{D}/mi-a3-01.key")
    case("B4 untrusted CA is refused at the TLS layer", False,
         f"{D}/foreign-01.crt", f"{D}/foreign-01.key")

    rx.close()

    # Reason-level assertions: the shipped contract requires a STABLE refusal reason, so a verdict
    # alone is not sufficient evidence. The receiver never reads the CN, so a certificate carrying a
    # known device's CN but an unknown key must be refused as not-enrolled, not merely "rejected".
    reasons = rx.log
    reason_expect = [
        ("revocation recorded with its reason", reasons.count("device_revoked:eluga-i7-01") == 1),
        ("unpaired recorded with its reason", reasons.count("device_unpaired:eluga-i7-01") == 1),
        ("not-enrolled reached exactly twice (absent device, and CN duplicate)",
         reasons.count("device_unknown") == 2),
        ("enrolled device accepted twice (before revoke, after lift)",
         reasons.count("accepted:eluga-i7-01") == 2),
        ("the other device accepted once", reasons.count("accepted:mi-a3-01") == 1),
    ]
    print()
    for label, ok in reason_expect:
        print(f"  {'ok' if ok else 'VIOLATION'}  {label}")
        required.append(ok)

    print(f"\nfixtures: {FX}\n")
    print(f"{'case':62} {'required':8} {'observed':8} result  detail")
    print("-" * 108)
    for name, expect_ok, ok, detail, note in results:
        tag = "ok" if expect_ok == ok else "VIOLATION"
        print(f"{name:62} {'ALLOW' if expect_ok else 'REJECT':8} "
              f"{'ALLOW' if ok else 'REJECT':8} {tag:9} {detail}")
        if note:
            print(f"{'':62} {'':8} {'':8} {note}")
    print("-" * 108)
    print("receiver-side decisions observed:", rx.log)
    bad = required.count(False)
    if bad:
        print(f"FAIL: {bad} case(s) violated the contract")
        return 1
    print(f"PASS: all {len(results)} cases match the contract "
          f"(live revocation and identity isolation verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
