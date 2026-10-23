"""Backend access for the HomeGuardian Alexa skill.

WHAT THIS TALKS TO
The native read-only MCP server, over loopback, calling exactly two tools:

    get_homeguardian_status      (src/mcp/McpTools.h:23  kToolStatus)
    get_alert_summary            (src/mcp/McpTools.h:24  kToolAlertSummary)

THE ONE RULE THIS MODULE EXISTS TO ENFORCE
If authenticated backend access is not available, the skill says so. It does not guess, does not
substitute a plausible number, and does not answer from anything cached. A voice assistant that
invents a household reading is worse than one that says it cannot reach the hub: the user cannot tell
the difference by listening, and the whole point of a security product is that its answers mean
something.

So every failure path here returns `None`, and every caller in lambda_function.py turns `None` into
"HomeGuardian is currently unavailable." Nothing else is ever rendered as data.

SCOPE AND SAFETY
  - The endpoint is LOOPBACK by default (127.0.0.1:8790) and this module deliberately refuses to send
    a request to a non-loopback host even when it is explicitly configured, because the mission states the
    phone-side MCP listener stays loopback-only. The existing private phone/VPS connection does not
    establish an Alexa-cloud route. We therefore make no claim of live Alexa remote access.
  - The bearer token is read from the environment at call time, never logged, never echoed into a
    response, and never included in an error message.
  - Household identity is carried as a request header, so a future per-household authorization layer on
    the hub can enforce it server-side. This module does not itself decide what a household may see —
    it forwards the identity and reports what the hub returns. Authorization stays where the data is.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import time
import urllib.error
import urllib.request

# Loopback only. If this ever needs to point elsewhere, that is a decision for the owner, not a default
# this file should quietly acquire.
DEFAULT_MCP_HOST = "127.0.0.1"
DEFAULT_MCP_PORT = 8790

# Socket timeouts plus a phase-boundary operation budget; this is not an Alexa-cloud route.
CONNECT_TIMEOUT_S = 3.0
MAX_BODY_BYTES = 64 * 1024

TOOL_STATUS = "get_homeguardian_status"
TOOL_ALERT_SUMMARY = "get_alert_summary"


class BackendUnavailable(Exception):
    """Raised when the hub cannot be reached or did not answer usably.

    Carrying a REASON separately from a MESSAGE matters: the reason goes to the skill's logs for the
    operator, and the message is what a person hears. A credential never appears in either.
    """

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


def _endpoint() -> tuple[str, int]:
    host = os.environ.get("HG_MCP_HOST", DEFAULT_MCP_HOST)
    try:
        port = int(os.environ.get("HG_MCP_PORT", str(DEFAULT_MCP_PORT)))
    except ValueError:
        raise BackendUnavailable("invalid port configuration") from None
    if not 1 <= port <= 65535:
        raise BackendUnavailable("invalid port configuration")
    if host not in ("127.0.0.1", "::1", "localhost"):
        # Explicit, loud, and refuses to proceed on a guess.
        raise BackendUnavailable("mcp host is not loopback; refusing to send")
    return host, port


def _token() -> str | None:
    token = os.environ.get("HG_MCP_TOKEN")
    if token and ("\r" in token or "\n" in token):
        raise BackendUnavailable("invalid token configuration")
    return token if token else None


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _open(request, timeout):
    # Neither proxy environment variables nor redirects may move credentials
    # away from the explicitly allowed loopback destination.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())
    return opener.open(request, timeout=timeout)


def _rpc(tool: str, household_id: str | None) -> dict:
    """Call one read-only MCP tool and return its parsed JSON result.

    Raises BackendUnavailable for every failure. Never returns invented data.
    """
    host, port = _endpoint()
    token = _token()
    if not token:
        # Not an error worth logging loudly: the local demo simply has no credential yet.
        raise BackendUnavailable("no MCP token configured")

    if tool not in (TOOL_STATUS, TOOL_ALERT_SUMMARY):
        raise BackendUnavailable("tool is not permitted")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "Authorization": "Bearer " + token,
    }
    if household_id:
        if not isinstance(household_id, str) or "\r" in household_id or "\n" in household_id:
            raise BackendUnavailable("invalid household header")
        # Forwarded, not interpreted. The hub decides what this household may read.
        headers["X-HG-Household"] = household_id

    url_host = "[::1]" if host == "::1" else host
    url = "http://%s:%d/mcp" % (url_host, port)
    deadline = time.monotonic() + CONNECT_TIMEOUT_S

    def send(payload=None, session=None, method="POST"):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise BackendUnavailable("backend operation timed out")
        request_headers = dict(headers)
        if session:
            request_headers["Mcp-Session-Id"] = session
        request = urllib.request.Request(url, data=None if payload is None else json.dumps(payload).encode("utf-8"),
                                         headers=request_headers, method=method)
        try:
            with _open(request, timeout=min(1.0, remaining)) as response:
                raw = response.read(MAX_BODY_BYTES + 1)
                session_header = response.getheader("Mcp-Session-Id")
            if len(raw) > MAX_BODY_BYTES:
                raise BackendUnavailable("backend response is too large")
            return raw.decode("utf-8"), session_header
        except (urllib.error.URLError, socket.timeout, OSError, ValueError, UnicodeError):
            raise BackendUnavailable("backend request failed") from None

    session = None
    try:
        body, session = send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "homeguardian-alexa", "version": "1.0"}}})
        if not isinstance(session, str) or not session or len(session) > 512 or "\r" in session or "\n" in session:
            session = None
            raise BackendUnavailable("backend did not establish a usable session")
        _parse_envelope(body, expected_id=1)
        body, _ = send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                        "params": {"name": tool, "arguments": {}}}, session=session)
        _parse_envelope(body, expected_id=2)
        return _parse_rpc(body)
    finally:
        if session:
            try:
                send(session=session, method="DELETE")
            except BackendUnavailable:
                pass


def _parse_envelope(body: str, expected_id=None) -> dict:
    """Extract the tool result from a JSON-RPC response, tolerating an SSE-framed body.

    The MCP server may answer with `text/event-stream`, in which case the JSON sits on `data:` lines.
    """
    text = body.strip()
    if text.startswith(("event:", "data:")) or "\ndata:" in text:
        chunks = [
            line[len("data:"):].strip()
            for line in text.splitlines()
            if line.startswith("data:")
        ]
        text = chunks[-1] if chunks else ""
    if not text:
        raise BackendUnavailable("hub returned an empty body")

    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        raise BackendUnavailable("hub returned a non-JSON body") from None

    if not isinstance(parsed, dict):
        raise BackendUnavailable("hub returned an unexpected shape")
    if parsed.get("jsonrpc") != "2.0" or (expected_id is not None and parsed.get("id") != expected_id):
        raise BackendUnavailable("hub returned an invalid response envelope")
    if "error" in parsed:
        raise BackendUnavailable("hub reported an error")
    result = parsed.get("result")
    if not isinstance(result, dict):
        raise BackendUnavailable("hub returned no result")
    if result.get("isError") is True:
        raise BackendUnavailable("hub reported a tool error")
    return result


def _parse_rpc(body: str) -> dict:
    result = _parse_envelope(body)

    # A tools/call result carries content blocks of text.
    content = result.get("content")
    if isinstance(content, list) and content:
        first = content[0]
        if isinstance(first, dict) and isinstance(first.get("text"), str):
            inner = first["text"].strip()
            try:
                decoded = json.loads(inner)
                return decoded if isinstance(decoded, dict) else {"value": decoded}
            except json.JSONDecodeError:
                return {"text": inner}
    return result


def get_homeguardian_status(household_id: str | None = None) -> dict | None:
    """Return the hub's own status reading, or None when it cannot be obtained."""
    return _read(TOOL_STATUS, household_id)


def _read(tool: str, household_id: str | None) -> dict | None:
    """Select only an explicitly configured transport; never weaken loopback policy."""
    try:
        configured = Path(__file__).with_name("mailbox_config.json").is_file()
        transport = os.environ.get("HG_ALEXA_TRANSPORT", "mailbox" if configured else "loopback")
        if transport == "mailbox":
            from mailbox_backend import get_result
            return get_result(tool, household_id)
        if transport != "loopback":
            return None
        return _rpc(tool, household_id)
    except (BackendUnavailable, ImportError):
        return None


def get_alert_summary(household_id: str | None = None) -> dict | None:
    """Return the hub's own alert summary, or None when it cannot be obtained."""
    return _read(TOOL_ALERT_SUMMARY, household_id)


def backend_available() -> bool:
    """Cheap reachability probe, used only to choose a message — never to synthesise data."""
    try:
        host, port = _endpoint()
    except BackendUnavailable:
        return False
    try:
        with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S):
            return True
    except OSError:
        return False
