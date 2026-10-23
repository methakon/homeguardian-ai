"""Full exported-Lambda contracts, using the real ASK SDK and synthetic envelopes."""
import json
import os
from pathlib import Path
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from test_skill import backend, lf


def envelope(request_type="LaunchRequest", intent=None):
    request: dict = {"type": request_type, "requestId": "test-request", "timestamp": "2026-10-23T00:00:00Z", "locale": "en-US"}
    if request_type == "IntentRequest":
        request["intent"] = {"name": intent, "confirmationStatus": "NONE", "slots": {}}
    return {"version": "1.0", "session": {"new": True, "sessionId": "test-session",
            "application": {"applicationId": "test-skill"}, "user": {"userId": "test-user"}},
            "context": {"System": {"application": {"applicationId": "test-skill"},
                        "user": {"userId": "test-user"}, "device": {"deviceId": "test-device", "supportedInterfaces": {}}}},
            "request": request}


def speech(response):
    output = response["response"].get("outputSpeech", {})
    return output.get("ssml", output.get("text", ""))


class TestLambdaEnvelope(unittest.TestCase):
    def test_invalid_envelopes_return_valid_safe_responses(self):
        invalid = [None, [], "bad", 3, {}, {"request": None}, {"request": []},
                   {"request": {"type": "IntentRequest"}},
                   {"request": {"type": "IntentRequest", "intent": None}},
                   {"request": {"type": "IntentRequest", "intent": {"name": []}}}]
        with mock.patch.object(backend, "get_homeguardian_status") as status, \
                mock.patch.object(backend, "get_alert_summary") as alerts:
            for event in invalid:
                with self.subTest(event=event):
                    result = lf.lambda_handler(event, None)
                    self.assertEqual(result["version"], "1.0")
                    self.assertIsInstance(result["response"], dict)
                    self.assertTrue(speech(result))
                    json.dumps(result)
            status.assert_not_called()
            alerts.assert_not_called()

    def test_exported_handler_serializes_every_supported_intent(self):
        model = json.loads((Path(__file__).parents[1] / "interactionModels/custom/en-US.json").read_text())
        names = [item["name"] for item in model["interactionModel"]["languageModel"]["intents"]]
        with mock.patch.object(backend, "get_homeguardian_status", return_value={"state": "Running", "capture_enabled": False}), \
                mock.patch.object(backend, "get_alert_summary", return_value={"alert_count": 0}):
            for event in [envelope()] + [envelope("IntentRequest", name) for name in names]:
                with self.subTest(request=event["request"]):
                    result = lf.lambda_handler(event, None)
                    self.assertEqual(result["version"], "1.0")
                    self.assertTrue(speech(result))
                    output = result["response"]["outputSpeech"]
                    if output["type"] == "SSML":
                        self.assertEqual(ET.fromstring(output["ssml"]).tag, "speak")
                    json.dumps(result)

    def test_model_preserves_original_node_intents(self):
        root = Path(__file__).parents[2]
        def names(path):
            return {item["name"] for item in json.loads(path.read_text())["interactionModel"]["languageModel"]["intents"]}
        self.assertLessEqual(names(root / "alexa_skill/interactionModels/custom/en-US.json"),
                             names(root / "alexa_skill_python/interactionModels/custom/en-US.json"))

    def test_routine_intent_does_not_invent_routines(self):
        result = lf.lambda_handler(envelope("IntentRequest", "GetRoutineStatusIntent"), None)
        self.assertIn("routine", speech(result).lower())
        self.assertNotIn("no configured routines", speech(result).lower())

    def test_unknown_intent_offers_help(self):
        result = lf.lambda_handler(envelope("IntentRequest", "UnknownIntent"), None)
        self.assertIn("help", speech(result).lower())

    def test_stop_and_cancel_explicitly_end_session(self):
        for name in ["AMAZON.StopIntent", "AMAZON.CancelIntent"]:
            with self.subTest(intent=name):
                result = lf.lambda_handler(envelope("IntentRequest", name), None)
                self.assertTrue(result["response"]["shouldEndSession"])

    def test_session_ended_returns_no_speech(self):
        result = lf.lambda_handler(envelope("SessionEndedRequest"), None)
        self.assertFalse(speech(result))

    def test_backend_text_cannot_break_ssml(self):
        fixtures = [("HomeStatusIntent", "get_homeguardian_status", {"state": "<bad&state>"}),
                    ("GetAlertSummaryIntent", "get_alert_summary", {"alert_count": 1, "highest_severity": "<bad&severity>"})]
        for intent, method, data in fixtures:
            with self.subTest(intent=intent), mock.patch.object(backend, method, return_value=data):
                result = lf.lambda_handler(envelope("IntentRequest", intent), None)
                ET.fromstring(speech(result))

    def test_invalid_counts_are_not_spoken_as_real_alerts(self):
        for count in [-1, True, "0", None]:
            with self.subTest(count=count), mock.patch.object(backend, "get_alert_summary", return_value={"alert_count": count}):
                result = lf.lambda_handler(envelope("IntentRequest", "GetAlertSummaryIntent"), None)
                self.assertIn("no alert summary", speech(result))


if __name__ == "__main__":
    unittest.main()
