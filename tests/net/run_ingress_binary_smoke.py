#!/usr/bin/env python3
"""Deployable-binary smoke: the shipped hg_device_ingress over real loopback mTLS.

Fresh synthetic credentials are generated under hg_build for every run. Nothing
outside the build directory is read or written, and no capture/network device is
touched: this only proves the freshly built ingress binary accepts authenticated
observation frames end to end.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import threading

root = Path(__file__).resolve().parents[2]
build = root / "hg_build"
ingress = build / "hg_device_ingress"
client = build / "hg_device_client"
device_id = "hg-tls-fixture"

if not ingress.exists() or not client.exists():
    sys.exit("build hg_device_ingress and hg_device_client first")

def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]

with tempfile.TemporaryDirectory(prefix="ingress-binary-", dir=build) as directory:
    d = Path(directory)
    def openssl(*args):
        return subprocess.check_output(["openssl", *args], cwd=d, stderr=subprocess.DEVNULL)
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
            "-out", "ca.pem", "-subj", "/CN=HG synthetic smoke CA", "-days", "1")
    for name, cn, usage in [("server", "localhost", "serverAuth"),
                            ("client", device_id, "clientAuth")]:
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key",
                "-out", name + ".csr", "-subj", "/CN=" + cn)
        (d / "ext").write_text("extendedKeyUsage=" + usage +
                               "\nsubjectAltName=IP:127.0.0.1,DNS:localhost\n")
        openssl("x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-CAcreateserial", "-out", name + ".pem", "-days", "1", "-extfile", "ext")
        (d / (name + ".key")).chmod(0o600)
    fingerprint = hashlib.sha256(openssl("x509", "-in", "client.pem", "-outform", "DER")).hexdigest()
    (d / "registry.json").write_text(json.dumps({"devices": [{
        "device_id": device_id, "display_name": "synthetic smoke fixture",
        "device_type": "hub-fixture", "credential_id": fingerprint,
        "paired": True, "revoked": False, "consent_granted": True,
        "consent_subject": device_id, "consent_purpose": "security_monitor",
        "capture_enabled": True, "capabilities": ["motion"], "is_synthetic": True,
    }]}))
    port = free_port()
    (d / "ingress.json").write_text(json.dumps({
        "certificate_path": str(d / "server.pem"),
        "private_key_path": str(d / "server.key"),
        "ca_path": str(d / "ca.pem"),
        "bind_address": "127.0.0.1", "port": port,
        "registry_path": str(d / "registry.json"),
        "audit_db_path": str(d / "audit.db"),
        "idle_timeout_ms": 5000, "authorization_refresh_ms": 50,
    }))
    server = subprocess.Popen([str(ingress), "--config", str(d / "ingress.json")],
                              cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert server.stdout is not None
    lines = []
    def drain():
        assert server.stdout is not None
        for line in server.stdout:
            lines.append(line)
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        deadline = time.time() + 10
        listening = False
        while time.time() < deadline:
            if server.poll() is not None:
                sys.exit("ingress exited early:\n" + "".join(lines))
            # The service announces its bound port only after the listener is up.
            if any(line.startswith("hg_device_ingress ready:") for line in lines):
                listening = True
                break
            time.sleep(0.05)
        if not listening:
            sys.exit("ingress never started listening on its configured port")
        result = subprocess.run([str(client), "--ca", str(d / "ca.pem"),
                                 "--cert", str(d / "client.pem"), "--key", str(d / "client.key"),
                                 "--device-id", device_id, "--port", str(port), "--count", "3"],
                                cwd=root, capture_output=True, text=True, timeout=30)
        print(result.stdout.strip())
        print(result.stderr.strip())
        if result.returncode != 0:
            sys.exit("client failed with exit code %d" % result.returncode)
    finally:
        server.terminate()
        reader.join(timeout=10)
        if reader.is_alive():
            server.kill()
            sys.exit("ingress did not shut down on SIGTERM")
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            sys.exit("ingress did not exit after SIGTERM")
        output = "".join(lines)
        print(output.strip())
        if server.returncode != 0:
            sys.exit("ingress exited with %s" % server.returncode)
