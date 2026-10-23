"""HomeGuardian AI — Alexa custom skill (ASK SDK for Python, Alexa-hosted).

DROP-IN TARGET: an Alexa-hosted (Python) skill, "Start from Scratch", en-US. Alexa-hosted stores code
in its own Lambda editor and the model in the console, so this file is written to be pasted whole into
the Code tab, with the model pasted into the Build tab. No build step, no dependencies beyond the SDK
that Alexa-hosted provides.

INTENTS IMPLEMENTED  (Launch, HomeStatus, RecentAlerts, Help, Stop)
  LaunchRequest                greeting, then the same status read as HomeStatus
  HomeStatusIntent             hub status via get_homeguardian_status
  RecentAlertsIntent           alert summary via get_alert_summary
  GetAlertSummaryIntent        the SAME handler as RecentAlertsIntent. The repository's existing
                               alexa_skill/interactionModels/custom/en-US.json already defines
                               GetAlertSummaryIntent, so mapping both names keeps this skill's model
                               additively compatible with the one that exists rather than forking it.
  AMAZON.HelpIntent            what the skill can do
  AMAZON.StopIntent            "Goodbye."
  AMAZON.CancelIntent          "Goodbye."
  SessionEndedRequest          silent close

THE RULE THIS SKILL IS BUILT AROUND
"HomeGuardian is currently unavailable" is a first-class answer, not a fallback of last resort. When
the hub cannot be reached or is not authenticated, this skill says exactly that and stops. It does not
substitute a zero, a guess, or a remembered value, and it does not narrate sensor readings it did not
receive. A security product whose voice interface improvises is not a security product.

WHAT IS DELIBERATELY NOT CLAIMED
  - No Alexa-cloud-to-hub access is established by these files. The private phone/VPS connection
    does not make an AWS Lambda's loopback reach the phone or authorize public gateway exposure.
  - No household sensor data is fabricated. If the hub returns nothing, the skill says nothing.
  - No credentials are read, echoed, logged or spoken.
  - Deployment to the console is NOT claimed: that requires the owner to create the skill and paste
    these files. See README.md.
"""

from __future__ import annotations

import logging

import ask_sdk_core.utils as ask_utils
from ask_sdk_core.dispatch_components import AbstractExceptionHandler, AbstractRequestHandler
from ask_sdk_core.handler_input import HandlerInput
from ask_sdk_core.skill_builder import SkillBuilder
from ask_sdk_model import Response

import backend

logger = logging.getLogger(__name__)
logger.setLevel(logging.INFO)

# The one sentence used everywhere the hub is not available. Kept as a constant so it cannot drift into
# several subtly different phrasings, and written so it never implies the data exists.
UNAVAILABLE_SPEECH = (
    "HomeGuardian is currently unavailable. I could not reach your hub, "
    "so I don't have any home status to report."
)

UNAVAILABLE_ALERTS_SPEECH = (
    "HomeGuardian is currently unavailable. I could not reach your hub, "
    "so I don't have any alert information to report."
)

HELP_SPEECH = (
    "You can ask me for your home status, or for recent alerts. "
    "I only report what your HomeGuardian hub tells me. "
    "If the hub is unreachable, I will say so rather than guess."
)


def household_id(handler_input: HandlerInput) -> str | None:
    """The household this request belongs to.

    Taken from the Alexa user id, which is already a stable per-user identifier. It is FORWARDED to the
    hub, never used here to decide what may be read: authorization belongs where the data is, so a
    future per-household policy is enforced server-side rather than by this handler's good intentions.
    """
    try:
        user = handler_input.request_envelope.context.system.user
        return getattr(user, "user_id", None)
    except Exception:  # pragma: no cover - defensive: a malformed envelope is not a crash here
        return None


def _status_speech(status: dict | None) -> str:
    """Render the hub's status. Returns the unavailable line when there is nothing to render."""
    if not status:
        return UNAVAILABLE_SPEECH

    # Only speak fields the hub actually provided. No field is defaulted to a value that would read as
    # a real observation.
    parts: list[str] = []
    state = status.get("state")
    if isinstance(state, str) and state:
        parts.append("The hub state is %s." % state)
    capture = status.get("capture_enabled")
    if isinstance(capture, bool):
        parts.append("Capture is %s." % ("enabled" if capture else "disabled"))
    devices = status.get("device_count")
    if isinstance(devices, int):
        parts.append("%d device%s registered." % (devices, "" if devices == 1 else "s"))
    alerts = status.get("alert_count")
    if isinstance(alerts, int):
        parts.append("%d alert%s recorded." % (alerts, "" if alerts == 1 else "s"))

    if not parts:
        # The hub answered but with nothing this skill knows how to say. Saying nothing is correct;
        # inventing a summary would not be.
        return "Your hub is reachable, but it returned no status I can report."
    return " ".join(parts)


def _alerts_speech(summary: dict | None) -> str:
    """Render the hub's alert summary, or the unavailable line."""
    if not summary:
        return UNAVAILABLE_ALERTS_SPEECH

    count = summary.get("alert_count")
    if not isinstance(count, int):
        return "Your hub is reachable, but it returned no alert summary I can report."
    if count == 0:
        return "There are no alerts recorded for your home."

    parts = ["There %s %d alert%s recorded." % ("is" if count == 1 else "are", count,
                                                "" if count == 1 else "s")]
    severity = summary.get("highest_severity")
    if isinstance(severity, str) and severity:
        parts.append("The highest severity is %s." % severity)
    return " ".join(parts)


class LaunchRequestHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return ask_utils.is_request_type("LaunchRequest")(handler_input)

    def handle(self, handler_input: HandlerInput) -> Response:
        status = backend.get_homeguardian_status(household_id(handler_input))
        speech = "Welcome to HomeGuardian. " + _status_speech(status)
        return (
            handler_input.response_builder.speak(speech)
            .ask("You can ask for your home status or recent alerts.")
            .response
        )


class HomeStatusIntentHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return ask_utils.is_intent_name("HomeStatusIntent")(handler_input)

    def handle(self, handler_input: HandlerInput) -> Response:
        status = backend.get_homeguardian_status(household_id(handler_input))
        return handler_input.response_builder.speak(_status_speech(status)).response


class RecentAlertsIntentHandler(AbstractRequestHandler):
    """Handles both RecentAlertsIntent and the repository's existing GetAlertSummaryIntent."""

    NAMES = ("RecentAlertsIntent", "GetAlertSummaryIntent")

    def can_handle(self, handler_input: HandlerInput) -> bool:
        return any(ask_utils.is_intent_name(name)(handler_input) for name in self.NAMES)

    def handle(self, handler_input: HandlerInput) -> Response:
        summary = backend.get_alert_summary(household_id(handler_input))
        return handler_input.response_builder.speak(_alerts_speech(summary)).response


class HelpIntentHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return ask_utils.is_intent_name("AMAZON.HelpIntent")(handler_input)

    def handle(self, handler_input: HandlerInput) -> Response:
        return (
            handler_input.response_builder.speak(HELP_SPEECH)
            .ask("What would you like to know?")
            .response
        )


class RoutineStatusIntentHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return ask_utils.is_intent_name("GetRoutineStatusIntent")(handler_input)

    def handle(self, handler_input: HandlerInput) -> Response:
        return handler_input.response_builder.speak(
            "Household routine status is not available through this read-only interface. "
            "I have not verified any routines, and nothing has been changed."
        ).response


class CancelOrStopIntentHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return (
            ask_utils.is_intent_name("AMAZON.CancelIntent")(handler_input)
            or ask_utils.is_intent_name("AMAZON.StopIntent")(handler_input)
        )

    def handle(self, handler_input: HandlerInput) -> Response:
        return handler_input.response_builder.speak("Goodbye.").set_should_end_session(True).response


class SessionEndedRequestHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return ask_utils.is_request_type("SessionEndedRequest")(handler_input)

    def handle(self, handler_input: HandlerInput) -> Response:
        return handler_input.response_builder.response


class CatchAllExceptionHandler(AbstractExceptionHandler):
    """Last line of defence.

    It logs the EXCEPTION CLASS only. An exception message from a transport layer can carry a URL or a
    header value, and this skill's logs live in a hosted console; the class name is enough to find the
    problem and cannot leak a token.
    """

    def can_handle(self, handler_input: HandlerInput, exception: Exception) -> bool:
        return True

    def handle(self, handler_input: HandlerInput, exception: Exception) -> Response:
        logger.error("unhandled skill error: %s", exception.__class__.__name__)
        return handler_input.response_builder.speak(UNAVAILABLE_SPEECH).response


class UnhandledRequestHandler(AbstractRequestHandler):
    def can_handle(self, handler_input: HandlerInput) -> bool:
        return True

    def handle(self, handler_input: HandlerInput) -> Response:
        return handler_input.response_builder.speak(
            "I can help with home status or recent alerts. Please ask for help or try again."
        ).ask("What would you like to know?").response


sb = SkillBuilder()
sb.add_request_handler(LaunchRequestHandler())
sb.add_request_handler(HomeStatusIntentHandler())
sb.add_request_handler(RecentAlertsIntentHandler())
sb.add_request_handler(HelpIntentHandler())
sb.add_request_handler(RoutineStatusIntentHandler())
sb.add_request_handler(CancelOrStopIntentHandler())
sb.add_request_handler(SessionEndedRequestHandler())
sb.add_request_handler(UnhandledRequestHandler())
sb.add_exception_handler(CatchAllExceptionHandler())

_sdk_lambda_handler = sb.lambda_handler()


def lambda_handler(event, context):
    """Contain deserialization failures outside the SDK's exception-handler chain."""
    request = event.get("request") if isinstance(event, dict) else None
    valid = isinstance(request, dict) and isinstance(request.get("type"), str)
    if isinstance(request, dict) and request.get("type") == "IntentRequest":
        intent = request.get("intent")
        valid = isinstance(intent, dict) and isinstance(intent.get("name"), str) and bool(intent["name"])
    if valid:
        try:
            return _sdk_lambda_handler(event, context)
        except Exception as exc:
            logger.error("skill envelope failure: %s", exc.__class__.__name__)
    return {"version": "1.0", "response": {
        "outputSpeech": {"type": "PlainText", "text": "Sorry, I could not read that request. Please ask HomeGuardian for help."},
        "shouldEndSession": True,
    }}
