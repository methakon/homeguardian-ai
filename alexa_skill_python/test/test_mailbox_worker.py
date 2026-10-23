"""Read-only mailbox worker contracts. All MCP/SQS responses here are fixtures."""
import importlib.util
import io
import json
from pathlib import Path
import time
import unittest
from unittest import mock

path = Path(__file__).parents[2] / "deploy/coordinator/alexa_mailbox_worker.py"
spec = importlib.util.spec_from_file_location("alexa_mailbox_worker", path)
assert spec is not None and spec.loader is not None
worker_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker_module)


class Response(io.BytesIO):
    def __init__(self, body, session=None):
        super().__init__(json.dumps(body).encode())
        self.session = session

    def getheader(self, name):
        return self.session if name == "Mcp-Session-Id" else None


class TestWorker(unittest.TestCase):
    def setUp(self):
        self.sqs = mock.Mock()
        self.mcp = mock.Mock()
        self.mcp.fetch.return_value = {"state": "Running", "read_only": True, "capture_enabled": False}
        self.worker = worker_module.Worker(self.sqs, "request-queue-fixture", "reply-queue-fixture", self.mcp, clock=lambda: 100)
        self.body = {"version": 1, "request_id": "a" * 32, "tool": "get_homeguardian_status", "issued_at": 99, "expires_at": 104}

    def message(self, **changes):
        return {"ReceiptHandle": "fixture-receipt", "Body": json.dumps(dict(self.body, **changes))}

    def test_valid_request_releases_only_aggregates(self):
        self.mcp.fetch.return_value.update({"device_id": "must-not-leave", "raw_media": "must-not-leave"})
        self.assertTrue(self.worker.handle(self.message()))
        self.mcp.fetch.assert_called_once_with("get_homeguardian_status", 104)
        call = self.sqs.send_message.call_args.kwargs
        self.assertEqual(call["QueueUrl"], "reply-queue-fixture")
        body = json.loads(call["MessageBody"])
        self.assertEqual(body["request_id"], "a" * 32)
        self.assertEqual(body["result"], {"state": "Running", "capture_enabled": False, "read_only": True})
        self.assertNotIn("must-not-leave", call["MessageBody"])
        self.sqs.delete_message.assert_called_once()

    def test_unavailable_backend_is_null_not_zero_alerts(self):
        self.mcp.fetch.return_value = None
        self.assertTrue(self.worker.handle(self.message(tool="get_alert_summary")))
        result = json.loads(self.sqs.send_message.call_args.kwargs["MessageBody"])
        self.assertIsNone(result["result"])

    def test_alerts_drop_identifiers_and_media(self):
        self.mcp.fetch.return_value = {"alert_count": 2, "highest_severity": "warning", "read_only": True,
                                      "alert_ids": ["must-not-leave"], "audio": "must-not-leave"}
        self.worker.handle(self.message(tool="get_alert_summary"))
        data = json.loads(self.sqs.send_message.call_args.kwargs["MessageBody"])["result"]
        self.assertEqual(set(data), {"alert_count", "highest_severity", "read_only"})

    def test_invalid_requests_never_reach_mcp(self):
        cases = [self.message(version=True), self.message(tool="set_capture_enabled"), self.message(expires_at=99),
                 self.message(request_id="invalid"), self.message(issued_at=103), self.message(expires_at=200),
                 self.message(arguments={}), self.message(url="http://attacker.invalid/mcp"),
                 self.message(issued_at=float("nan")), self.message(issued_at=True),
                 {"Body": "null", "ReceiptHandle": "fixture"}, {"Body": "not-json", "ReceiptHandle": "fixture"}]
        for case in cases:
            with self.subTest(case=case):
                self.assertFalse(self.worker.handle(case))
        self.mcp.fetch.assert_not_called()
        self.sqs.send_message.assert_not_called()

    def test_duplicate_id_is_not_executed_twice(self):
        self.worker.handle(self.message())
        self.assertFalse(self.worker.handle(self.message()))
        self.mcp.fetch.assert_called_once()

    def test_idle_keepalive_stays_private_and_does_not_publish_readings(self):
        now = [100.0]
        worker = worker_module.Worker(self.sqs, "request-queue-fixture", "reply-queue-fixture", self.mcp, clock=lambda: now[0])
        self.sqs.receive_message.return_value = {}
        now[0] = 161.0
        worker.poll()
        self.mcp.fetch.assert_called_once_with("get_homeguardian_status", 164.0)
        self.sqs.send_message.assert_not_called()
        worker.poll()
        self.mcp.fetch.assert_called_once()

    def test_invalid_counts_never_become_readings(self):
        for count in [-1, True, "0", None]:
            with self.subTest(count=count):
                self.assertIsNone(worker_module.minimize("get_alert_summary", {"alert_count": count, "read_only": True}))
        self.assertIsNone(worker_module.minimize("get_homeguardian_status", {"state": "Running", "read_only": False}))


class TestPrivateClient(unittest.TestCase):
    def test_queue_urls_cannot_redirect_worker_credentials(self):
        request = "https://sqs.us-east-1.amazonaws.com/000000000000/homeguardian-alexa-requests"
        reply = "https://sqs.us-east-1.amazonaws.com/000000000000/homeguardian-alexa-replies"
        worker_module.validate_queue_urls("us-east-1", request, reply)
        for url in ["http://sqs.us-east-1.amazonaws.com/000000000000/test", "https://attacker.invalid/queue",
                    reply + "?token=bad", "https://sqs.us-east-1.amazonaws.com/111111111111/homeguardian-alexa-replies", request]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                worker_module.validate_queue_urls("us-east-1", request, url)

    def test_only_the_explicit_private_destination_is_allowed(self):
        bad = ["http://169.254.169.254:8790/mcp", "http://203.0.113.1:8790/mcp", "https://example.invalid/mcp",
               "http://127.0.0.1:8790/other", "http://127.0.0.1:8790/mcp?token=bad", "http://user:pass@127.0.0.1:8790/mcp"]
        for url in bad:
            with self.subTest(url=url), self.assertRaises(worker_module.Unavailable):
                worker_module.PrivateMcpClient(url, "fixture-token")

    def test_http_200_empty_json_is_not_a_successful_mcp_read(self):
        calls = []
        def open_request(req, timeout):
            calls.append(req)
            return Response({}, "fixture-session" if req.method == "POST" else None)
        client = worker_module.PrivateMcpClient("http://127.0.0.1:8790/mcp", "fixture-token", open_request)
        self.assertIsNone(client.fetch("get_homeguardian_status", time.time() + 5))
        self.assertEqual([req.method for req in calls], ["POST", "DELETE"])

    def test_successful_read_initializes_authenticates_and_cleans_up(self):
        calls = []
        def open_request(req, timeout):
            calls.append(req)
            self.assertEqual(req.get_header("Authorization"), "Bearer fixture-token")
            if req.method == "DELETE":
                self.assertEqual(req.get_header("Mcp-session-id"), "fixture-session")
                return Response({"ok": True})
            payload = json.loads(req.data)
            if payload["method"] == "initialize":
                return Response({"jsonrpc": "2.0", "id": 1, "result": {}}, "fixture-session")
            self.assertEqual(req.get_header("Mcp-session-id"), "fixture-session")
            return Response({"jsonrpc": "2.0", "id": 2, "result": {"content": [{"type": "text", "text": json.dumps({
                "state": "Running", "read_only": True, "capture_enabled": False})}]}})
        client = worker_module.PrivateMcpClient("http://127.0.0.1:8790/mcp", "fixture-token", open_request)
        self.assertEqual(client.fetch("get_homeguardian_status", time.time() + 5)["state"], "Running")
        self.assertEqual([req.method for req in calls], ["POST", "POST", "DELETE"])

    def test_timeouts_and_forbidden_tools_fail_closed(self):
        opener = mock.Mock(side_effect=TimeoutError)
        client = worker_module.PrivateMcpClient("http://127.0.0.1:8790/mcp", "fixture-token", opener)
        self.assertIsNone(client.fetch("set_capture_enabled", time.time() + 5))
        opener.assert_not_called()
        self.assertIsNone(client.fetch("get_homeguardian_status", time.time() + 5))
        self.assertIsNone(client.fetch("get_homeguardian_status", time.time() - 1))


if __name__ == "__main__":
    unittest.main()
