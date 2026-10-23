#!/usr/bin/env python3
"""Portal smoke test: starts the backup portal and proves the behaviours the mission requires.

Run from the repository root:

    python3 scripts/portal_smoke.py

It starts hg_alexa_bridge in its native HTTP mode on a loopback port with a throwaway upload store,
drives the real HTTP surface, prints one line per check, stops the server, and exits non-zero if any
check failed. Nothing is deployed and nothing outside a temporary directory is touched.

Why a script and not just unit tests: the unit tests cover the store's validation logic directly, but
they cannot observe HTTP status codes, CSRF enforcement, the served dashboard, or what the process
does when the backend is unavailable. Every defect found so far in this component was found here.

Note on the issuance rate limit: the transport mints at most 5 CSRF sessions per 10-second window, so
this script reuses the token where a check does not need a fresh one and waits out the window when it
does. That limit is the transport's own anti-abuse bound, not a property of the portal.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

CHECKS: list[tuple[bool, str, object, object, str]] = []


def check(name: str, want, got, detail: str = "") -> None:
    CHECKS.append((want == got, name, want, got, detail))
    mark = "PASS" if want == got else "FAIL"
    print(f"  {mark}  {name:<56} want={want!s:>14}  got={got!s:>14}  {detail}")


# ----------------------------------------------------------------- media fixtures (real containers)

def png(extra: int = 0) -> bytes:
    """A structurally plausible PNG: the signature is what the validator verifies."""
    return bytes([0x89]) + b"PNG\r\n\x1a\n" + bytes(24) + b"\0" * extra


def mp4(seconds: int = 5, timescale: int = 1000) -> bytes:
    """A minimal MP4 carrying a real moov/mvhd, so the duration is read from the container."""
    def be32(v: int) -> bytes:
        return struct.pack(">I", v)
    ftyp = be32(16) + b"ftypisom" + be32(0x200)
    mvhd = bytearray(be32(108) + b"mvhd" + bytes(100))
    struct.pack_into(">I", mvhd, 8 + 12, timescale)
    struct.pack_into(">I", mvhd, 8 + 16, seconds * timescale)
    return ftyp + be32(8 + 108) + b"moov" + bytes(mvhd)


class Portal:
    """Starts the portal on a free loopback port with an isolated store and operator secret."""

    def __init__(self, binary: str, config: str, port: int | None):
        self.dir = tempfile.mkdtemp(prefix="hg_portal_smoke_")
        self.secret = "smoke-secret-" + os.urandom(4).hex()
        self.secret_file = os.path.join(self.dir, "secret")
        with open(self.secret_file, "w") as f:
            f.write(self.secret + "\n")
        with open(os.path.join(self.dir, "creds.json"), "w") as f:
            json.dump({"smoke-token": {"principal": "smoke", "allow_status": True,
                                       "allow_alert_summary": True, "expires_at_ms": 0,
                                       "revoked": False}}, f)
        self.port = port or self._free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log = open(os.path.join(self.dir, "server.log"), "w")
        self.proc = subprocess.Popen(
            [binary, "--http", config, os.path.join(self.dir, "creds.json"),
             "--port", str(self.port), "--secret-file", self.secret_file,
             "--uploads-dir", os.path.join(self.dir, "uploads")],
            stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(80):
            with socket.socket() as s:
                s.settimeout(0.25)
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            time.sleep(0.25)
        raise RuntimeError("the portal did not bind; see " + self.log.name)

    @staticmethod
    def _free_port() -> int:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()

    def rss_kib(self) -> str:
        try:
            with open(f"/proc/{self.proc.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return line.split()[1]
        except OSError:
            pass
        return "?"

    def request(self, method: str, path: str, body: bytes | None = None,
                ctype: str | None = None, headers: dict | None = None):
        h = {"Origin": self.base}
        if ctype:
            h["Content-Type"] = ctype
        if headers:
            h.update(headers)
        r = urllib.request.Request(self.base + path, data=body, headers=h, method=method)
        try:
            with urllib.request.urlopen(r, timeout=30) as resp:
                return resp.status, resp.read(), dict(resp.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def csrf(self, attempts: int = 4) -> str:
        for _ in range(attempts):
            s, b, _ = self.request("POST", "/api/session", b"{}", "application/json",
                                   {"x-hg-secret": self.secret})
            if s == 200:
                return json.loads(b)["csrf"]
            if s != 429:
                raise RuntimeError(f"cannot mint a session: {s} {b[:120]!r}")
            time.sleep(11)  # the transport's 10s issuance window: pace, do not fail
        raise RuntimeError("cannot mint a session: issuance kept being limited")

    def cleanup(self) -> None:
        shutil.rmtree(self.dir, ignore_errors=True)


def run(portal: Portal) -> None:
    print("  --- the dashboard a browser receives ---")
    s, body, _ = portal.request("GET", "/")
    html = body.decode("utf-8", "replace")
    check("dashboard served", 200, s)
    check("label present verbatim", True, "Uploaded media — simulated camera feed" in html)
    check("states it is not live CCTV", True, "Not live CCTV" in html)
    check("denies camera/assistant provenance", True, "No camera, device or voice assistant" in html)
    check("upload control present", True, 'id="uploadFile"' in html and 'id="uploadBtn"' in html)
    check("feed container present", True, 'id="feed"' in html)

    print("  --- authorization ---")
    check("session without the operator secret", 401, portal.request("POST", "/api/session", b"{}", "application/json")[0])
    check("session with the operator secret", 200, portal.request("POST", "/api/session", b"{}", "application/json", {"x-hg-secret": portal.secret})[0])
    check("upload without CSRF", 403, portal.request("POST", "/api/upload", png(), "image/png")[0])
    check("delete without CSRF", 403, portal.request("POST", "/api/delete", b'{"id":"x"}', "application/json")[0])

    print("  --- valid uploads, preview and playback ---")
    s, b, _ = portal.request("POST", "/api/upload", png(200), "image/png", {"x-hg-csrf": portal.csrf()})
    check("valid PNG upload", 200, s)
    img = json.loads(b).get("id", "") if s == 200 else ""
    check("upload is labelled simulated", True, json.loads(b).get("simulated") is True if s == 200 else False)
    s, back, h = portal.request("GET", f"/media/{img}")
    check("image served for preview", 200, s, h.get("Content-Type", ""))
    check("preview bytes identical", True, back == png(200))
    s, b, _ = portal.request("POST", "/api/upload", mp4(5), "video/mp4", {"x-hg-csrf": portal.csrf()})
    check("valid 5s MP4 upload", 200, s)
    check("video duration read from the container", 5000, json.loads(b).get("duration_ms") if s == 200 else None)
    vid = json.loads(b).get("id", "") if s == 200 else ""
    s, back, h = portal.request("GET", f"/media/{vid}")
    check("video served for playback", 200, s, h.get("Content-Type", ""))

    print("  --- the feed ---")
    s, b, _ = portal.request("GET", "/api/feed")
    feed = json.loads(b)
    check("feed lists both uploads", 2, len(feed.get("entries", [])))
    check("feed labelled simulated", True, "Uploaded media" in feed.get("label", ""))
    check("feed never claims a live camera", False, feed.get("live_camera"))
    check("each entry carries a media url", True, all(e.get("url", "").startswith("/media/") for e in feed.get("entries", [])))

    print("  --- rejections ---")
    check("script renamed .png", 415, portal.request("POST", "/api/upload", b"#!/bin/sh\nrm -rf /\n", "image/png", {"x-hg-csrf": portal.csrf()})[0])
    check("native content type absent (text/html)", 415, portal.request("POST", "/api/upload", png(), "text/html")[0])
    check("image over the 8 MiB ceiling", 413, portal.request("POST", "/api/upload", png(9 * 1024 * 1024), "image/png", {"x-hg-csrf": portal.csrf()})[0])
    check("path traversal in the media id", 404, portal.request("GET", "/media/../../etc/passwd")[0])
    check("well-formed but unissued id", 404, portal.request("GET", "/media/u_20260101_000000_deadbeef")[0])
    check("delete with a malformed id refused", 400, portal.request("POST", "/api/delete", b'{"id":"nope"}', "application/json", {"x-hg-csrf": portal.csrf()})[0])

    print("  --- delete ---")
    s, _, _ = portal.request("POST", "/api/delete", json.dumps({"id": img}).encode(), "application/json", {"x-hg-csrf": portal.csrf()})
    check("delete with CSRF", 200, s)
    check("deleted media is gone", 404, portal.request("GET", f"/media/{img}")[0])

    print("  --- the real backend, and its absence ---")
    s, b, _ = portal.request("GET", "/api/status")
    check("status answers from the real backend", 200, s)
    # A check that cannot fail is worse than no check: this one asserts the payload is real JSON
    # naming the backend's own state, so a stubbed or empty response would fail it.
    try:
        st = json.loads(b)
        real = isinstance(st, dict) and ("status" in st or "summary" in st)
    except Exception:
        real = False
    check("status payload is the backend's own JSON", True, real)
    check("portal RSS while serving is modest", True, int(portal.rss_kib()) < 200 * 1024 if portal.rss_kib().isdigit() else False,
          f"{portal.rss_kib()} KiB")


def degraded_store_case(binary: str, config: str) -> None:
    """A portal whose upload store cannot be created must still serve, and must not invent content.

    This is the honest test of 'the backend component is unavailable': an unwritable uploads dir makes
    the store fail to load, and the requirement is that the portal keeps answering (an empty, clearly
    labelled feed) rather than erroring out or fabricating entries.
    """
    print("  --- a portal whose upload store is unavailable ---")
    dir_ = tempfile.mkdtemp(prefix="hg_portal_degraded_")
    try:
        secret_file = os.path.join(dir_, "secret")
        with open(secret_file, "w") as f:
            f.write("degraded-secret\n")
        with open(os.path.join(dir_, "creds.json"), "w") as f:
            json.dump({"t": {"principal": "p", "allow_status": True, "allow_alert_summary": True,
                             "expires_at_ms": 0, "revoked": False}}, f)
        with socket.socket() as sk:
            sk.bind(("127.0.0.1", 0))
            port = sk.getsockname()[1]
        proc = subprocess.Popen([binary, "--http", config, os.path.join(dir_, "creds.json"),
                                 "--port", str(port), "--secret-file", secret_file,
                                 "--uploads-dir", "/proc/self/definitely-not-writable"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            up = False
            for _ in range(80):
                with socket.socket() as sk:
                    sk.settimeout(0.25)
                    if sk.connect_ex(("127.0.0.1", port)) == 0:
                        up = True
                        break
                time.sleep(0.25)
            check("portal still serves with an unusable store", True, up)
            if up:
                r = urllib.request.Request(f"http://127.0.0.1:{port}/api/feed",
                                           headers={"Origin": f"http://127.0.0.1:{port}"})
                with urllib.request.urlopen(r, timeout=10) as resp:
                    d = json.loads(resp.read())
                check("degraded feed answers rather than erroring", 200, resp.status)
                check("degraded feed invents nothing", 0, len(d.get("entries", [])))
                check("degraded feed still carries the label", True, "Uploaded media" in d.get("label", ""))
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    finally:
        shutil.rmtree(dir_, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="build/hg_alexa_bridge")
    ap.add_argument("--config", default="config/homeguardian.demo.json")
    ap.add_argument("--port", type=int, default=0)
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print(f"  binary not found: {args.binary} (build it first)", file=sys.stderr)
        return 2
    print(f"  using {args.binary}")
    portal = Portal(args.binary, args.config, args.port)
    print(f"  portal up on {portal.base} (pid {portal.proc.pid})")
    try:
        run(portal)
    finally:
        portal.stop()
        portal.cleanup()
    degraded_store_case(args.binary, args.config)
    failed = [c for c in CHECKS if not c[0]]
    print(f"\n  smoke: {len(CHECKS) - len(failed)} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
