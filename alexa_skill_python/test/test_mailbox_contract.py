"""Cross-component mailbox contracts; synthetic queues and native readings only."""
import os
import unittest
from unittest import mock

from test_mailbox_backend import ENV, mb
from test_mailbox_worker import worker_module
from test_skill import backend


class InMemorySqs:
    def __init__(self, result):
        self.replies = []
        self.native = mock.Mock()
        self.native.fetch.return_value = result
        self.worker = worker_module.Worker(self, ENV["HG_ALEXA_REQUEST_QUEUE_URL"], ENV["HG_ALEXA_REPLY_QUEUE_URL"], self.native)

    def send_message(self, QueueUrl, MessageBody):
        message = {"Body": MessageBody, "ReceiptHandle": "synthetic-receipt"}
        if QueueUrl == ENV["HG_ALEXA_REQUEST_QUEUE_URL"]:
            self.worker.handle(message)
        else:
            self.replies.append(message)
        return {"MessageId": "synthetic-message"}

    def receive_message(self, **kwargs):
        replies, self.replies = self.replies, []
        return {"Messages": replies}

    def delete_message(self, **kwargs):
        pass

    def change_message_visibility(self, **kwargs):
        raise AssertionError("An exactly correlated synthetic reply should not be returned visible")


class TestPairedMailbox(unittest.TestCase):
    def test_worker_results_match_consumer_contract(self):
        fixtures = [("get_homeguardian_status", {"state": "Running", "capture_enabled": False, "read_only": True}),
                    ("get_alert_summary", {"alert_count": 0, "highest_severity": "", "read_only": True}),
                    ("get_alert_summary", {"alert_count": 2, "highest_severity": "warning", "read_only": True})]
        for tool, result in fixtures:
            with self.subTest(tool=tool, result=result):
                queues = InMemorySqs(result)
                with mock.patch.dict(os.environ, ENV, clear=True), mock.patch.object(mb, "_make_sqs", return_value=queues):
                    self.assertEqual(mb.get_result(tool), result)
                queues.native.fetch.assert_called_once()

    def test_empty_private_response_does_not_become_cloud_reading(self):
        queues = InMemorySqs(None)
        with mock.patch.dict(os.environ, ENV, clear=True), mock.patch.object(mb, "_make_sqs", return_value=queues):
            self.assertIsNone(mb.get_result("get_homeguardian_status"))

    def test_transport_selection_never_relaxes_loopback_guard(self):
        with mock.patch.dict(os.environ, {"HG_ALEXA_TRANSPORT": "invalid"}, clear=True), mock.patch.object(backend, "_rpc") as local:
            self.assertIsNone(backend.get_homeguardian_status())
            local.assert_not_called()
        # Use the actual module name imported by backend.py, without real AWS calls.
        with mock.patch.dict(os.environ, {"HG_ALEXA_TRANSPORT": "mailbox"}, clear=True), \
                mock.patch.dict("sys.modules", {"mailbox_backend": mb}), \
                mock.patch.object(mb, "get_result", return_value=None) as mailbox, \
                mock.patch.object(backend, "_rpc") as local:
            self.assertIsNone(backend.get_alert_summary("not-transmitted"))
            mailbox.assert_called_once_with("get_alert_summary", "not-transmitted")
            local.assert_not_called()


if __name__ == "__main__":
    unittest.main()
