"""Offline mailbox protocol tests; all AWS clients are doubles, never networked."""
import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / "lambda" / "mailbox_backend.py"
SPEC = importlib.util.spec_from_file_location("hg_mailbox_backend", PATH)
assert SPEC is not None and SPEC.loader is not None
mb = importlib.util.module_from_spec(SPEC)
if PATH.exists():
    SPEC.loader.exec_module(mb)

# Obviously synthetic account/queue names, but valid AWS URL structure.
ENV = {
    "HG_ALEXA_MAILBOX_REGION": "us-east-1",
    "HG_ALEXA_REQUEST_QUEUE_URL": "https://sqs.us-east-1.amazonaws.com/000000000000/synthetic-requests",
    "HG_ALEXA_REPLY_QUEUE_URL": "https://sqs.us-east-1.amazonaws.com/000000000000/synthetic-replies",
    "HG_ALEXA_MAILBOX_ROLE_ARN": "arn:aws:iam::000000000000:role/synthetic-publisher",
}
STATUS = {"state": "ready", "capture_enabled": False, "read_only": True}


class MailboxTests(unittest.TestCase):
    def setUp(self):
        self.env = mock.patch.dict("os.environ", ENV, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.now = 1000.0
        self.clock = 0.0
        self.sqs = mock.Mock()
        self.request = None
        self.sqs.send_message.side_effect = self.send
        self.sqs.receive_message.side_effect = self.receive
        self.responses = []
        self.sts = mock.Mock()
        self.sts.assume_role.return_value = {"Credentials": {
            "AccessKeyId": "SYNTHETIC_KEY", "SecretAccessKey": "SYNTHETIC_SECRET",
            "SessionToken": "SYNTHETIC_SESSION"}}
        self.session = mock.Mock()
        self.assumed = mock.Mock()
        self.session.get_available_regions.return_value = ["us-east-1"]
        self.session.client.return_value = self.sts
        self.assumed.client.return_value = self.sqs

    def send(self, **kwargs):
        self.request = json.loads(kwargs["MessageBody"])
        return {"MessageId": "synthetic"}

    def receive(self, **kwargs):
        self.clock += 1.0
        return {"Messages": self.responses.pop(0) if self.responses else []}

    def reply(self, result=STATUS, **changes):
        body = {"version": 1, "request_id": "a" * 32,
                "tool": "get_homeguardian_status", "expires_at": 1005.0,
                "result": result}
        body.update(changes)
        return {"Body": json.dumps(body), "ReceiptHandle": "synthetic-receipt"}

    def run_call(self, tool="get_homeguardian_status", household_id=None):
        with mock.patch.object(mb.boto3, "Session", side_effect=[self.session, self.assumed]), \
             mock.patch.object(mb.time, "time", side_effect=lambda: self.now + self.clock), \
             mock.patch.object(mb.time, "monotonic", side_effect=lambda: self.clock), \
             mock.patch.object(mb.uuid, "uuid4") as uid:
            uid.return_value.hex = "a" * 32
            return mb.get_result(tool, household_id)

    def test_valid_status_and_private_request(self):
        self.assertTrue(hasattr(mb, "get_result"), "mailbox client is missing")
        self.responses = [[self.reply()]]
        self.assertEqual(self.run_call(household_id="PRIVATE_HOUSEHOLD"), STATUS)
        self.assertEqual(self.request, {"version": 1, "request_id": "a" * 32,
                         "tool": "get_homeguardian_status", "issued_at": 1000.0,
                         "expires_at": 1005.0})
        self.sqs.delete_message.assert_called_once_with(
            QueueUrl=ENV["HG_ALEXA_REPLY_QUEUE_URL"], ReceiptHandle="synthetic-receipt")
        self.sts.assume_role.assert_called_once_with(
            RoleArn=ENV["HG_ALEXA_MAILBOX_ROLE_ARN"], RoleSessionName="homeguardian-alexa-mailbox",
            DurationSeconds=900)
        cfg = self.assumed.client.call_args.kwargs["config"]
        self.assertEqual(cfg.retries["total_max_attempts"], 1)
        self.assertLessEqual(cfg.connect_timeout, 0.5)
        self.assertLessEqual(cfg.read_timeout, 1.5)
        for call in self.sqs.receive_message.call_args_list:
            self.assertEqual(call.kwargs["WaitTimeSeconds"], 1)
            self.assertLessEqual(call.kwargs["MaxNumberOfMessages"], 4)

    def test_valid_alert_summary_native_warning(self):
        result = {"alert_count": 2, "highest_severity": "warning", "read_only": True}
        self.responses = [[self.reply(result, tool="get_alert_summary")]]
        self.assertEqual(self.run_call("get_alert_summary"), result)

    def test_native_zero_alert_summary_preserves_empty_severity(self):
        result = {"alert_count": 0, "highest_severity": "", "read_only": True}
        self.responses = [[self.reply(result, tool="get_alert_summary")]]
        self.assertEqual(self.run_call("get_alert_summary"), result)

    def test_no_configuration_never_uses_aws(self):
        with mock.patch.dict("os.environ", {}, clear=True):
            self.assertIsNone(self.run_call())
        self.session.client.assert_not_called()

    def test_private_deployment_settings_support_managed_hosting(self):
        self.responses = [[self.reply()]]
        with mock.patch.dict("os.environ", {}, clear=True), mock.patch.object(mb, "_settings", return_value=ENV):
            self.assertEqual(self.run_call(), STATUS)

    def test_dotenv_reads_only_allowlisted_settings(self):
        data = "\n".join(key + "=" + value for key, value in ENV.items())
        data += "\nAWS_SECRET_ACCESS_KEY=SYNTHETIC_DO_NOT_LOAD\n"
        with mock.patch.object(mb.Path, "is_file", return_value=True), \
                mock.patch.object(mb.Path, "open", mock.mock_open(read_data=data)):
            self.assertEqual(mb._settings(), ENV)
        for raw in ["null", "[]", "{}", "x" * 65537, "HG_ALEXA_MAILBOX_REGION=one\nHG_ALEXA_MAILBOX_REGION=two"]:
            with self.subTest(size=len(raw)), mock.patch.object(mb.Path, "is_file", return_value=True), \
                    mock.patch.object(mb.Path, "open", mock.mock_open(read_data=raw)), self.assertRaises(ValueError):
                mb._settings()

    def test_malformed_configuration_never_uses_aws(self):
        cases = [("HG_ALEXA_MAILBOX_REGION", "evil.invalid"),
                 ("HG_ALEXA_MAILBOX_ROLE_ARN", "arn:aws:iam::*:role/publisher"),
                 ("HG_ALEXA_MAILBOX_ROLE_ARN", "arn:aws:iam::111111111111:role/publisher"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "http://sqs.us-east-1.amazonaws.com/000000000000/queue"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "https://sqs.us-west-2.amazonaws.com/000000000000/queue"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "https://sqs.us-east-1.amazonaws.com.evil.invalid/000000000000/queue"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "https://sqs.us-east-1.amazonaws.com:443/000000000000/queue"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "https://sqs.us-east-1.amazonaws.com/000000000000/queue?endpoint=x"),
                 ("HG_ALEXA_REQUEST_QUEUE_URL", "https://sqs.us-east-1.amazonaws.com/000000000000/queue.fifo"),
                 ("HG_ALEXA_REPLY_QUEUE_URL", ENV["HG_ALEXA_REQUEST_QUEUE_URL"])]
        for key, value in cases:
            with self.subTest(key=key, value=value), mock.patch.dict("os.environ", {key: value}):
                self.assertIsNone(self.run_call())
        self.session.client.assert_not_called()

    def test_dotenv_quotes_export_and_transport_precedence(self):
        data = '# selected application settings only\nexport HG_ALEXA_MAILBOX_REGION="us-east-1"\nHG_ALEXA_TRANSPORT=mailbox # approved mode\nUNRELATED_SECRET=SYNTHETIC_DO_NOT_LOAD\n'
        with mock.patch.object(mb.Path, "is_file", return_value=True), \
                mock.patch.object(mb.Path, "open", mock.mock_open(read_data=data)):
            settings = mb._settings()
            self.assertEqual(settings, {"HG_ALEXA_MAILBOX_REGION": "us-east-1", "HG_ALEXA_TRANSPORT": "mailbox"})
            self.assertEqual(mb.configured_transport(), "mailbox")
        with mock.patch.dict("os.environ", {"HG_ALEXA_TRANSPORT": "loopback"}), mock.patch.object(mb, "_settings") as loader:
            self.assertEqual(mb.configured_transport(), "loopback")
            loader.assert_not_called()

    def test_forbidden_tool_never_uses_aws(self):
        for tool in ("start_capture", "get_raw_media", "", None, []):
            with self.subTest(tool=tool):
                self.assertIsNone(self.run_call(tool))
        self.session.client.assert_not_called()

    def test_timeout_has_five_second_nominal_budget(self):
        self.assertIsNone(self.run_call())
        self.assertEqual(self.clock, 5.0)
        self.assertEqual(self.sqs.receive_message.call_count, 5)
        self.sqs.delete_message.assert_not_called()

    def test_api_failures_and_no_sensitive_logging(self):
        for method in (self.sts.assume_role, self.sqs.send_message,
                       self.sqs.receive_message, self.sqs.delete_message,
                       self.sqs.change_message_visibility):
            with self.subTest(method=method):
                self.clock = 0.0
                self.responses = [[self.reply(request_id="b" * 32) if method is self.sqs.change_message_visibility
                                   else self.reply()]]
                method.side_effect = RuntimeError("SYNTHETIC_SECRET PRIVATE_BODY PRIVATE_HOUSEHOLD")
                with mock.patch("sys.stdout") as out, mock.patch("sys.stderr") as err, \
                     mock.patch("logging.Logger._log") as log:
                    self.assertIsNone(self.run_call(household_id="PRIVATE_HOUSEHOLD"))
                    out.write.assert_not_called()
                    err.write.assert_not_called()
                    log.assert_not_called()
                method.side_effect = (self.send if method is self.sqs.send_message else
                                      self.receive if method is self.sqs.receive_message else None)

    def test_wrong_correlation_returned_visible_never_consumed(self):
        wrong_id = dict(self.reply(request_id="b" * 32), ReceiptHandle="other-id")
        wrong_tool = dict(self.reply(tool="get_alert_summary"), ReceiptHandle="other-tool")
        self.responses = [[wrong_id, wrong_tool], [self.reply()]]
        self.assertEqual(self.run_call(), STATUS)
        self.assertEqual(self.sqs.change_message_visibility.call_count, 2)
        for call in self.sqs.change_message_visibility.call_args_list:
            self.assertEqual(call.kwargs["VisibilityTimeout"], 0)
        self.sqs.delete_message.assert_called_once_with(
            QueueUrl=ENV["HG_ALEXA_REPLY_QUEUE_URL"], ReceiptHandle="synthetic-receipt")

    def test_expired_or_wrong_deadline_reply_never_deleted(self):
        for deadline in (999.0, 1004.0, 1006.0, True, "1005.0", float("nan"), float("inf")):
            with self.subTest(deadline=deadline):
                self.clock = 0.0
                self.sqs.reset_mock()
                self.responses = [[self.reply(expires_at=deadline)]]
                self.assertIsNone(self.run_call())
                self.sqs.delete_message.assert_not_called()
                self.sqs.change_message_visibility.assert_called_once()

    def test_wall_clock_expired_even_before_monotonic_budget(self):
        def wall_clock_jump(**kwargs):
            self.now = 1010.0
            self.clock += 1.0
            return {"Messages": [self.reply()]} if self.clock == 1.0 else {}
        self.sqs.receive_message.side_effect = wall_clock_jump
        self.assertIsNone(self.run_call())
        self.sqs.delete_message.assert_not_called()
        self.sqs.change_message_visibility.assert_called_once()

    def test_malformed_json_never_deleted(self):
        for body in ("PRIVATE_BODY{", "[]", "null", "{\"version\":1,\"version\":1}", "x" * 8193, None):
            with self.subTest(body_type=type(body)):
                self.clock = 0.0
                self.sqs.reset_mock()
                self.responses = [[{"Body": body, "ReceiptHandle": "synthetic"}]]
                self.assertIsNone(self.run_call())
                self.sqs.delete_message.assert_not_called()
                self.sqs.change_message_visibility.assert_called_once()

    def test_invalid_status_result_is_unavailable(self):
        for value in (None, [], {}, {"state": "ready", "capture_enabled": False},
                      dict(STATUS, read_only=False), dict(STATUS, read_only=1),
                      dict(STATUS, state=3), dict(STATUS, capture_enabled="false"),
                      dict(STATUS, raw_media="PRIVATE_BODY"), dict(STATUS, alert_ids=["private"])):
            with self.subTest(value=value):
                self.clock = 0.0
                self.responses = [[self.reply(value)]]
                self.assertIsNone(self.run_call())

    def test_invalid_alert_results_are_unavailable(self):
        valid = {"alert_count": 0, "highest_severity": None, "read_only": True}
        invalid = [dict(valid, alert_count=count) for count in (-1, True, 1.5, "2", None)]
        invalid += [dict(valid, highest_severity=x) for x in (True, 4, [], {})]
        invalid += [dict(valid, read_only=False), dict(valid, alert_ids=["private"])]
        for value in invalid:
            with self.subTest(value=value):
                self.clock = 0.0
                self.responses = [[self.reply(value, tool="get_alert_summary")]]
                self.assertIsNone(self.run_call("get_alert_summary"))
        self.clock = 0.0
        self.responses = [[self.reply(valid, tool="get_alert_summary")]]
        self.assertEqual(self.run_call("get_alert_summary"), valid)

    def test_batch_returns_other_messages_visible_even_after_match(self):
        self.responses = [[self.reply(), self.reply(request_id="b" * 32)]]
        self.assertEqual(self.run_call(), STATUS)
        self.sqs.change_message_visibility.assert_called_once()
        self.sqs.delete_message.assert_called_once()

    def test_slow_auth_prevents_sending_after_budget(self):
        def slow_auth(**kwargs):
            self.clock = 5.1
            return {"Credentials": {"AccessKeyId": "x", "SecretAccessKey": "y", "SessionToken": "z"}}
        self.sts.assume_role.side_effect = slow_auth
        self.assertIsNone(self.run_call())
        self.sqs.send_message.assert_not_called()


if __name__ == "__main__":
    unittest.main()
