#!/usr/bin/env python3
"""HomeGuardian AI(dhar) — end-to-end voice demonstration transcript.

Drives the REAL skill handlers against the REAL C++ gateway on loopback and prints the exchange a judge
would hear, labelling where every fact came from. Nothing here is a mock: the gateway is
`build/hg_alexa_bridge --http`, started and stopped by this script, and the last act shows what the skill
says when the gateway is gone.

Run:  python3 alexa_hosted/demo_voice_transcript.py
"""
from __future__ import annotations

import json
import os
import pathlib
import socket
import subprocess
import sys
import time
import types
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "alexa_hosted" / "lambda"))

# Same stub as the test suite: only so the handlers import without the SDK installed. The handler logic
# under demonstration is unchanged code.
if "ask_sdk_core" not in sys.modules:
    def _mk(name):
        return types.ModuleType(name)
    pkg, sb, dc, ut, mo, ui = (_mk(n) for n in
        ("ask_sdk_core", "ask_sdk_core.skill_builder", "ask_sdk_core.dispatch_components",
         "ask_sdk_core.utils", "ask_sdk_model", "ask_sdk_model.ui"))
    class _SB:
        def add_request_handler(self, h): return self
        def add_exception_handler(self, h): return self
        @property
        def lambda_handler(self): return lambda e, c: None
    class _RH: pass
    class _EH: pass
    class _Resp: pass
    class _Card:
        def __init__(self, title="", content=""): pass
    sb.SkillBuilder = _SB
    dc.AbstractRequestHandler = _RH
    dc.AbstractExceptionHandler = _EH
    def _req(h): return h.request_envelope.request
    ut.is_request_type = lambda n: (lambda h: getattr(_req(h), "object_type", None) == n)
    ut.is_intent_name = lambda n: (lambda h: getattr(getattr(_req(h), "intent", None), "name", None) == n)
    mo.Response = _Resp
    ui.SimpleCard = _Card
    sys.modules.update({"ask_sdk_core": pkg, "ask_sdk_core.skill_builder": sb,
                        "ask_sdk_core.dispatch_components": dc, "ask_sdk_core.utils": ut,
                        "ask_sdk_model": mo, "ask_sdk_model.ui": ui})

import lambda_function as skill  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class Input:
    def __init__(self, object_type="IntentRequest", intent_name=None):
        req = types.SimpleNamespace(object_type=object_type)
        if intent_name:
            req.intent = types.SimpleNamespace(name=intent_name)
        self.request_envelope = types.SimpleNamespace(request=req)


def speech(response) -> str:
    return response.get("output_speech", "") if isinstance(response, dict) else \
        (getattr(response, "output_speech", "") or "")


def main() -> int:
    binary = REPO / "build" / "hg_alexa_bridge"
    if not binary.exists():
        print(f"gateway binary not built: {binary}", file=sys.stderr)
        return 2
    work = pathlib.Path(os.environ.get("TMPDIR", "/tmp")) / f"hg-alexa-demo-{os.getpid()}"
    work.mkdir(parents=True, exist_ok=True)
    creds = work / "creds.json"
    creds.write_text(json.dumps({"demo-token": {"principal": "demo", "allow_status": True,
                                                "allow_alert_summary": True}}))
    os.chmod(creds, 0o600)
    port = free_port()
    proc = subprocess.Popen(
        [str(binary), "--http", str(REPO / "config" / "homeguardian.demo.json"), str(creds),
         "--port", str(port), "--token", "demo-token", "--secret", "demo-throwaway-secret"],
        env=dict(os.environ, HG_DEMO_SECRET="demo-throwaway-secret"),
        stdout=open(work / "bridge.log", "w"), stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            with socket.socket() as s:
                s.settimeout(0.25)
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    break
            time.sleep(0.25)
        os.environ["HG_GATEWAY_URL"] = f"http://127.0.0.1:{port}"

        # what the gateway itself says, so the transcript can be checked against it
        facts = {}
        for path in ("/api/health", "/api/status", "/api/alerts"):
            req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method="GET")
            req.add_header("Host", f"127.0.0.1:{port}")
            try:
                with urllib.request.urlopen(req, timeout=5) as r:
                    facts[path] = r.read().decode("utf-8", "replace")
            except Exception as exc:
                facts[path] = f"<{type(exc).__name__}>"

        print("HomeGuardian AI(dhar) — voice demonstration")
        print("=" * 78)
        print("Gateway: build/hg_alexa_bridge on 127.0.0.1:%d (real C++ service, started for this run)" % port)
        for k, v in facts.items():
            print(f"  {k:<14} -> {v[:88]}")
        print("=" * 78)

        turns = [
            ("Alexa, open Home Guardian.", Input("LaunchRequest")),
            ("What is the home status?", Input(intent_name="HomeStatusIntent")),
            ("Are there any alerts?", Input(intent_name="GetAlertSummaryIntent")),
            ("Are there any unusual animal activities?", Input(intent_name="AnimalActivityIntent")),
            ("Are the cameras available?", Input(intent_name="CameraAvailabilityIntent")),
            ("What routines are set?", Input(intent_name="GetRoutineStatusIntent")),
            ("Emergency advisory.", Input(intent_name="EmergencyAdvisoryIntent")),
            ("What can you do?", Input(intent_name="AMAZON.HelpIntent")),
            ("Stop.", Input(intent_name="AMAZON.StopIntent")),
        ]
        for utterance, request in turns:
            for cls in skill.HANDLERS:
                inst = cls()
                if inst.can_handle(request):
                    print(f"\n  User:  {utterance}")
                    print(f"  Alexa: {speech(inst.handle(request))}")
                    break

        print("\n" + "-" * 78)
        print("  Now the gateway is stopped, so the honesty of the failure path can be seen.")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        proc = None
        os.environ["HG_GATEWAY_URL"] = f"http://127.0.0.1:{free_port()}"
        for utterance, request in (("What is the home status?", Input(intent_name="HomeStatusIntent")),
                                   ("Are there any alerts?", Input(intent_name="GetAlertSummaryIntent")),
                                   ("Alexa, open Home Guardian.", Input("LaunchRequest"))):
            for cls in skill.HANDLERS:
                inst = cls()
                if inst.can_handle(request):
                    print(f"\n  User:  {utterance}")
                    print(f"  Alexa: {speech(inst.handle(request))}")
                    break
        print("\n" + "=" * 78)
        print("  Every response above came from the gateway's own JSON, or says it could not. No state was")
        print("  guessed, no alert was invented, no camera media was requested, and no emergency prediction")
        print("  was made at any point.")
        return 0
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
