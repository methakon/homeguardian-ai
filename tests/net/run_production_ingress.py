#!/usr/bin/env python3
"""Generate fresh synthetic credentials under hg_build; run real loopback mTLS tests."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
build = root / "hg_build"
with tempfile.TemporaryDirectory(prefix="production-ingress-", dir=build) as directory:
    d = Path(directory)
    def openssl(*args):
        return subprocess.check_output(["openssl", *args], cwd=d, stderr=subprocess.DEVNULL)
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
            "-out", "ca.pem", "-subj", "/CN=HG synthetic test CA", "-days", "1")
    for name, cn, extension in [("server", "localhost", "serverAuth"),
                                ("client", "hg-tls-fixture", "clientAuth")]:
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key",
                "-out", name + ".csr", "-subj", "/CN=" + cn)
        (d / "extensions").write_text("extendedKeyUsage=" + extension +
                                       "\nsubjectAltName=IP:127.0.0.1,DNS:localhost\n")
        openssl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-CAcreateserial", "-out", name + ".pem", "-days", "1", "-extfile", "extensions")
        (d / (name + ".key")).chmod(0o600)
    der = openssl("x509", "-in", "client.pem", "-outform", "DER")
    (d / "fingerprint").write_text(hashlib.sha256(der).hexdigest())
    env = dict(os.environ, HG_INGRESS_TEST_DIR=str(d))
    result = subprocess.run([str(build / "homeguardian_tests"),
                             * (sys.argv[1:] or ["[production-ingress]"])], cwd=root, env=env)
    sys.exit(result.returncode)
