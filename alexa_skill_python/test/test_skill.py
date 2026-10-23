"""Tests for the HomeGuardian Alexa Python skill.

WHAT THESE TESTS ARE FOR
The skill's most important behaviour is not answering — it is REFUSING to answer. Every other test here
is in service of that one: the unavailable path is asserted first and hardest, because a voice
interface that improvises a household reading is indistinguishable, to the person listening, from one
that read it from the hub.

Run:  cd alexa_skill_python && python -m pytest test/ -q
      (or: python test/test_handler.py  — the file is also runnable without pytest)

DESIGN NOTE ON HOW THE HUB IS FAKED
The backend is patched at the `lambda_function` module boundary, not at the socket. That keeps these
tests about RENDERING and DISPATCH — given the hub said X, what does the user hear — and leaves the
transport's own behaviour to backend-level tests, which assert on real request construction.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import re
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
LAMBDA_DIR = os.path.join(os.path.dirname(HERE), "lambda")
sys.path.insert(0, LAMBDA_DIR)


def _load_lambda_function():
    """Import lambda_function.py, whose name is not a valid identifier for a plain import."""
    spec = importlib.util.spec_from_file_location(
        "hg_lambda_function", os.path.join(LAMBDA_DIR, "lambda_function.py")
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules["hg_lambda_function"] = module
    spec.loader.exec_module(module)
    return module


lf = _load_lambda_function()
backend = sys.modules["backend"]


# ------------------------------------------------------------------------------------------------
# Minimal envelope doubles. Building real ask_sdk_model objects would couple these tests to the SDK's
# constructors rather than to the behaviour under test.
# ------------------------------------------------------------------------------------------------


# ------------------------------------------------------------------------------------------------
# Request doubles.
#
# These use the REAL ask_sdk_model classes, and that is deliberate rather than convenient. The SDK's
# dispatch predicates are isinstance-based:
#
#     ask_sdk_core/utils/predicate.py
#       is_intent_name:  isinstance(request, IntentRequest) and request.intent.name == name
#       is_request_type: request.object_type == request_type
#
# An earlier version of this file duck-typed the requests, so every IntentRequest predicate returned
# False and the dispatch test failed for a reason that had nothing to do with dispatch. A stand-in that
# does not satisfy the real contract cannot test code that runs against it.
# ------------------------------------------------------------------------------------------------

from ask_sdk_core.response_helper import ResponseFactory
from ask_sdk_model.intent import Intent
from ask_sdk_model.intent_request import IntentRequest
from ask_sdk_model.launch_request import LaunchRequest
from ask_sdk_model.session_ended_request import SessionEndedRequest


class _User:
    def __init__(self, user_id="amzn1.ask.account.TESTHOUSEHOLD"):
        self.user_id = user_id


class _System:
    def __init__(self):
        self.user = _User()


class _Context:
    def __init__(self):
        self.system = _System()


class _Envelope:
    def __init__(self, request):
        self.request = request
        self.context = _Context()


def _intent_request(name):
    return IntentRequest(intent=Intent(name=name))


class FakeHandlerInput:
    """A HandlerInput whose response builder is the REAL one.

    This used to be a hand-written recorder that implemented `.speak()` and `.reprompt()`. The SDK's
    ResponseFactory has NO `.reprompt()` — that is the JavaScript ASK SDK's method. So the double was
    MORE PERMISSIVE THAN REALITY, every test passed, and the deployed skill raised
    `AttributeError: 'ResponseFactory' object has no attribute 'reprompt'` on exactly the two handlers
    that reprompt (LaunchRequest and HelpIntent) — the identical-failure symptom the owner reported.

    Using the real ResponseFactory means the tests exercise the real API surface and cannot invent a
    method the runtime lacks. Responses are dicts, so assertions read `response.outputSpeech.ssml`.
    """

    def __init__(self, request):
        self.request_envelope = _Envelope(request)
        self.response_builder = ResponseFactory()
        self.service_client_factory = None


def _unwrap_ssml(value):
    """Strip the <speak> wrapper the real ResponseFactory adds, returning bare text."""
    if not value:
        return ""
    match = re.match(r"^\s*<speak>(.*)</speak>\s*$", value, re.DOTALL)
    return (match.group(1) if match else value).strip()


def _speech_of(response):
    """The spoken text, from either form this can arrive in.

    A handler called directly returns the ask_sdk_model `Response` OBJECT, while `lambda_handler`
    returns the SERIALISED dict. Reading only one of those is how a test ends up asserting on None.
    """
    if response is None:
        return ""
    if isinstance(response, dict):
        output = response.get("response", {}).get("outputSpeech") or {}
        return _unwrap_ssml(output.get("ssml") or output.get("text"))
    output = getattr(response, "output_speech", None)
    if output is None:
        return ""
    return _unwrap_ssml(getattr(output, "ssml", None) or getattr(output, "text", None))


def _reprompt_of(response):
    """The reprompt text, from either form."""
    if response is None:
        return ""
    if isinstance(response, dict):
        reprompt = response.get("response", {}).get("reprompt") or {}
        output = reprompt.get("outputSpeech") or {}
        return _unwrap_ssml(output.get("ssml") or output.get("text"))
    reprompt = getattr(response, "reprompt", None)
    if reprompt is None:
        return None
    output = getattr(reprompt, "output_speech", None)
    if output is None:
        return ""
    return _unwrap_ssml(getattr(output, "ssml", None) or getattr(output, "text", None))


def speak_of(handler, request):
    """Run a handler with the real response builder and return the speech text it produced."""
    handler_input = FakeHandlerInput(request)
    return _speech_of(handler.handle(handler_input)) or ""


LAUNCH = LaunchRequest()
HELP = _intent_request("AMAZON.HelpIntent")
STOP = _intent_request("AMAZON.StopIntent")
HOME_STATUS = _intent_request("HomeStatusIntent")
RECENT_ALERTS = _intent_request("RecentAlertsIntent")
ALERT_SUMMARY = _intent_request("GetAlertSummaryIntent")
SESSION_ENDED = SessionEndedRequest()


class TestUnavailableIsThePrimaryBehaviour(unittest.TestCase):
    """The hub is unreachable. Every intent must say so and must not invent a reading."""

    def setUp(self):
        patcher = mock.patch.object(lf.backend, "get_homeguardian_status", return_value=None)
        self.addCleanup(patcher.stop)
        patcher.start()
        patcher2 = mock.patch.object(lf.backend, "get_alert_summary", return_value=None)
        self.addCleanup(patcher2.stop)
        patcher2.start()

    def test_launch_says_unavailable(self):
        speech = speak_of(lf.LaunchRequestHandler(), LAUNCH)
        self.assertIn("currently unavailable", speech)

    def test_home_status_says_unavailable(self):
        speech = speak_of(lf.HomeStatusIntentHandler(), HOME_STATUS)
        self.assertIn("currently unavailable", speech)

    def test_recent_alerts_says_unavailable(self):
        speech = speak_of(lf.RecentAlertsIntentHandler(), RECENT_ALERTS)
        self.assertIn("currently unavailable", speech)

    def test_no_digits_leak_into_the_unavailable_answer(self):
        """A stray number would read as a real reading. This is the assertion that matters most."""
        for handler, request in (
            (lf.LaunchRequestHandler(), LAUNCH),
            (lf.HomeStatusIntentHandler(), HOME_STATUS),
            (lf.RecentAlertsIntentHandler(), RECENT_ALERTS),
        ):
            speech = speak_of(handler, request)
            self.assertFalse(
                any(ch.isdigit() for ch in speech),
                "unavailable speech must contain no digits, got: %r" % speech,
            )


class TestStatusRendering(unittest.TestCase):
    def _with_status(self, status):
        return mock.patch.object(lf.backend, "get_homeguardian_status", return_value=status)

    def test_reports_the_hubs_own_values(self):
        with self._with_status({"state": "Running", "capture_enabled": True, "device_count": 5}):
            speech = speak_of(lf.HomeStatusIntentHandler(), HOME_STATUS)
        self.assertIn("Running", speech)
        self.assertIn("enabled", speech)
        self.assertIn("5 devices", speech)

    def test_singular_plural_is_not_wrong(self):
        with self._with_status({"device_count": 1}):
            speech = speak_of(lf.HomeStatusIntentHandler(), HOME_STATUS)
        self.assertIn("1 device registered", speech)

    def test_a_reachable_hub_with_no_known_fields_does_not_invent_a_summary(self):
        """Reachable but unusable is different from unreachable — and still not an invention."""
        with self._with_status({"something_unexpected": 1}):
            speech = speak_of(lf.HomeStatusIntentHandler(), HOME_STATUS)
        self.assertIn("reachable", speech)
        self.assertIn("no status I can report", speech)
        self.assertNotIn("currently unavailable", speech)


class TestAlertRendering(unittest.TestCase):
    def _with_summary(self, summary):
        return mock.patch.object(lf.backend, "get_alert_summary", return_value=summary)

    def test_zero_alerts_is_stated_plainly(self):
        with self._with_summary({"alert_count": 0}):
            speech = speak_of(lf.RecentAlertsIntentHandler(), RECENT_ALERTS)
        self.assertIn("no alerts recorded", speech)

    def test_count_and_severity_are_reported(self):
        with self._with_summary({"alert_count": 2, "highest_severity": "warning"}):
            speech = speak_of(lf.RecentAlertsIntentHandler(), RECENT_ALERTS)
        self.assertIn("2 alerts", speech)
        self.assertIn("warning", speech)

    def test_both_intent_names_reach_the_same_handler(self):
        """RecentAlertsIntent is what the owner asked for; GetAlertSummaryIntent already existed."""
        handler = lf.RecentAlertsIntentHandler()
        self.assertTrue(handler.can_handle(FakeHandlerInput(RECENT_ALERTS)))
        self.assertTrue(handler.can_handle(FakeHandlerInput(ALERT_SUMMARY)))
        self.assertFalse(handler.can_handle(FakeHandlerInput(HOME_STATUS)))


class TestDispatchAndBoundaries(unittest.TestCase):
    def test_help_and_stop_do_not_touch_the_backend(self):
        with mock.patch.object(lf.backend, "get_homeguardian_status") as s, \
                mock.patch.object(lf.backend, "get_alert_summary") as a:
            help_speech = speak_of(lf.HelpIntentHandler(), HELP)
            stop_speech = speak_of(lf.CancelOrStopIntentHandler(), STOP)
        self.assertIn("home status", help_speech)
        self.assertEqual("Goodbye.", stop_speech)
        s.assert_not_called()
        a.assert_not_called()

    def test_every_handler_runs_against_the_real_response_factory(self):
        """THE REGRESSION FOR THE DEPLOYED FAILURE.

        The deployed skill raised `AttributeError: 'ResponseFactory' object has no attribute 'reprompt'`
        on LaunchRequest and HelpIntent - the two handlers that reprompt - and the owner saw the same
        failure for both. The old test double implemented `.reprompt()`, so it could never catch this.

        This runs EVERY handler through the REAL ResponseFactory. Any SDK method the code calls that
        does not exist therefore raises here, in a test, instead of in the console.
        """
        with mock.patch.object(lf.backend, "get_homeguardian_status", return_value=None), \
                mock.patch.object(lf.backend, "get_alert_summary", return_value=None):
            for handler, request in (
                (lf.LaunchRequestHandler(), LAUNCH),
                (lf.HomeStatusIntentHandler(), HOME_STATUS),
                (lf.RecentAlertsIntentHandler(), RECENT_ALERTS),
                (lf.RecentAlertsIntentHandler(), ALERT_SUMMARY),
                (lf.HelpIntentHandler(), HELP),
                (lf.CancelOrStopIntentHandler(), STOP),
                (lf.SessionEndedRequestHandler(), SESSION_ENDED),
            ):
                with self.subTest(handler=type(handler).__name__):
                    response = handler.handle(FakeHandlerInput(request))
                    # Direct handler call -> ask_sdk_model Response object (lambda_handler serialises).
                    self.assertIsNotNone(response)
                    self.assertIsNotNone(_speech_of(response))

    def test_reprompting_handlers_set_a_reprompt_via_ask(self):
        """Launch and Help must keep the session open. In the Python SDK that is `.ask()`, not
        `.reprompt()`; asserting the reprompt is actually present is what makes the method choice
        observable rather than incidental."""
        with mock.patch.object(lf.backend, "get_homeguardian_status", return_value=None):
            for handler, request in (
                (lf.LaunchRequestHandler(), LAUNCH),
                (lf.HelpIntentHandler(), HELP),
            ):
                with self.subTest(handler=type(handler).__name__):
                    response = handler.handle(FakeHandlerInput(request))
                    self.assertIsNotNone(_reprompt_of(response),
                                         "a reprompting handler must set one")

    def test_the_python_sdk_has_no_reprompt_method(self):
        """Pins the fact the bug was made of, so a future edit 'restoring' .reprompt() fails here."""
        self.assertFalse(hasattr(ResponseFactory, "reprompt"))
        self.assertTrue(hasattr(ResponseFactory, "ask"))
        self.assertTrue(hasattr(ResponseFactory, "speak"))

    def test_handlers_are_mutually_exclusive(self):
        cases = [
            (lf.LaunchRequestHandler(), LAUNCH, True),
            (lf.HomeStatusIntentHandler(), LAUNCH, False),
            (lf.HomeStatusIntentHandler(), HOME_STATUS, True),
            (lf.RecentAlertsIntentHandler(), HOME_STATUS, False),
            (lf.HelpIntentHandler(), HELP, True),
            (lf.CancelOrStopIntentHandler(), STOP, True),
            (lf.SessionEndedRequestHandler(), SESSION_ENDED, True),
        ]
        for handler, request, expected in cases:
            with self.subTest(handler=type(handler).__name__, request=request.object_type):
                self.assertEqual(expected, handler.can_handle(FakeHandlerInput(request)))

    def test_household_id_comes_from_the_alexa_user(self):
        handler_input = FakeHandlerInput(HOME_STATUS)
        self.assertEqual("amzn1.ask.account.TESTHOUSEHOLD", lf.household_id(handler_input))

    def test_household_id_is_forwarded_to_the_hub(self):
        """Authorization stays server-side: we pass the identity and report what the hub returns."""
        with mock.patch.object(lf.backend, "get_homeguardian_status", return_value=None) as s:
            speak_of(lf.HomeStatusIntentHandler(), HOME_STATUS)
        s.assert_called_once()
        self.assertEqual("amzn1.ask.account.TESTHOUSEHOLD", s.call_args[0][0])

    def test_catch_all_handler_does_not_leak_an_exception_message(self):
        """A transport exception message can carry a URL or header; only the class may be logged."""
        response = lf.CatchAllExceptionHandler().handle(
            FakeHandlerInput(HOME_STATUS),
            RuntimeError("Bearer supersecret-token-abcdef0123456789 at http://127.0.0.1:8790/mcp"),
        )
        spoken = _speech_of(response) or ""
        # It must say the unavailable line...
        self.assertIn("currently unavailable", spoken)
        # ...and must NOT echo the exception text, which here carries a fake token and a URL.
        self.assertNotIn("supersecret", spoken)
        self.assertNotIn("8790", spoken)


class TestBackendSafety(unittest.TestCase):
    """The transport's own rules, checked without a network."""

    def test_non_loopback_host_is_refused(self):
        """The mission states the MCP listener stays loopback-only; the code must agree."""
        with mock.patch.dict(os.environ, {"HG_MCP_HOST": "10.0.0.5"}):
            with self.assertRaises(backend.BackendUnavailable):
                backend._endpoint()

    def test_loopback_hosts_are_accepted(self):
        for host in ("127.0.0.1", "::1", "localhost"):
            with mock.patch.dict(os.environ, {"HG_MCP_HOST": host}):
                self.assertEqual(host, backend._endpoint()[0])

    def test_missing_token_is_unavailable_not_an_error(self):
        env = {k: v for k, v in os.environ.items() if k != "HG_MCP_TOKEN"}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertIsNone(backend.get_homeguardian_status())
            self.assertIsNone(backend.get_alert_summary())

    def test_unreachable_hub_returns_none(self):
        with mock.patch.dict(os.environ, {"HG_MCP_TOKEN": "x", "HG_MCP_PORT": "1"}):
            # Port 1 is not listening; the call must degrade to None, never raise into Alexa.
            self.assertIsNone(backend.get_homeguardian_status())

    def test_sse_framed_body_is_parsed(self):
        body = 'event: message\ndata: {"jsonrpc":"2.0","id":1,"result":{"content":[{"text":"{\\"alert_count\\":3}"}]}}\n'
        self.assertEqual({"alert_count": 3}, backend._parse_rpc(body))

    def test_error_response_is_unavailable(self):
        with self.assertRaises(backend.BackendUnavailable):
            backend._parse_rpc('{"jsonrpc":"2.0","id":1,"error":{"code":-32601}}')

    def test_non_json_body_is_unavailable(self):
        with self.assertRaises(backend.BackendUnavailable):
            backend._parse_rpc("not json at all")

    def test_intent_names_match_the_cpp_tool_constants(self):
        """The tool names must equal src/mcp/McpTools.h:23-24, not merely resemble them."""
        self.assertEqual("get_homeguardian_status", backend.TOOL_STATUS)
        self.assertEqual("get_alert_summary", backend.TOOL_ALERT_SUMMARY)


if __name__ == "__main__":
    unittest.main(verbosity=2)
