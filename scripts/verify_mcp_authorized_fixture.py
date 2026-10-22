#!/usr/bin/env python3
"""HG-WEB-PORTAL-GOOGLE-LOGIN-51 (acceptance item 1): an ISOLATED, localhost-only fixture that
reproduces and fixes the "authorized tools/call fails" defect, with no phone, no VPN and no Docker.

WHAT WAS ACTUALLY WRONG (established here, not guessed)
  The MCP authorizes through the Alexa contract's verifier (McpHttpServer.cpp:404,
  verifier.verify(credential, now)). My lane then changed main_mcp_server.cpp to open the alert store
  READ-ONLY (Option A), and that reader REFUSES a store that does not exist yet:

      PersistenceAudit(path, Access::read_only)   src/core/PersistenceAudit.cpp:14-28
        open_read_only(path)      -> throws when the file is missing; never creates it
        AlertRepository(..., ensure_schema=false) -> no CREATE can run
        alert_repo_->count()      -> the proof the home_alerts table is really there; throws otherwise

  and src/mcp/McpTools.cpp:14-16 makes that failure INDISTINGUISHABLE from an auth failure to the
  client: "a client must not be able to tell 'expired token' from 'not permitted' from 'application
  unavailable'". So an unauthorized-looking refusal can actually mean "the store was never
  provisioned" -- and that is exactly what happens to every caller pointed at a fresh path, which is
  what scripts/verify_mcp.sh and web_mcp/test_client.js both do. The server LOG says the real reason
  (my Option A warning), which is what makes this diagnosable at all.

  The fix is therefore PROVISIONING, not a wider gate: a writer creates the store with the schema the
  writer itself uses (AlertRepository.cpp:14-26, quoted below), and the reader then opens it read-only.
  This fixture proves both halves: with a store present the authorized call SUCCEEDS; without one the
  call fails AND the log names the store as unavailable rather than reporting zero alerts.

  Run:  python3 scripts/verify_mcp_authorized_fixture.py [--binary path/to/hg_mcp_server]
  Exit: 0 = every assertion passed.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
TOKEN = "fixture-read-only-token"
PRINCIPAL = "fixture-reader"

# The writer's own DDL, transcribed from src/persistence/AlertRepository.cpp:14-26. The reader does not
# create it -- this fixture plays the writer's part so the reader has a real store to open.
DDL = """
CREATE TABLE IF NOT EXISTS home_alerts (
    alert_id TEXT PRIMARY KEY,
    severity TEXT NOT NULL,
    message TEXT NOT NULL,
    contributing_event_ids TEXT NOT NULL,
    detected_at INTEGER NOT NULL,
    confidence REAL NOT NULL,
    rule_name TEXT NOT NULL,
    details TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_home_alerts_severity ON home_alerts(severity);
CREATE INDEX IF NOT EXISTS idx_home_alerts_detected_at ON home_alerts(detected_at);
"""

results: list[tuple[bool, str, str]] = []


def check(cond: bool, label: str, detail: str = "") -> None:
    results.append((bool(cond), label, detail))
    print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"   {detail}" if detail else ""))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def wait_port(port: int, timeout: float = 12.0) -> bool:
    for _ in range(int(timeout / 0.25)):
        with socket.socket() as s:
            s.settimeout(0.25)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.25)
    return False


def http(port: int, method: str, params: dict, sid: str | None = None, token: str | None = TOKEN) -> tuple[str, str]:
    """POST one JSON-RPC message with curl (the transport is SSE-carrying). Returns (headers, body).

    The body is extracted by scanning the WHOLE combined output for an SSE `data:` line: a combined
    `-D -` stream puts the header block and the event framing in one buffer, and partitioning on the
    first blank line loses the payload. An empty parse must never be allowed to satisfy an assertion --
    callers check payload_nonempty() before trusting a negative.
    """
    args = ["curl", "-s", "-D", "-", "-H", "Content-Type: application/json",
            "-H", "Accept: application/json, text/event-stream"]
    if token is not None:
        args += ["-H", f"Authorization: Bearer {token}"]
    if sid:
        args += ["-H", f"Mcp-Session-Id: {sid}"]
    args += ["-X", "POST", f"http://127.0.0.1:{port}/mcp",
             "-d", json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})]
    out = subprocess.run(args, capture_output=True, text=True, timeout=15).stdout
    lines = out.splitlines()
    # The server answers plain JSON (Content-Type: application/json) with LF line endings; a client that
    # accepts SSE gets the same payload. So take the body as everything after the first BLANK line --
    # splitting on "\r\n\r\n" silently loses it, which is how an empty parse once looked like a pass.
    split_at = next((i for i, l in enumerate(lines) if l.strip() == ""), None)
    if split_at is None:
        headers = [l for l in lines if ":" in l and not l.lstrip().startswith(("{", "data:", "event:"))]
        return "\n".join(headers), ""
    headers, rest = lines[:split_at], lines[split_at + 1:]
    payload = [l[6:] for l in rest if l.startswith("data: ")]
    body = payload[0] if payload else "\n".join(rest).strip()
    return "\n".join(headers), body


def payload_of(body: str) -> dict:
    try:
        return json.loads(body)
    except Exception:
        return {}


def nonempty(p: dict) -> bool:
    """A negative assertion is only meaningful when something was actually parsed."""
    return bool(p.get("result")) or bool(p.get("error"))


def start_server(binary: pathlib.Path, cfg: pathlib.Path, creds: pathlib.Path, store: pathlib.Path,
                 log: pathlib.Path) -> tuple[subprocess.Popen, int]:
    port = free_port()
    env = dict(os.environ, HG_MCP_PORT=str(port), HG_MCP_BIND="127.0.0.1", HG_MCP_ALERTS_DB=str(store))
    proc = subprocess.Popen([str(binary), str(cfg), str(creds)], env=env,
                            stdout=open(log, "w"), stderr=subprocess.STDOUT)
    return proc, port


def stop(proc: subprocess.Popen | None) -> None:
    """Terminate by handle only - never by name, and never by a pattern."""
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(REPO / "build" / "hg_mcp_server"))
    args = ap.parse_args()
    binary = pathlib.Path(args.binary).resolve()
    cfg = REPO / "config" / "homeguardian.demo.json"

    print("== isolated localhost MCP fixture (no phone, no VPN, no Docker) ==")
    check(binary.exists(), "native MCP binary present", str(binary))
    if not binary.exists():
        return 1
    check(cfg.exists(), "demo config present", cfg.name)

    work = pathlib.Path(tempfile.mkdtemp(prefix="hg51-mcp-fixture-"))
    proc = None
    try:
        creds = work / "creds.json"
        creds.write_text(json.dumps({TOKEN: {"principal": PRINCIPAL, "allow_status": True,
                                             "allow_alert_summary": True}}, indent=2))
        os.chmod(creds, 0o600)
        check((creds.stat().st_mode & 0o777) == 0o600, "credential file is 0600")

        store = work / "alerts.db"

        # ---- the defect first: NO store provisioned ------------------------------------------
        proc, port = start_server(binary, cfg, creds, store, work / "unavailable.log")
        check(wait_port(port), "server binds loopback with no store present", f"port {port}")
        head, _ = http(port, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "fixture", "version": "1"}})
        sid = next((l.split(":", 1)[1].strip() for l in head.splitlines()
                    if l.lower().startswith("mcp-session-id:")), "")
        check(bool(sid), "session minted without a store")
        _, body = http(port, "tools/call", {"name": "get_homeguardian_status", "arguments": {}}, sid)
        p = payload_of(body)
        check(nonempty(p) and "could not be authorized" in body,
              "UNPROVISIONED store -> the generic refusal (auth and unavailability are one message)",
              body[:80])
        log_txt = (work / "unavailable.log").read_text()
        check("UNAVAILABLE" in log_txt,
              "server LOG names the store unavailable (operator can tell; client cannot)",
              next((l.split("mcp:")[-1].strip()[:100] for l in log_txt.splitlines() if "UNAVAILABLE" in l), ""))
        check(not store.exists(), "the read-only reader did NOT create the store")
        stop(proc); proc = None

        # ---- now provision it the way the writer does ----------------------------------------
        con = sqlite3.connect(store)
        con.executescript(DDL)
        con.commit(); con.close()
        check(store.exists(), "store provisioned with the writer's own DDL", store.name)

        proc, port = start_server(binary, cfg, creds, store, work / "available.log")
        check(wait_port(port), "server binds loopback with the store present", f"port {port}")
        head, _ = http(port, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                            "clientInfo": {"name": "fixture", "version": "1"}})
        sid = next((l.split(":", 1)[1].strip() for l in head.splitlines()
                    if l.lower().startswith("mcp-session-id:")), "")
        check(bool(sid), "session minted")
        log = (work / "available.log").read_text()
        check("READ-ONLY" in log, "server opened the store READ-ONLY (Option A invariant)",
              next((l.split("mcp:")[-1].strip()[:110] for l in log.splitlines() if "READ-ONLY" in l), ""))

        # tools/list: exactly the two read-only tools, no mutating tool
        _, body = http(port, "tools/list", {}, sid)
        p = payload_of(body)
        names = [t.get("name") for t in p.get("result", {}).get("tools", [])]
        check(nonempty(p) and set(names) == {"get_homeguardian_status", "get_alert_summary"},
              "tools/list advertises exactly the two read-only tools", ",".join(n for n in names if n))
        check(nonempty(p) and not any(n and ("set_" in n or "capture" in n) for n in names),
              "no mutating tool advertised")

        # THE ACCEPTANCE ITEM: the authorized read-only call now SUCCEEDS
        _, body = http(port, "tools/call", {"name": "get_homeguardian_status", "arguments": {}}, sid)
        p = payload_of(body)
        text = json.dumps(p)
        check(nonempty(p) and "capture_enabled" in text,
              "AUTHORIZED get_homeguardian_status succeeds", text[:110])
        check(nonempty(p) and not p.get("result", {}).get("isError"),
              "no error flag on the authorized status call")

        _, body = http(port, "tools/call", {"name": "get_alert_summary", "arguments": {}}, sid)
        p = payload_of(body)
        inner: dict = {}
        try:  # the tool's own JSON payload, parsed rather than string-matched (dumps() inserts spaces)
            inner = json.loads(p.get("result", {}).get("content", [{}])[0].get("text", "{}"))
        except Exception:
            inner = {}
        check(nonempty(p) and "alert_count" in inner, "AUTHORIZED get_alert_summary succeeds",
              json.dumps(inner)[:110])
        check(nonempty(p) and inner.get("alert_count") == 0,
              "an empty but REAL store reports 0 truthfully (not unavailable)", json.dumps(inner)[:90])

        # ---- denials must still hold ----------------------------------------------------------
        head, body = http(port, "tools/list", {}, sid, token=None)
        check("401" in head.splitlines()[0] if head else False, "no token -> 401", head.splitlines()[0] if head else "")
        check("bearer" in head.lower(), "401 advertises Bearer")
        head, body = http(port, "tools/list", {}, sid, token="wrong-token")
        check("401" in (head.splitlines()[0] if head else ""), "bad token -> 401",
              head.splitlines()[0] if head else "")
        head, body = http(port, "tools/call", {"name": "get_alert_summary", "arguments": {}}, sid, token=None)
        check("401" in (head.splitlines()[0] if head else ""), "unauthenticated tools/call -> 401",
              head.splitlines()[0] if head else "")
        check("expired" not in body and "principal" not in body, "401 body leaks no diagnostic and no principal")
        check(not any("Set-Cookie" in l for l in head.splitlines()),
              "no Set-Cookie from this server (bearer model, no cookie tracking)")
    finally:
        stop(proc)
        shutil.rmtree(work, ignore_errors=True)

    passed = sum(1 for ok, _, _ in results if ok)
    print(f"\n  fixture: {passed} passed, {len(results) - passed} failed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
