"""Offline fixtures only: real ASK SDK, no AWS or deployed portal acceptance."""
import io
import json
import os
from pathlib import Path
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from test_mailbox_worker import worker_module as wm
from test_mailbox_backend import mb
from test_lambda_contract import envelope, speech
from test_skill import backend, lf

VIDEO = {
    "id": "12345678-abcd-1234-abcd-123456789abc",
    "event_id": "fixture_event-1",
    "summary": "Movement <near> door & window",
    "severity": "warning",
    "detected_at": "2026-10-24T12:34:56.123Z",
    "portal_url": "https://homeguardian.berhampore.in/?event_video=12345678-abcd-1234-abcd-123456789abc#event-videos",
}
NATIVE = {"alert_count": 2, "highest_severity": "critical", "read_only": True}


class ValidationTests(unittest.TestCase):
    def test_both_boundaries_accept_identical_minimal_metadata(self):
        self.assertEqual(wm.validate_latest_video(VIDEO), VIDEO)
        self.assertEqual(mb.validate_latest_video(VIDEO), VIDEO)

    def test_both_boundaries_reject_malicious_or_malformed_fields(self):
        bad_urls = [
            VIDEO["portal_url"].replace("https:", "http:"),
            VIDEO["portal_url"].replace(".in/", ".in:443/"),
            VIDEO["portal_url"].replace("homeguardian.berhampore.in", "127.0.0.1"),
            VIDEO["portal_url"].replace("homeguardian.berhampore.in", "homeguardian.berhampore.in.evil.invalid"),
            VIDEO["portal_url"].replace("https://", "https://user:pass@"),
            VIDEO["portal_url"].replace("/?", "/other?"),
            VIDEO["portal_url"].replace("#event-videos", "&token=secret#event-videos"),
            VIDEO["portal_url"].replace("#event-videos", "#other"),
            VIDEO["portal_url"].replace("12345678", "12345679"),
            VIDEO["portal_url"] + "\n",
        ]
        cases = [dict(VIDEO, portal_url=url) for url in bad_urls]
        cases += [None, {}, dict(VIDEO, raw_media="private"),
                  dict(VIDEO, id=VIDEO["id"].upper()), dict(VIDEO, event_id="../bad"),
                  dict(VIDEO, summary=""), dict(VIDEO, summary="x" * 501),
                  dict(VIDEO, summary="bad\x00text"), dict(VIDEO, summary="bad\u202etext"),
                  dict(VIDEO, severity="emergency"), dict(VIDEO, severity=True),
                  dict(VIDEO, detected_at="2026-02-30T12:00:00Z"),
                  dict(VIDEO, detected_at="2026-10-24"), dict(VIDEO, detected_at=123)]
        for value in cases:
            for validator in (wm.validate_latest_video, mb.validate_latest_video):
                with self.subTest(value=value, validator=validator.__module__):
                    self.assertIsNone(validator(value))

    def test_optional_invalid_metadata_does_not_discard_native_aggregate(self):
        for value in (None, {}, dict(VIDEO, portal_url="https://evil.invalid")):
            self.assertEqual(mb._result("get_alert_summary", dict(NATIVE, latest_video=value)), NATIVE)
        self.assertEqual(mb._result("get_alert_summary", NATIVE), NATIVE)
        zero = dict(NATIVE, alert_count=0)
        self.assertEqual(mb._result("get_alert_summary", dict(zero, latest_video=VIDEO)), zero)
        self.assertIsNone(mb._result("get_alert_summary", {"latest_video": VIDEO}))


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"])
        self.addCleanup(self.temp.cleanup)
        self.token = Path(self.temp.name) / "read-token"
        self.token.write_bytes(b"a" * 64 + b"\n")
        self.token.chmod(0o600)
        self.open = mock.Mock(side_effect=lambda *a, **kw: io.BytesIO(json.dumps({"ok": True, "event": VIDEO}).encode()))
        self.client = wm.PortalNotificationClient(self.token, opener=self.open, clock=lambda: 100)

    def test_fixed_authenticated_get_and_deadline_bound(self):
        self.assertEqual(self.client.fetch(100.2), VIDEO)
        req = self.open.call_args.args[0]
        self.assertEqual(req.full_url, wm.NOTIFICATION_URL)
        self.assertEqual(req.method, "GET")
        self.assertIsNone(req.data)
        self.assertEqual(req.get_header("Authorization"), "Bearer " + "a" * 64)
        self.assertLessEqual(self.open.call_args.kwargs["timeout"], 0.2 + 1e-10)
        self.client.fetch(105)
        self.assertEqual(self.open.call_args.kwargs["timeout"], 0.5)
        self.open.reset_mock()
        self.assertIsNone(self.client.fetch(100))
        self.open.assert_not_called()

    def test_insecure_missing_symlink_and_bad_token_files_fail_optional_closed(self):
        for mode in (0o644, 0o640, 0o400, 0o700):
            self.token.chmod(mode)
            self.assertIsNone(self.client.fetch(101))
        self.token.chmod(0o600)
        for token in (b"A" * 64, b"a" * 63, b"a" * 257, b"a" * 32 + b"\n" + b"a" * 32):
            self.token.write_bytes(token)
            self.assertIsNone(self.client.fetch(101))
        self.token.unlink()
        self.assertIsNone(self.client.fetch(101))
        target = Path(self.temp.name) / "real-token"
        target.write_bytes(b"a" * 64)
        target.chmod(0o600)
        self.token.symlink_to(target)
        self.assertIsNone(self.client.fetch(101))
        self.token.unlink()
        os.mkfifo(self.token, 0o600)
        self.assertIsNone(self.client.fetch(101))
        self.open.assert_not_called()

    def test_wrong_owner_rejected(self):
        with mock.patch.object(wm.os, "geteuid", return_value=os.geteuid() + 1):
            self.assertIsNone(self.client.fetch(101))
        self.open.assert_not_called()

    def test_invalid_oversize_duplicate_and_unavailable_responses(self):
        bodies = [b"x" * 16385, b"not-json", b'{"ok":true,"ok":true,"event":null}',
                  b'{"ok":true,"event":null}', b'{"ok":false,"event":{}}',
                  json.dumps({"ok": True, "event": VIDEO, "extra": "private"}).encode()]
        for body in bodies:
            self.open.side_effect = lambda *a, **kw: io.BytesIO(body)
            self.assertIsNone(self.client.fetch(101))
        self.open.side_effect = TimeoutError("must not log")
        with mock.patch("builtins.print") as printer:
            self.assertIsNone(self.client.fetch(101))
            printer.assert_not_called()

    def test_real_urllib_rejects_redirect_and_ignores_proxy(self):
        seen = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.path)
                self.send_response(302)
                self.send_header("Location", "/must-not-follow")
                self.end_headers()
            def log_message(self, format, *args):
                pass
        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with mock.patch.object(wm, "NOTIFICATION_URL", "http://127.0.0.1:%d/notification" % server.server_port), \
                    mock.patch.dict(os.environ, {"http_proxy": "http://127.0.0.1:1", "no_proxy": ""}):
                client = wm.PortalNotificationClient(self.token, clock=lambda: 100)
                self.assertIsNone(client.fetch(101))
            self.assertEqual(seen, ["/notification"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


class WorkerAndAskTests(unittest.TestCase):
    def run_worker(self, native, portal_value=VIDEO, portal_error=None):
        sqs, mcp, portal = mock.Mock(), mock.Mock(), mock.Mock()
        mcp.fetch.return_value = native
        portal.fetch.return_value = portal_value
        portal.fetch.side_effect = portal_error
        worker = wm.Worker(sqs, "fixture-request", "fixture-reply", mcp, clock=lambda: 100, portal=portal)
        request = {"version": 1, "request_id": "a" * 32, "tool": "get_alert_summary", "issued_at": 99, "expires_at": 104}
        worker.handle({"ReceiptHandle": "fixture", "Body": json.dumps(request)})
        return json.loads(sqs.send_message.call_args.kwargs["MessageBody"])["result"], portal

    def test_worker_to_actual_ask_response_preserves_native_provenance(self):
        result, portal = self.run_worker(NATIVE)
        self.assertEqual(result, dict(NATIVE, latest_video=VIDEO))
        portal.fetch.assert_called_once_with(104)
        validated = mb._result("get_alert_summary", result)
        for intent in ("RecentAlertsIntent", "GetAlertSummaryIntent"):
            with mock.patch.object(backend, "get_alert_summary", return_value=validated):
                response = lf.lambda_handler(envelope("IntentRequest", intent), None)
            text = speech(response)
            ET.fromstring(text)
            self.assertIn("2 alerts", text)
            self.assertIn("highest severity is critical", text)
            self.assertIn("Latest recorded event:", text)
            self.assertIn("&lt;near&gt;", text)
            card = response["response"]["card"]
            self.assertEqual(card["type"], "Simple")
            self.assertEqual(card["title"], "Latest recorded event")
            self.assertIn(VIDEO["summary"], card["content"])
            self.assertIn(VIDEO["portal_url"], card["content"])
            self.assertNotIn(VIDEO["portal_url"], text)
            self.assertNotIn("live", text)

    def test_zero_or_unavailable_never_queries_portal_or_invents_alerts(self):
        for native in (None, dict(NATIVE, alert_count=0), dict(NATIVE, read_only=False)):
            result, portal = self.run_worker(native)
            portal.fetch.assert_not_called()
            with mock.patch.object(backend, "get_alert_summary", return_value=result):
                response = lf.lambda_handler(envelope("IntentRequest", "RecentAlertsIntent"), None)
            self.assertNotIn("card", response["response"])
            self.assertNotIn("Latest recorded event", speech(response))

    def test_old_reply_invalid_optional_and_reader_exception_preserve_speech(self):
        for metadata in (None, {}, dict(VIDEO, portal_url="https://evil.invalid")):
            result, _ = self.run_worker(NATIVE, portal_value=metadata)
            self.assertEqual(result, NATIVE)
        result, _ = self.run_worker(NATIVE, portal_error=RuntimeError("private"))
        self.assertEqual(result, NATIVE)
        for summary in (NATIVE, dict(NATIVE, latest_video={}), dict(NATIVE, latest_video=dict(VIDEO, portal_url="https://evil.invalid"))):
            with mock.patch.object(backend, "get_alert_summary", return_value=summary):
                response = lf.lambda_handler(envelope("IntentRequest", "RecentAlertsIntent"), None)
            self.assertNotIn("card", response["response"])
            self.assertEqual(speech(response), "<speak>" + lf._alerts_speech(NATIVE) + "</speak>")

    def test_real_mailbox_consumer_retains_correlated_optional_metadata(self):
        from test_mailbox_backend import MailboxTests
        fixture = MailboxTests()
        fixture.setUp()
        try:
            result, _ = self.run_worker(NATIVE)
            fixture.responses = [[fixture.reply(result=result, tool="get_alert_summary")]]
            self.assertEqual(fixture.run_call(tool="get_alert_summary"), result)
        finally:
            fixture.doCleanups()

    def test_card_requires_valid_positive_native_aggregate(self):
        for changes in ({"alert_count": 0}, {"alert_count": True}, {"read_only": False}):
            summary = dict(NATIVE, latest_video=VIDEO, **changes)
            with mock.patch.object(backend, "get_alert_summary", return_value=summary):
                response = lf.lambda_handler(envelope("IntentRequest", "RecentAlertsIntent"), None)
            self.assertNotIn("card", response["response"])

    def test_spoken_summary_is_concise_but_card_retains_full_summary(self):
        video = dict(VIDEO, summary="&" * 500)
        with mock.patch.object(backend, "get_alert_summary", return_value=dict(NATIVE, latest_video=video)):
            response = lf.lambda_handler(envelope("IntentRequest", "RecentAlertsIntent"), None)
        parsed = ET.fromstring(speech(response))
        self.assertIn("&" * 197 + "...", parsed.text or "")
        self.assertNotIn("&" * 198, parsed.text or "")
        self.assertIn("&" * 500, response["response"]["card"]["content"])


if __name__ == "__main__":
    unittest.main()
