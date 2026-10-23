#!/usr/bin/env python3
"""Outbound-only, read-only Alexa mailbox worker. No HTTP listener or media path.

Use dedicated SQS credentials, never personal/root AWS credentials. The request
queue chooses only an allowlisted tool, never a destination, token or arguments.
"""
from __future__ import annotations

import argparse
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import signal
import time
import urllib.parse
import urllib.request

TOOLS = {"get_homeguardian_status", "get_alert_summary"}
MAX_BYTES = 64 * 1024


class Unavailable(Exception):
    pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def validate_queue_urls(region, request_url, reply_url):
    if region != "us-east-1":
        raise ValueError("mailbox region is outside the approved initial deployment")
    accounts = []
    for url in (request_url, reply_url):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "https" or parsed.hostname != "sqs." + region + ".amazonaws.com"
                or parsed.port not in (None, 443) or parsed.username or parsed.password
                or parsed.query or parsed.fragment
                or not re.fullmatch(r"/[0-9]{12}/homeguardian-alexa-[A-Za-z0-9_-]+", parsed.path)):
            raise ValueError("invalid mailbox queue URL")
        accounts.append(parsed.path.split("/")[1])
    if accounts[0] != accounts[1] or request_url == reply_url:
        raise ValueError("mailbox queues must be distinct and in the same account")


def minimize(tool, value):
    """Release aggregates only. Invalid readings must not become an all-clear."""
    if not isinstance(value, dict) or value.get("read_only") is not True:
        return None
    if tool == "get_homeguardian_status":
        if not isinstance(value.get("state"), str) or not 0 < len(value["state"]) <= 128:
            return None
        result = {"state": value["state"], "read_only": True}
        if isinstance(value.get("capture_enabled"), bool):
            result["capture_enabled"] = value["capture_enabled"]
        return result
    count = value.get("alert_count")
    if tool != "get_alert_summary" or type(count) is not int or count < 0:
        return None
    result = {"alert_count": count, "read_only": True}
    severity = value.get("highest_severity")
    if isinstance(severity, str) and len(severity) <= 128:
        result["highest_severity"] = severity
    return result


class PrivateMcpClient:
    def __init__(self, url, token, opener=None, approved_private_ip=None):
        parsed = urllib.parse.urlsplit(url)
        try:
            host = ipaddress.ip_address(parsed.hostname or "")
        except ValueError:
            raise Unavailable("private endpoint must use an explicit IP address") from None
        # The owner configures this on the VPS; requests cannot change it. Link-local,
        # public, metadata, arbitrary paths and credential-bearing URLs are refused.
        if (parsed.scheme != "http" or parsed.path != "/mcp" or parsed.query or parsed.fragment
                or parsed.username or parsed.password or parsed.port not in (8790, 18995)
                or not (host.is_loopback or (str(host) == approved_private_ip and host.is_private
                                              and not host.is_link_local and not host.is_reserved))):
            raise Unavailable("endpoint is outside the approved private MCP boundary")
        if not token or "\r" in token or "\n" in token:
            raise Unavailable("invalid private MCP credential")
        self.url = url
        self.token = token
        self.open = opener or urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect()).open

    def fetch(self, tool, expires_at):
        if tool not in TOOLS:
            return None
        session = None

        def send(payload=None, method="POST", cleanup=False):
            remaining = expires_at - time.time()
            if remaining <= 0 and not cleanup:
                raise Unavailable("request expired")
            headers = {"Authorization": "Bearer " + self.token, "Content-Type": "application/json",
                       "Accept": "application/json, text/event-stream"}
            if session:
                headers["Mcp-Session-Id"] = session
            req = urllib.request.Request(self.url, method=method, headers=headers,
                                         data=None if payload is None else json.dumps(payload).encode())
            with self.open(req, timeout=0.25 if cleanup else min(1.0, remaining)) as response:
                raw = response.read(MAX_BYTES + 1)
                sid = response.getheader("Mcp-Session-Id")
            if len(raw) > MAX_BYTES:
                raise Unavailable("response too large")
            text = raw.decode("utf-8").strip()
            if text.startswith(("data:", "event:")):
                chunks = [line[5:].strip() for line in text.splitlines() if line.startswith("data:")]
                text = chunks[-1] if chunks else ""
            return json.loads(text), sid

        def result(data, identifier):
            if (not isinstance(data, dict) or data.get("jsonrpc") != "2.0"
                    or data.get("id") != identifier or "error" in data
                    or not isinstance(data.get("result"), dict) or data["result"].get("isError") is True):
                raise Unavailable("invalid MCP response")
            return data["result"]

        try:
            data, session = send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "homeguardian-alexa-mailbox", "version": "1.0"}}})
            if not isinstance(session, str) or not session or len(session) > 512 or "\r" in session or "\n" in session:
                session = None
                raise Unavailable("no usable MCP session")
            result(data, 1)
            data, _ = send({"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                            "params": {"name": tool, "arguments": {}}})
            value = result(data, 2)
            content = value.get("content")
            if not isinstance(content, list) or not content or not isinstance(content[0], dict):
                raise Unavailable("no tool content")
            return minimize(tool, json.loads(content[0]["text"]))
        except Exception:
            # Deliberately no raw HTTP error, token, URL, message or response logging.
            return None
        finally:
            if session:
                try:
                    send(method="DELETE", cleanup=True)
                except Exception:
                    pass


class Worker:
    def __init__(self, sqs, request_url, reply_url, mcp, clock=time.time):
        self.sqs, self.request_url, self.reply_url, self.mcp, self.clock = sqs, request_url, reply_url, mcp, clock
        self.seen = {}
        self.last_keepalive = self.clock()

    def handle(self, message):
        receipt = message.get("ReceiptHandle")
        try:
            raw = message["Body"]
            if not isinstance(raw, str) or len(raw.encode()) > MAX_BYTES:
                raise ValueError("oversized request")
            request = json.loads(raw)
            if not isinstance(request, dict) or set(request) != {"version", "request_id", "tool", "issued_at", "expires_at"}:
                raise ValueError("invalid envelope")
            now = self.clock()
            identifier, tool, issued, expires = (request[k] for k in ("request_id", "tool", "issued_at", "expires_at"))
            if (type(request["version"]) is not int or request["version"] != 1
                    or not isinstance(identifier, str) or not re.fullmatch(r"[0-9a-f]{32}", identifier)
                    or not isinstance(tool, str) or tool not in TOOLS
                    or type(issued) not in (float, int) or type(expires) not in (float, int)
                    or not math.isfinite(issued) or not math.isfinite(expires)
                    or issued > now + 2 or not 0 < expires - issued <= 5 or expires <= now):
                raise ValueError("invalid or expired request")
        except (KeyError, TypeError, ValueError):
            if receipt:
                self.sqs.delete_message(QueueUrl=self.request_url, ReceiptHandle=receipt)
            return False
        self.seen = {key: deadline for key, deadline in self.seen.items() if deadline > now}
        if identifier in self.seen or len(self.seen) >= 128:
            if receipt:
                self.sqs.delete_message(QueueUrl=self.request_url, ReceiptHandle=receipt)
            return False
        value = minimize(tool, self.mcp.fetch(tool, expires))
        if self.clock() < expires:
            self.sqs.send_message(QueueUrl=self.reply_url, MessageBody=json.dumps({
                "version": 1, "request_id": identifier, "tool": tool,
                "expires_at": expires, "result": value}, allow_nan=False))
        self.seen[identifier] = expires
        if receipt:
            self.sqs.delete_message(QueueUrl=self.request_url, ReceiptHandle=receipt)
        return True

    def poll(self):
        # Interim mitigation for the installed Android bridge's stale native
        # session cache. The server default idle TTL is 1800 seconds; keep this
        # authenticated control session active without publishing unsolicited data.
        # This does not repair recovery after an extended outage/phone bridge restart.
        now = self.clock()
        if now - self.last_keepalive >= 60:
            self.last_keepalive = now
            self.mcp.fetch("get_homeguardian_status", now + 3)
        response = self.sqs.receive_message(QueueUrl=self.request_url, MaxNumberOfMessages=1,
                                           WaitTimeSeconds=10, VisibilityTimeout=10)
        for message in response.get("Messages", []):
            self.handle(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration-seconds", type=int, default=300, choices=range(1, 3601), metavar="1..3600")
    args = parser.parse_args()
    import boto3
    from botocore.config import Config
    region = os.environ["HG_ALEXA_MAILBOX_REGION"]
    request_url, reply_url = (os.environ[k] for k in ("HG_ALEXA_REQUEST_QUEUE_URL", "HG_ALEXA_REPLY_QUEUE_URL"))
    validate_queue_urls(region, request_url, reply_url)
    # Use only a dedicated credential file deployed for this worker; never inherit
    # a root/default profile by accident. The SDK validates its contents.
    credentials = Path(os.environ["HG_ALEXA_WORKER_CREDENTIALS_FILE"])
    if not credentials.is_file() or credentials.stat().st_mode & 0o077:
        raise SystemExit("Dedicated AWS worker credential file must be owner-only")
    os.environ["AWS_SHARED_CREDENTIALS_FILE"] = str(credentials)
    os.environ["AWS_CONFIG_FILE"] = os.devnull
    for key in ("AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_PROFILE", "AWS_DEFAULT_PROFILE"):
        os.environ.pop(key, None)
    session = boto3.Session(profile_name="homeguardian-alexa-worker", region_name=region)
    sqs = session.client("sqs", config=Config(connect_timeout=1, read_timeout=12, retries={"max_attempts": 0}))
    token_file = Path(os.environ["HG_PRIVATE_MCP_TOKEN_FILE"])
    if not token_file.is_file() or token_file.stat().st_mode & 0o007:
        raise SystemExit("Private MCP token file must not be world-accessible")
    client = PrivateMcpClient(os.environ["HG_PRIVATE_MCP_URL"], token_file.read_text().strip(),
                              approved_private_ip=os.environ.get("HG_PRIVATE_MCP_APPROVED_IP"))
    worker = Worker(sqs, request_url, reply_url, client)
    stop = [False]
    def shutdown(signum, frame):
        stop[0] = True
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    deadline = time.monotonic() + args.duration_seconds
    while not stop[0] and time.monotonic() < deadline:
        try:
            worker.poll()
        except Exception as exc:
            print("Mailbox worker operation unavailable:", exc.__class__.__name__, flush=True)
            time.sleep(1)


if __name__ == "__main__":
    main()
