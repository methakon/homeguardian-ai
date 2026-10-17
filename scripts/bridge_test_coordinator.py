#!/usr/bin/env python3
"""
HG-NET-02B Phase 2/3 — test coordinator for the outbound control bridge.

Stands in for the authorized private control coordinator so the bridge can be exercised
end-to-end WITHOUT the Oracle hub being deployed. It speaks exactly the protocol
ControlBridge.java implements, so a pass here is a real pass of the bridge, not of a stub.

It is deliberately adversarial: besides the happy path it tries the things that must FAIL
(an unauthenticated coordinator, a disallowed MCP method, an oversized frame), because a
bridge that relays everything it is handed has not been tested.

Usage:
    bridge_test_coordinator.py --port 9443 [--enrol <secret>] [--host 0.0.0.0]

The enrolment secret is the bridge's OWN credential, obtained once from the device
(app-private file, 0600). It is NOT the MCP bearer token, which this script never sees and
must never be given: the bridge adds that itself, in-process, on its way to loopback.
"""

import argparse
import json
import socket
import struct
import sys
import time

MAGIC = "HGB1"
MAX_FRAME = 64 * 1024
HANDSHAKE_BYTES = 256


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_line(sock, limit=HANDSHAKE_BYTES):
    buf = b""
    while len(buf) <= limit:
        ch = sock.recv(1)
        if not ch:
            return None
        if ch == b"\n":
            return buf.decode("utf-8", "replace")
        buf += ch
    return None


def send_frame(sock, payload: bytes):
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def recv_frame(sock):
    hdr = recv_exact(sock, 4)
    if hdr is None:
        return None
    (n,) = struct.unpack(">I", hdr)
    if n == 0 or n > MAX_FRAME:
        return ("OVERSIZE", n)
    return recv_exact(sock, n)


def rpc(method, req_id, params=None):
    o = {"jsonrpc": "2.0", "id": req_id, "method": method}
    if params is not None:
        o["params"] = params
    return json.dumps(o).encode()


def run_case(sock, name, payload, expect_error=False):
    send_frame(sock, payload)
    resp = recv_frame(sock)
    if resp is None:
        print(f"  {name:<34} FAIL (no response / connection closed)")
        return False
    if isinstance(resp, tuple):
        print(f"  {name:<34} FAIL (bridge returned raw oversize marker)")
        return False
    try:
        obj = json.loads(resp.decode("utf-8"))
    except Exception as e:
        print(f"  {name:<34} FAIL (unparseable response: {e})")
        return False
    is_err = "error" in obj
    ok = (is_err == expect_error)
    detail = ""
    if is_err:
        detail = "error=%s" % obj["error"].get("message", "")[:48]
    elif "result" in obj:
        r = obj["result"]
        if isinstance(r, dict):
            if "tools" in r:
                detail = "tools=%s" % ",".join(t["name"] for t in r["tools"])
            elif "serverInfo" in r:
                detail = "server=%s proto=%s" % (
                    r["serverInfo"].get("name", "?"), r.get("protocolVersion", "?"))
            else:
                detail = "result_keys=%s" % ",".join(list(r)[:4])
    print(f"  {name:<34} {'PASS' if ok else 'FAIL'}  {detail}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9443)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--enrol", default=None,
                    help="the bridge enrolment secret; if omitted, read from stdin")
    ap.add_argument("--single", action="store_true", help="serve one session then exit")
    args = ap.parse_args()

    enrol = args.enrol
    if enrol is None:
        enrol = input("bridge enrolment secret (not the MCP token): ").strip()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"test coordinator listening on {args.host}:{args.port}")
    print(f"  enrolment fingerprint: {__import__('hashlib').sha256(enrol.encode()).hexdigest()[:16]}")

    results = []
    while True:
        conn, addr = srv.accept()
        conn.settimeout(20)
        print(f"\n--- session from {addr[0]}:{addr[1]} ---")
        try:
            hello = recv_line(conn)
            if hello is None:
                print("  FAIL handshake: no hello")
                conn.close()
                continue
            parts = hello.split()
            if not parts or parts[0] != MAGIC or len(parts) != 3:
                print(f"  FAIL handshake: malformed hello {hello[:64]!r}")
                conn.close()
                continue
            device_id, dev_fp = parts[1], parts[2]
            print(f"  device_id={device_id} enrolment_fp={dev_fp}")

            # Present the enrolment secret. The bridge verifies it in constant time.
            conn.sendall((enrol + "\n").encode())
            verdict = recv_line(conn)
            if verdict is None or verdict.strip() != "OK":
                print(f"  REJECTED by the bridge (verdict={verdict!r}) - expected for a bad secret")
                results.append(False)
                conn.close()
                continue
            print("  coordinator ACCEPTED by the bridge")

            results.append(run_case(conn, "1 initialize",
                                    rpc("initialize", 1, {"protocolVersion": "2025-11-25",
                                                          "capabilities": {},
                                                          "clientInfo": {"name": "coord", "version": "1"}})))
            results.append(run_case(conn, "2 tools/list", rpc("tools/list", 2)))
            results.append(run_case(conn, "3 tools/call status",
                                    rpc("tools/call", 3, {"name": "get_homeguardian_status", "arguments": {}})))
            results.append(run_case(conn, "4 tools/call alert summary",
                                    rpc("tools/call", 4, {"name": "get_alert_summary", "arguments": {}})))

            # MUST FAIL: a method outside the allowlist.
            results.append(run_case(conn, "5 disallowed method rejected",
                                    rpc("shutdown", 5), expect_error=True))
            # MUST FAIL: something that is not JSON-RPC at all.
            results.append(run_case(conn, "6 non-JSON-RPC rejected",
                                    b"GET /etc/passwd HTTP/1.1\r\n\r\n", expect_error=True))
            # MUST FAIL: a frame over the cap. The bridge must refuse before allocating.
            big = b'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"pad":"' + b"A" * (MAX_FRAME + 1) + b'"}}'
            send_frame(conn, big)
            resp = recv_frame(conn)
            closed = resp is None
            print(f"  {'7 oversized frame refused':<34} {'PASS' if closed else 'FAIL'}"
                  f"  (connection {'closed' if closed else 'still open'})")
            results.append(closed)
        except Exception as e:
            print(f"  session error: {e}")
        finally:
            try:
                conn.close()
            except Exception:
                pass

        if args.single:
            break

    passed = sum(1 for r in results if r)
    print(f"\n=== coordinator summary: {passed}/{len(results)} passed ===")
    sys.exit(0 if passed == len(results) and results else 1)


if __name__ == "__main__":
    main()
