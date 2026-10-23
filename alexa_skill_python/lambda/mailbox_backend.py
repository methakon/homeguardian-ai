"""Outbound-only SQS request/reply transport, fail closed without fallback data.

The configured publisher role and both queues must belong to the same account;
its execution-principal trust and least-privilege policy are deployment concerns.
Household identity is deliberately NOT transmitted: this is a single configured
household mailbox, not a multi-tenant authorization mechanism.

A shared standard reply inbox may time out under contention. Other callers'
messages are made visible again, never deleted or returned as this caller's data.
The nominal monotonic operation budget includes STS and is five seconds, not a
hard wall-clock limit: an in-flight SDK call can overrun a boundary by a socket
timeout (and credential-provider/DNS scheduling can introduce further delay).
No long-lived client/credentials or result cache is retained here.
"""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import time
import uuid
from urllib.parse import urlsplit

import boto3
from botocore.config import Config

TOOLS = frozenset(("get_homeguardian_status", "get_alert_summary"))
BUDGET_SECONDS = 5.0
SDK_CONFIG = Config(connect_timeout=0.25, read_timeout=1.25,
                    retries={"total_max_attempts": 1, "mode": "standard"},
                    ignore_configured_endpoint_urls=True)


def _settings():
    """Non-secret deployment settings for managed hosting without custom env access."""
    path = Path(__file__).with_name("mailbox_config.json")
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8") as source:
        raw = source.read(4097)
    if len(raw) > 4096:
        raise ValueError("deployment settings are too large")
    settings = json.loads(raw)
    expected = {"HG_ALEXA_MAILBOX_REGION", "HG_ALEXA_MAILBOX_ROLE_ARN",
                "HG_ALEXA_REQUEST_QUEUE_URL", "HG_ALEXA_REPLY_QUEUE_URL"}
    if (not isinstance(settings, dict) or set(settings) != expected
            or any(not isinstance(value, str) for value in settings.values())):
        raise ValueError("invalid deployment settings")
    return settings


def _configuration(session):
    settings = _settings()
    def value(name):
        return os.environ.get(name, settings.get(name, ""))
    region = value("HG_ALEXA_MAILBOX_REGION")
    # Use the installed SDK's endpoint catalog, not a user-supplied endpoint.
    if region not in session.get_available_regions("sqs", partition_name="aws"):
        raise ValueError("invalid region")
    role = value("HG_ALEXA_MAILBOX_ROLE_ARN")
    match = re.fullmatch(r"arn:aws:iam::([0-9]{12}):role/([A-Za-z0-9_+=,.@/-]{1,512})", role)
    if not match or role.endswith("/") or "//" in role:
        raise ValueError("invalid publisher role")
    account = match.group(1)
    queues = []
    for name in ("HG_ALEXA_REQUEST_QUEUE_URL", "HG_ALEXA_REPLY_QUEUE_URL"):
        raw = value(name)
        parsed = urlsplit(raw)
        if (parsed.scheme != "https" or parsed.netloc != "sqs." + region + ".amazonaws.com"
                or parsed.query or parsed.fragment
                or not re.fullmatch(r"/" + account + r"/[A-Za-z0-9_-]{1,80}", parsed.path)
                or raw != "https://" + parsed.netloc + parsed.path):
            raise ValueError("invalid queue configuration")
        queues.append(raw)
    if queues[0] == queues[1]:
        raise ValueError("queues must differ")
    return region, role, queues[0], queues[1]


def _remaining(deadline):
    if time.monotonic() >= deadline:
        raise TimeoutError("mailbox budget exhausted")


def _make_sqs(session, region, role, deadline):
    _remaining(deadline)
    sts = session.client("sts", region_name=region, config=SDK_CONFIG)
    _remaining(deadline)
    credentials = sts.assume_role(RoleArn=role,
        RoleSessionName="homeguardian-alexa-mailbox", DurationSeconds=900)["Credentials"]
    _remaining(deadline)
    assumed = boto3.Session(aws_access_key_id=credentials["AccessKeyId"],
        aws_secret_access_key=credentials["SecretAccessKey"],
        aws_session_token=credentials["SessionToken"], region_name=region)
    return assumed.client("sqs", region_name=region, config=SDK_CONFIG)


def _result(tool, value):
    if not isinstance(value, dict) or value.get("read_only") is not True:
        return None
    if tool == "get_homeguardian_status":
        if (set(value) != {"state", "capture_enabled", "read_only"}
                or not isinstance(value["state"], str) or not value["state"]
                or len(value["state"]) > 64
                or not re.fullmatch(r"[a-zA-Z0-9_ /-]+", value["state"])
                or type(value["capture_enabled"]) is not bool):
            return None
    else:
        if (set(value) != {"alert_count", "highest_severity", "read_only"}
                or type(value["alert_count"]) is not int or value["alert_count"] < 0
                or (value["highest_severity"] is not None
                    and (not isinstance(value["highest_severity"], str)
                         or not re.fullmatch(r"[a-zA-Z0-9_ /-]{0,64}", value["highest_severity"])))):
            return None
    return value.copy()


def _parse(body):
    if not isinstance(body, str) or len(body) > 8192:
        return None
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result
    value = json.loads(body, object_pairs_hook=pairs)
    return value if isinstance(value, dict) else None


def get_result(tool: str, household_id: str | None = None) -> dict | None:
    """Return only a validated correlated read-only result, or None on ANY failure.

    household_id is accepted for backend API compatibility and is never serialized.
    No exception text, credentials, identity, or message body is logged.
    """
    try:
        if tool not in TOOLS:
            return None
        deadline = time.monotonic() + BUDGET_SECONDS
        issued_at = time.time()
        expires_at = issued_at + BUDGET_SECONDS
        session = boto3.Session()
        region, role, requests, replies = _configuration(session)
        sqs = _make_sqs(session, region, role, deadline)
        request_id = uuid.uuid4().hex
        request = {"version": 1, "request_id": request_id, "tool": tool,
                   "issued_at": issued_at, "expires_at": expires_at}
        _remaining(deadline)
        sqs.send_message(QueueUrl=requests, MessageBody=json.dumps(request))
        while True:
            _remaining(deadline)
            messages = sqs.receive_message(QueueUrl=replies, WaitTimeSeconds=1,
                MaxNumberOfMessages=4, VisibilityTimeout=2).get("Messages", [])
            answer = None
            matched = False
            for message in messages:
                _remaining(deadline)
                receipt = message.get("ReceiptHandle")
                if not isinstance(receipt, str) or not receipt:
                    continue
                try:
                    reply = _parse(message.get("Body"))
                except (ValueError, TypeError, RecursionError):
                    reply = None
                correlated = (isinstance(reply, dict)
                    and type(reply.get("version")) is int and reply["version"] == 1
                    and reply.get("request_id") == request_id and reply.get("tool") == tool
                    and type(reply.get("expires_at")) in (int, float)
                    and math.isfinite(reply["expires_at"])
                    and reply["expires_at"] == expires_at and time.time() < expires_at)
                _remaining(deadline)
                if correlated:
                    # Only exact, live correlation gives ownership of a reply.
                    assert isinstance(reply, dict)
                    value = _result(tool, reply.get("result"))
                    sqs.delete_message(QueueUrl=replies, ReceiptHandle=receipt)
                    if not matched:
                        answer, matched = value, True
                else:
                    sqs.change_message_visibility(QueueUrl=replies,
                        ReceiptHandle=receipt, VisibilityTimeout=0)
            if matched:
                _remaining(deadline)
                return answer
    except Exception:
        # Deliberately no logging: SDK errors may include credentials or bodies.
        return None
