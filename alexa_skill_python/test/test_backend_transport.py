"""MCP transport contracts with explicit synthetic HTTP responses; no household data."""
import io
import json
import os
import socket
import unittest
from unittest import mock

from test_skill import backend


class Response(io.BytesIO):
    def __init__(self, data, session=None):
        super().__init__(json.dumps(data).encode())
        self.headers = {"Mcp-Session-Id": session} if session else {}

    def getheader(self, name, default=None):
        return self.headers.get(name, default)


def tool_result(data):
    return {"jsonrpc": "2.0", "id": 2, "result": {"content": [{"type": "text", "text": json.dumps(data)}]}}


class TestTransport(unittest.TestCase):
    def setUp(self):
        self.env = mock.patch.dict(os.environ, {"HG_MCP_HOST": "127.0.0.1", "HG_MCP_PORT": "8790", "HG_MCP_TOKEN": "local-test-token"}, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)

    def test_initialize_call_and_delete_share_authenticated_session(self):
        requests = []
        def open_request(request, timeout):
            requests.append(request)
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 3)
            self.assertEqual(request.get_header("Authorization"), "Bearer local-test-token")
            if request.method == "DELETE":
                self.assertEqual(request.get_header("Mcp-session-id"), "test-session")
                return Response({"ok": True})
            body = json.loads(request.data)
            if body["method"] == "initialize":
                return Response({"jsonrpc": "2.0", "id": 1, "result": {"protocolVersion": "2025-11-25"}}, "test-session")
            self.assertEqual(request.get_header("Mcp-session-id"), "test-session")
            self.assertEqual(body["params"]["name"], backend.TOOL_STATUS)
            return Response(tool_result({"state": "Running", "read_only": True}))
        with mock.patch.object(backend, "_open", side_effect=open_request):
            self.assertEqual(backend.get_homeguardian_status(), {"state": "Running", "read_only": True})
        self.assertEqual([r.method for r in requests], ["POST", "POST", "DELETE"])

    def test_bad_configuration_is_unavailable_without_network(self):
        with mock.patch.object(backend, "_open") as opener:
            for value in ["bad", "0", "-1", "65536", "nan"]:
                with self.subTest(port=value), mock.patch.dict(os.environ, {"HG_MCP_PORT": value}):
                    self.assertIsNone(backend.get_homeguardian_status())
            opener.assert_not_called()

    def test_timeout_is_unavailable_not_an_all_clear(self):
        with mock.patch.object(backend, "_open", side_effect=socket.timeout):
            self.assertIsNone(backend.get_homeguardian_status())
            self.assertIsNone(backend.get_alert_summary())

    def test_oversized_tool_response_is_rejected(self):
        data = tool_result({"state": "Running"})
        data["padding"] = "x" * (70 * 1024)
        responses = [Response({"jsonrpc": "2.0", "id": 1, "result": {}}, "test-session"), Response(data), Response({"ok": True})]
        with mock.patch.object(backend, "_open", side_effect=responses):
            self.assertIsNone(backend.get_homeguardian_status())


if __name__ == "__main__":
    unittest.main()
