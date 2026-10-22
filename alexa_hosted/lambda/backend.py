"""HomeGuardian backend adapter for the Alexa-hosted skill.

A VOICE ADAPTER, NOT AN ANALYSER. Every statement the skill makes comes from a parsed response of the
existing C++ HomeGuardian gateway; nothing is invented here. The adapter calls the gateway's READ-ONLY
routes only -- /api/health, /api/status, /api/alerts -- which the C++ server serves without a session or
CSRF token (src/bridge/NativeHttp.h:426-437 in the GET branch, before the session logic at :455+). The
state route, which advances a scenario, is never called: a voice request must not mutate the home, and it
is the only route that wants the launch secret (x-hg-secret) or a CSRF token (x-hg-csrf).

No media is requested, proxied or referenced. The project's own readiness spec records that neither camera
has an authenticated interface a viewer may attach to, so no camera route exists here at all.

Security posture:
  * loopback Host header, no Origin, short timeout, and no credentials on any request;
  * the body is parsed, never echoed, and only allowlisted fields are read;
  * no URL, query, header value or body is ever logged by this module;
  * when the gateway is unreachable the caller gets BackendUnavailable and the handler says it cannot
    reach the service -- never "no alerts". Those are different statements about the state of the home.
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request

DEFAULT_TIMEOUT_S = float(os.environ.get("HG_ALEXA_TIMEOUT_S", "3"))
MAX_BODY_BYTES = 64 * 1024


class BackendUnavailable(Exception):
    """A read did not produce usable data.

    `reason` is one of: unreachable | refused | unparseable | unexpected_shape. The distinction is carried
    rather than flattened because saying "I can't reach the service" when the service answered and refused
    would itself be an untrue statement -- and this skill exists to avoid exactly that.
    """

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


def gateway_base_url() -> str:
    """Where the C++ gateway is. Loopback by default; the HTTPS origin is an owner-supplied value."""
    return os.environ.get("HG_GATEWAY_URL", "http://127.0.0.1:8080").rstrip("/")


def gateway_secret() -> str:
    """The operator credential, from the host environment only -- never from this file, and never logged."""
    return os.environ.get("HG_GATEWAY_SECRET", "")


def _get(path: str) -> dict:
    base = gateway_base_url()
    req = urllib.request.Request(base + path, method="GET")
    req.add_header("Host", base.split("://", 1)[-1])
    req.add_header("Accept", "application/json")
    secret = gateway_secret()
    if secret:
        # The read routes on the C++ side hand this to the same verifier the state route uses, so a reader
        # can be authorized as itself instead of anonymously.
        req.add_header("x-hg-secret", secret)
    try:
        with urllib.request.urlopen(req, timeout=DEFAULT_TIMEOUT_S) as resp:
            raw = resp.read(MAX_BODY_BYTES)
            status = getattr(resp, "status", 200)
            if status != 200:
                raise BackendUnavailable("refused")
    except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError) as exc:
        # Deliberately terse: no URL, query, header value or payload reaches the caller or a log.
        raise BackendUnavailable("unreachable") from None
    try:
        data = json.loads(raw.decode("utf-8", "replace"))
    except Exception:
        raise BackendUnavailable("unparseable") from None
    if not isinstance(data, dict):
        raise BackendUnavailable("unexpected_shape")
    # An error is NOT data. The gateway can answer HTTP 200 with an error object alongside an EMPTY
    # collection, and reading that empty list as "zero alerts" would be a false all-clear -- the one
    # failure mode a security skill must never have. Every error shape seen in practice is refused here.
    # Measured: the gateway marks SUCCESS with {"error":"ok"} -- a truthy string. Refusing on any non-empty
    # error field would therefore throw away perfectly good data and, worse, report a working service as
    # refused. Only a value that is not a success marker counts as an error.
    SUCCESS = {"", "ok", "none", "success", "0"}
    def refused(body: dict) -> bool:
        if body.get("ok") is False:
            return True
        err = body.get("error")
        return isinstance(err, str) and err.strip().lower() not in SUCCESS

    if refused(data):
        raise BackendUnavailable("refused")
    for section_name in ("status", "summary"):
        inner = data.get(section_name)
        if isinstance(inner, dict) and refused(inner):
            raise BackendUnavailable("refused")
    return data


def section(data: dict, *names: str) -> dict:
    """The body the caller wants, whichever shape the gateway used.

    Measured: /api/status answers {"status":{...},"summary":{...}} while /api/alerts answers flat
    {"alerts":[],"ok":true,"total":0}. A reader that assumes one shape reports "not established" for data
    that was right there -- so both are accepted, and an unknown shape is refused rather than guessed.
    """
    for name in names:
        inner = data.get(name)
        if isinstance(inner, dict):
            merged = dict(inner)
            merged.setdefault("ok", data.get("ok"))
            return merged
    return data


def get_health() -> dict:
    return _get("/api/health")


def get_status() -> dict:
    """Application status. Read-only. The only source for `capture_enabled`."""
    return _get("/api/status")


def get_alerts() -> dict:
    """Recent alert rows. An unreachable gateway raises; it never becomes a zero."""
    return _get("/api/alerts")


def describe_alerts() -> tuple[int, str, list]:
    """(count, highest_severity, alert_ids) from verified data. Raises BackendUnavailable otherwise."""
    data = section(get_alerts(), "summary", "alerts_section")
    alerts = data.get("alerts")
    if not isinstance(alerts, list):
        raise BackendUnavailable("unexpected_shape")
    order = {"critical": 3, "error": 3, "warning": 2, "warn": 2, "info": 1, "notice": 1}
    severities = [str(a.get("severity", "")).lower() for a in alerts if isinstance(a, dict)]
    highest = max((s for s in severities if s in order), key=lambda s: order[s], default="none")
    ids = [str(a.get("alert_id")) for a in alerts if isinstance(a, dict) and a.get("alert_id")]
    return len(alerts), highest, ids


def capture_enabled() -> bool | None:
    """True/False from the gateway, or None when it could not be established (never guessed)."""
    value = section(get_status(), "status").get("capture_enabled")
    return value if isinstance(value, bool) else None
