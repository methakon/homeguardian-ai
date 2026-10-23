#!/usr/bin/env python3
"""Verify synthetic catalogue -> native rules -> SQLite -> native MCP -> ASK.

Run with a Python environment containing alexa_skill_python/lambda/requirements.txt:
  python scripts/verify_scenarios_to_alexa.py --build-dir hg_build --output result.json

Every declared scenario runs exactly once with capture on and once with capture
 off, in independent temporary fixtures. No camera, AWS, deployment or proactive
notification is exercised. Fixture credentials are generated, owner-only, and
removed with all fixture databases/logs even on failure. Only the optional JSON
report survives. Exit codes: 0 pass, 1 verification/dependency failure, 2 CLI error,
130 interrupted. Checks remain active under python -O.
"""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
from datetime import datetime, timezone
import importlib
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NEGATIVES = {"normal", "inconclusive", "consent-denied"}
RANK = {"info": 0, "warning": 1, "critical": 2}
INTENTS = ("RecentAlertsIntent", "GetAlertSummaryIntent")


class VerificationError(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def clean_env():
    # Do not inherit live HG settings, proxy routes, AWS credentials or Python
    # injection settings. Native subprocesses receive only these essentials.
    env = {key: os.environ[key] for key in
           ("PATH", "LANG", "LC_ALL", "LD_LIBRARY_PATH", "TMPDIR") if key in os.environ}
    env.update(HG_ALEXA_TRANSPORT="loopback", HG_BRIDGE_SECURE_CREDS="1",
               HG_MCP_BIND="127.0.0.1", HG_MCP_ALLOW_REMOTE="0", HG_MCP_ALLOW_VPN="0")
    return env


@contextmanager
def sdk_environment(env):
    old = dict(os.environ)
    try:
        os.environ.clear()
        os.environ.update(env)
        yield
    finally:
        os.environ.clear()
        os.environ.update(old)


def private_file(path, content):
    with open(path, "x", opener=lambda name, flags: os.open(name, flags, 0o600)) as stream:
        stream.write(content)
    require(path.stat().st_mode & 0o777 == 0o600, "fixture file not owner-only")


class Fixture:
    def __init__(self, directory, build, capture):
        self.directory = directory
        self.build = build
        self.capture = capture
        self.processes = []
        self.logs = []
        self.env = clean_env()
        self.env["HOME"] = str(directory)
        self.env["HG_HTTP_TOKEN"] = secrets.token_hex(24)
        self.env["HG_MCP_TOKEN"] = self.env["HG_HTTP_TOKEN"]
        self.env["HG_MCP_AUDIT_DB"] = str(directory / "homeguardian_bridge_audit.db")
        self.config = directory / "config.json"
        self.credentials = directory / "credentials.json"
        self.secret = directory / "session.secret"
        private_file(self.config, json.dumps({
            "media_capture_enabled": capture, "cloud_processing_enabled": False,
            "data_dir": str(directory / "data"), "server_host": "127.0.0.1",
            "server_port": 8080, "log_level": "critical"}))
        private_file(self.credentials, json.dumps({self.env["HG_MCP_TOKEN"]: {
            "principal": "synthetic-verifier", "allow_status": True,
            "allow_alert_summary": True, "expires_at_ms": 0, "revoked": False}}))
        private_file(self.secret, secrets.token_hex(24))
        self.base = None

    def start(self, name, args, port):
        log = (self.directory / (name + ".log")).open("wb")
        self.logs.append(log)
        proc = subprocess.Popen(args, cwd=self.directory, env=self.env,
                                stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                start_new_session=True)
        self.processes.append(proc)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            require(proc.poll() is None, name + " exited before readiness")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return proc
            except OSError:
                time.sleep(0.05)
        raise VerificationError(name + " readiness timeout")

    @staticmethod
    def stop(proc):
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait(timeout=4)

    def close(self):
        try:
            for proc in reversed(self.processes):
                self.stop(proc)
        finally:
            for log in self.logs:
                log.close()

    def writer(self):
        port = free_port()
        self.base = "http://127.0.0.1:" + str(port)
        proc = self.start("writer", [str(self.build / "hg_alexa_bridge"), "--http",
            str(self.config), str(self.credentials), "--port", str(port),
            "--secret-file", str(self.secret)], port)
        require(self.http("/api/health").get("ok") is True, "writer health failed")
        return proc

    def http(self, path, post_headers=None):
        headers = {"Origin": self.base, "X-HG-Token": self.env["HG_HTTP_TOKEN"]}
        if post_headers is not None:
            headers.update(post_headers)
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path,
            data=b"{}" if post_headers is not None else None, headers=headers)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=20) as response:
            return json.load(response)

    def catalogue(self):
        result = self.http("/api/scenarios")
        require(result.get("ok") is True, "catalogue failed")
        entries = result["scenarios"]
        ids = [entry["id"] for entry in entries]
        require(bool(ids) and all(isinstance(s, str) and s for s in ids), "empty/invalid catalogue")
        require(len(ids) == len(set(ids)), "duplicate catalogue IDs")
        require(NEGATIVES <= set(ids), "required negative scenarios missing")
        return entries

    def run(self, scenario):
        session = self.http("/api/session", {"x-hg-secret": self.secret.read_text()})
        require(session.get("ok") is True and bool(session.get("csrf")), "session failed")
        return self.http("/api/run-scenario?" + urllib.parse.urlencode({"scenario": scenario}),
                         {"x-hg-csrf": session["csrf"]})

    def reader(self):
        port = free_port()
        self.env["HG_MCP_HOST"] = "127.0.0.1"
        self.env["HG_MCP_PORT"] = str(port)
        return self.start("reader", [str(self.build / "hg_mcp_server"),
                                   str(self.config), str(self.credentials)], port)


def envelope(intent):
    user = {"userId": "synthetic-verifier-user"}
    application = {"applicationId": "amzn1.ask.skill.synthetic-verifier"}
    return {"version": "1.0", "session": {"new": True,
        "sessionId": "synthetic-session", "application": application, "user": user,
        "attributes": {}}, "context": {"System": {"application": application,
        "user": user, "device": {"deviceId": "synthetic-device", "supportedInterfaces": {}}}},
        "request": {"type": "IntentRequest", "requestId": "synthetic-" + intent,
        "timestamp": datetime.now(timezone.utc).isoformat(), "locale": "en-US",
        "intent": {"name": intent, "confirmationStatus": "NONE", "slots": {}}}}


def speech_text(response):
    speech = response["response"]["outputSpeech"]
    require(speech.get("type") == "SSML", "ASK response is not SDK SSML")
    return "".join(ET.fromstring(speech["ssml"]).itertext())


def verify_case(fixture, scenario, catalogue, backend, skill):
    writer = fixture.writer()
    require(fixture.catalogue() == catalogue, "catalogue changed between fixtures")
    payload = fixture.run(scenario)
    require(payload.get("status", {}).get("ok") is True and
            payload.get("summary", {}).get("ok") is True, "bridge contract authentication failed")
    result = payload["scenario"]
    require(result.get("ok") is True and result.get("synthetic") is True,
            "scenario did not report synthetic success: " + json.dumps(result, sort_keys=True))
    require(result.get("scenario_id") == scenario, "scenario ID mismatch")
    require(result.get("capture_enabled") is fixture.capture, "capture configuration mismatch")
    count = result["alerts_delivered"]
    require(type(count) is int and count >= 0, "invalid delivered count")
    require(result["alerts_persisted_this_run"] == count == result["persisted_alert_count"],
            "delivered/persisted counts differ")
    alerts = result["alerts"]
    require(len(alerts) == count, "delivered array count mismatch")
    ids = [a["alert_id"] for a in alerts]
    require(len(set(ids)) == count and all(isinstance(i, str) and i for i in ids),
            "duplicate/invalid delivered IDs")
    require(all(a["severity"] in RANK for a in alerts), "unknown output severity")
    expected = {a["alert_id"]: a["severity"] for a in alerts}
    highest = max(expected.values(), key=RANK.get) if count else ""
    require(all(a.get("provenance") == "synthetic" for a in alerts),
            "delivered alert missing synthetic provenance")
    bridge_summary = payload["summary"]
    require(bridge_summary.get("alert_count") == count and
            Counter(bridge_summary.get("alert_ids", [])) == Counter(ids) and
            bridge_summary.get("highest_severity") == highest,
            "writer bridge summary IDs/count/severity mismatch")
    if scenario in NEGATIVES or not fixture.capture:
        require(count == 0 and result["alerts_generated"] == 0, "negative scenario generated alerts")
    elif scenario in {"default", "unusual", "synthetic-critical-impact"}:
        require(count > 0, "required positive scenario produced no durable alert")
        if scenario == "synthetic-critical-impact":
            require(highest == "critical", "synthetic impact did not preserve critical priority")
    if not fixture.capture:
        require(result["cycles"] == 0 and result["events_processed"] == 0,
                "capture-off scenario processed evidence")
    # Stop the writer BEFORE SQL and MCP reads: this is durable cross-process
    # verification, not a shared in-memory state or a mock provider.
    fixture.stop(writer)
    db = Path(fixture.env["HG_MCP_AUDIT_DB"])
    require(db.is_file(), "writer audit database missing")
    with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as connection:
        rows = connection.execute("SELECT alert_id, severity FROM alerts ORDER BY alert_id").fetchall()
    require(len(rows) == count and dict(rows) == expected, "SQLite IDs/count/severity differ")
    fixture.reader()
    with sdk_environment(fixture.env):
        summary = backend.get_alert_summary("synthetic-verifier-user")
        require(isinstance(summary, dict), "real MCP backend unavailable")
        require(summary.get("read_only") is True, "MCP summary not read-only")
        require(summary.get("alert_count") == count, "MCP count mismatch")
        require(Counter(summary.get("alert_ids", [])) == Counter(ids), "MCP IDs mismatch/truncation")
        require(summary.get("highest_severity") == highest, "MCP severity mismatch")
        speeches = {}
        expected_speech = ("There are no alerts recorded for your home." if not count else
            "There %s %d alert%s recorded. The highest severity is %s." %
            ("is" if count == 1 else "are", count, "" if count == 1 else "s", highest))
        for intent in INTENTS:
            actual = speech_text(skill.lambda_handler(envelope(intent), None))
            require(actual == expected_speech, intent + " speech count/severity mismatch")
            speeches[intent] = actual
    return {"scenario_id": scenario, "capture_enabled": fixture.capture, "passed": True,
            "native_result": result, "sqlite_alerts": [dict(alert_id=i, severity=s) for i, s in rows],
            "mcp_summary": summary, "ask_speech": speeches}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "hg_build")
    parser.add_argument("--output", type=Path, help="optional persistent JSON matrix")
    args = parser.parse_args()
    report = {"passed": False, "scope": "isolated synthetic local integration; no deployed voice or notifications",
              "catalogue": [], "matrix": []}
    code = 1
    try:
        build = args.build_dir.resolve()
        for name in ("hg_alexa_bridge", "hg_mcp_server"):
            require((build / name).is_file() and os.access(build / name, os.X_OK),
                    "missing executable: " + name)
        # Import the real repository adapter and ASK SDK entry point, without
        # writing __pycache__ into the checkout or inheriting live credentials.
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(ROOT / "alexa_skill_python" / "lambda"))
        with sdk_environment(clean_env()):
            backend = importlib.import_module("backend")
            skill = importlib.import_module("lambda_function")
        scratch = os.environ.get("TMPDIR")
        require(bool(scratch) and Path(scratch).is_dir(), "TMPDIR must name an existing scratch directory")
        with tempfile.TemporaryDirectory(prefix="hg-scenarios-alexa-", dir=scratch) as temp:
            base = Path(temp)
            discovery = base / "discovery"
            discovery.mkdir(mode=0o700)
            fixture = Fixture(discovery, build, True)
            try:
                fixture.writer()
                catalogue = fixture.catalogue()
            finally:
                fixture.close()
            report["catalogue"] = catalogue
            ids = [entry["id"] for entry in catalogue]
            for capture in (True, False):
                for index, scenario in enumerate(ids):
                    directory = base / ("on-" if capture else "off-") / str(index)
                    directory.mkdir(mode=0o700, parents=True)
                    fixture = Fixture(directory, build, capture)
                    try:
                        row = verify_case(fixture, scenario, catalogue, backend, skill)
                    except Exception as exc:
                        report["matrix"].append({"scenario_id": scenario, "capture_enabled": capture,
                                                 "passed": False, "error": str(exc)})
                        raise
                    finally:
                        fixture.close()
                    report["matrix"].append(row)
                    print("PASS capture=%s scenario=%s alerts=%d" %
                          (capture, scenario, row["mcp_summary"]["alert_count"]), file=sys.stderr)
            for capture in (True, False):
                visited = [r["scenario_id"] for r in report["matrix"] if r["capture_enabled"] is capture]
                require(Counter(visited) == Counter(ids), "catalogue coverage not exactly once per capture mode")
            require(any(r["mcp_summary"]["alert_count"] > 0 for r in report["matrix"]),
                    "no positive alerts exercised")
        report.update(passed=True, scenario_count=len(ids), cases_passed=len(report["matrix"]),
                      sdk_responses_verified=len(report["matrix"]) * len(INTENTS))
        code = 0
    except KeyboardInterrupt:
        report["error"] = "interrupted"
        code = 130
    except Exception as exc:
        report["error"] = type(exc).__name__ + ": " + str(exc)
    output = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        try:
            args.output.write_text(output)
        except OSError as exc:
            print("Cannot write report: " + str(exc), file=sys.stderr)
            print(output, end="")
            return 1
    else:
        print(output, end="")
    print("PASS" if code == 0 else "FAIL: " + report.get("error", "verification failed"), file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
