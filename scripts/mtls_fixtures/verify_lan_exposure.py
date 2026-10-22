#!/usr/bin/env python3
"""HG-LAN-MTLS-07 - executable network-exposure gate for the handset observation path.

Turns the written validation procedure into checks that PASS or FAIL, so the bind split
(receiver on the private LAN interface, MCP still loopback-only) is proven mechanically
rather than asserted in prose.

Checks performed against a target host, dialled on its LAN address:
  1. RECEIVER REACHABLE   the observation port answers on the LAN address
  2. FAIL-CLOSED          a handshake WITHOUT a client certificate is refused
  3. AUTHENTICATED PATH   a valid client certificate completes AND receives an application ACK
  4. MCP NOT EXPOSED      the MCP port is NOT reachable on the LAN address (loopback-only preserved)
  5. ICMP                 reported for information ONLY - never treated as authentication

If check 1 fails, checks 2 and 3 are reported **SKIP**, never PASS: a refused connection is not
evidence that the receiver is fail-closed, and reporting it as such would be a pass for the wrong
reason (the same defect class that produced a false PASS in the first revision of
test_mtls_validation.py).

Self-test (`--self-test`) builds mock receivers locally and proves this tool distinguishes a
compliant layout from three non-compliant ones:
  S1 compliant                         -> PASS PASS PASS PASS
  S2 receiver accepts any/no client cert -> PASS FAIL PASS PASS
  S3 MCP bound to all interfaces         -> PASS PASS PASS FAIL (credential-free leak detected)
  S4 receiver bound to loopback only     -> FAIL SKIP SKIP PASS (no LAN listener)

Stdlib only. Exit code 0 only when every required check holds (or, under --self-test, when every
scenario is classified correctly).
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import ssl
import subprocess
import sys
import threading

FX_DEFAULT = os.path.expanduser("~/.hermes/cache/scratch/hg-mtls-07-fixtures")
PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


def require_fixtures(fx: str) -> str | None:
    """Fail with an actionable message rather than a traceback."""
    if not os.path.exists(f"{fx}/dev-ca/ca.crt"):
        return (f"fixtures not found at {fx} - generate them first:\n"
                f"  bash scripts/mtls_fixtures/generate_dev_fixtures.sh {fx}")
    return None


# ----------------------------------------------------------------------------- checks
def tcp_reachable(host: str, port: int, timeout: float = 3.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def icmp_informational(host: str) -> str:
    """Reported, never gated on - a live link is not evidence of authenticated transport."""
    try:
        r = subprocess.run(["ping", "-c", "1", "-W", "2", host], capture_output=True, timeout=6)
        return "replies" if r.returncode == 0 else "no reply"
    except Exception:                                          # noqa: BLE001
        return "unavailable"


def tls_attempt(host: str, port: int, ca: str, client_crt: str | None,
                client_key: str | None, timeout: float = 6.0) -> tuple[bool, str]:
    """Attempt mTLS and require an application-level ACK. Returns (authenticated, detail)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.check_hostname = True                    # SAN-only; a CN is never substituted
    ctx.load_verify_locations(ca)
    if client_crt:
        ctx.load_cert_chain(client_crt, client_key)
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            with ctx.wrap_socket(sock, server_hostname=host) as tls:
                tls.sendall(b"obs")
                ack = tls.recv(2)
                if ack != b"OK":
                    return False, f"no application ACK (got {ack!r})"
                return True, f"authenticated ({tls.version()})"
    except Exception as exc:                                   # noqa: BLE001
        return False, f"{type(exc).__name__}: {str(exc)[:60]}"


def run_checks(host: str, obs_port: int, mcp_port: int, ca: str,
               client_crt: str, client_key: str) -> list[tuple[str, str, bool, str]]:
    """Returns rows of (label, status, required, detail). status is PASS / FAIL / SKIP."""
    rows: list[tuple[str, str, bool, str]] = []

    icmp = icmp_informational(host)
    reachable = tcp_reachable(host, obs_port)
    if reachable:
        d1 = "TCP connect OK"
    elif icmp == "replies":
        d1 = "host answers ICMP but nothing listens on that port - a closed port, not a network fault"
    else:
        d1 = ("no ICMP reply and no TCP - the host is not reachable at all: routing, subnet or "
              "filtering, NOT a closed port")
    rows.append(("1 receiver reachable on the LAN address",
                 PASS if reachable else FAIL, True, d1))

    if not reachable:
        rows.append(("2 fail-closed without a client certificate", SKIP, False,
                     "not applicable - no LAN listener to test"))
        rows.append(("3 valid client certificate authenticates", SKIP, False,
                     "not applicable - no LAN listener to test"))
    else:
        bad_ok, bad_detail = tls_attempt(host, obs_port, ca, None, None)
        rows.append(("2 fail-closed without a client certificate",
                     FAIL if bad_ok else PASS, True,
                     f"ACCEPTED WITHOUT A CERT ({bad_detail})" if bad_ok else "refused"))
        auth_ok, auth_detail = tls_attempt(host, obs_port, ca, client_crt, client_key)
        rows.append(("3 valid client certificate authenticates",
                     PASS if auth_ok else FAIL, True, auth_detail))

    mcp_open = tcp_reachable(host, mcp_port)
    mcp_local = tcp_reachable("127.0.0.1", mcp_port)
    if mcp_open:
        rows.append(("4 MCP port NOT reachable on the LAN address", FAIL, True,
                     "REACHABLE - loopback-only violated"))
    elif not mcp_local:
        # A port that is closed EVERYWHERE is not isolated, it is absent. Certifying that as PASS
        # would report isolation from a state in which there is nothing to isolate - the same class
        # of vacuous result this lane keeps finding. Not a pass, and not a failure either.
        rows.append(("4 MCP port NOT reachable on the LAN address", SKIP, False,
                     "nothing is listening on loopback either - ABSENCE, not isolation "
                     "(not counted as passing)"))
    else:
        rows.append(("4 MCP port NOT reachable on the LAN address", PASS, True,
                     "closed on the LAN address while listening on loopback"))

    rows.append(("5 ICMP (informational only)", PASS, False,
                 f"link probe: {icmp} - never a proof of authentication"))
    return rows


# ----------------------------------------------------------------------------- mocks
def _serve_receiver(listen_host: str, port_box: list, srv_crt: str, srv_key: str,
                    ca: str, require_client: bool, stop: threading.Event) -> None:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(srv_crt, srv_key)
    ctx.verify_mode = ssl.CERT_REQUIRED if require_client else ssl.CERT_NONE
    if require_client:
        ctx.load_verify_locations(ca)
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((listen_host, 0))
    s.listen(4)
    s.settimeout(0.3)
    port_box.append(s.getsockname()[1])
    while not stop.is_set():
        try:
            conn, _ = s.accept()
        except (socket.timeout, OSError):
            continue
        try:
            with ctx.wrap_socket(conn, server_side=True) as tls:
                tls.recv(16)
                tls.sendall(b"OK")
        except Exception:                                      # noqa: BLE001
            pass
    s.close()


def _serve_plain(listen_host: str, port_box: list, stop: threading.Event) -> None:
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((listen_host, 0))
    s.listen(4)
    s.settimeout(0.3)
    port_box.append(s.getsockname()[1])
    while not stop.is_set():
        try:
            conn, _ = s.accept()
            conn.close()
        except (socket.timeout, OSError):
            continue
    s.close()


def primary_lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))                            # no packets are sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def mint_selftest_cert(fx: str, lan_ip: str) -> tuple[str, str]:
    """A server cert whose SAN covers the address the self-test actually dials."""
    out = os.path.join(fx, "runtime")
    crt, key = os.path.join(out, "selftest-srv.crt"), os.path.join(out, "selftest-srv.key")
    ext = os.path.join(out, "selftest-srv.ext")
    with open(ext, "w") as fh:
        fh.write("basicConstraints=CA:FALSE\n"
                 "keyUsage=critical,digitalSignature,keyEncipherment\n"
                 "extendedKeyUsage=serverAuth\n"
                 f"subjectAltName=IP:{lan_ip},IP:127.0.0.1\n")
    subprocess.run(["openssl", "genrsa", "-out", key, "2048"], capture_output=True)
    csr = os.path.join(out, "selftest-srv.csr")
    subprocess.run(["openssl", "req", "-new", "-key", key, "-out", csr,
                    "-subj", "/CN=homeguardian-hub"], capture_output=True)
    subprocess.run(["openssl", "x509", "-req", "-in", csr, "-CA", f"{fx}/dev-ca/ca.crt",
                    "-CAkey", f"{fx}/dev-ca/ca.key", "-CAcreateserial", "-out", crt,
                    "-days", "2", "-sha256", "-extfile", ext], capture_output=True)
    os.chmod(key, 0o600)
    return crt, key


def self_test(fx: str) -> int:
    lan = primary_lan_ip()
    crt, key = mint_selftest_cert(fx, lan)
    ca, el, elk = f"{fx}/dev-ca/ca.crt", f"{fx}/devices/eluga-i7-01.crt", f"{fx}/devices/eluga-i7-01.key"

    # (name, receiver bind, require client cert, MCP bind, expected statuses for checks 1-4)
    scenarios = [
        ("S1 compliant (receiver on LAN, MCP loopback-only)", lan, True, "127.0.0.1",
         [PASS, PASS, PASS, PASS]),
        ("S2 receiver accepts any/no client cert", lan, False, "127.0.0.1",
         [PASS, FAIL, PASS, PASS]),
        ("S3 MCP bound to all interfaces (credential-free leak)", lan, True, "0.0.0.0",
         [PASS, PASS, PASS, FAIL]),
        ("S4 receiver bound to loopback only (no LAN listener)", "127.0.0.1", True, "127.0.0.1",
         [FAIL, SKIP, SKIP, PASS]),
    ]

    print(f"self-test on LAN address {lan}, fixtures {fx}\n")
    violations = 0
    for name, obs_host, require_client, mcp_host, expect in scenarios:
        stop = threading.Event()
        obs_box: list = []
        mcp_box: list = []
        threading.Thread(target=_serve_receiver,
                         args=(obs_host, obs_box, crt, key, ca, require_client, stop), daemon=True).start()
        threading.Thread(target=_serve_plain, args=(mcp_host, mcp_box, stop), daemon=True).start()
        for _ in range(200):
            if obs_box and mcp_box:
                break
            threading.Event().wait(0.02)
        # always dial the LAN address: the gate exists to judge the LAN-exposed surface
        rows = run_checks(lan, obs_box[0], mcp_box[0], ca, el, elk)
        stop.set()
        got = [r[1] for r in rows[:4]]
        ok = got == expect
        violations += 0 if ok else 1
        print(f"{name}")
        print(f"   expected {'/'.join(expect)}   observed {'/'.join(got)}   "
              f"{'ok' if ok else 'TOOL DEFECT'}")
        for label, status, _req, detail in rows[:4]:
            print(f"     {status:4} {label}" + (f"  ({detail[:52]})" if detail else ""))
        print()
    if violations:
        print(f"FAIL: {violations} scenario(s) misclassified - the gate cannot be trusted")
        return 1
    print("PASS: the gate classifies a compliant layout and all three non-compliant ones correctly")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true", help="prove the gate classifies mock layouts")
    ap.add_argument("--host", help="target LAN address")
    ap.add_argument("--observation-port", type=int, default=8443)
    ap.add_argument("--mcp-port", type=int, default=8790)
    ap.add_argument("--fixture-dir", default=FX_DEFAULT)
    ap.add_argument("--client-id", default="eluga-i7-01")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        if (msg := require_fixtures(a.fixture_dir)):
            print(msg)
            return 2
        return self_test(a.fixture_dir)
    if not a.host:
        ap.error("--host is required unless --self-test is used")
    if (msg := require_fixtures(a.fixture_dir)):
        print(msg)
        return 2

    ca = f"{a.fixture_dir}/dev-ca/ca.crt"
    crt = f"{a.fixture_dir}/devices/{a.client_id}.crt"
    key = f"{a.fixture_dir}/devices/{a.client_id}.key"
    rows = run_checks(a.host, a.observation_port, a.mcp_port, ca, crt, key)
    if a.json:
        print(json.dumps([{"check": r[0], "status": r[1], "required": r[2], "detail": r[3]}
                          for r in rows], indent=2))
    else:
        print(f"exposure gate against {a.host} (observation {a.observation_port}, MCP {a.mcp_port})\n")
        for label, status, required, detail in rows:
            tag = status if required else "INFO"
            print(f"  {tag:4}  {label:52} ({detail})")
    bad = [r for r in rows if r[2] and r[1] == FAIL]
    skipped = [r for r in rows if r[1] == SKIP]
    if skipped:
        print(f"\nNOTE: {len(skipped)} check(s) SKIPPED (no LAN listener) - not counted as passing")
    if bad:
        print(f"\nFAIL: {len(bad)} required check(s) did not hold")
        return 1
    print("\nPASS: every required check held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
