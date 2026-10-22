"""HomeGuardian AI(dhar) -- Alexa-hosted Python skill. Voice adapter only.

Launch/Welcome, Help, home security status, active alert summary, camera availability and health, animal
activity summary, household routine status, emergency advisory summary, Stop/Cancel, and error/timeout
handling.

Two rules govern every response:
  1. It is built from data the C++ gateway actually returned. If the gateway is unreachable, or does not
     expose a field, the skill says it cannot tell -- it does not assert a state.
  2. No claim is made about predicting earthquakes, fires, disasters or other emergencies from animal
     behaviour, or from anything else. The activity summary reports that authorized observations exist and
     how many; it never interprets them, and unvalidated ideas are described as not established.

The analysis stays in the C++ application. This module only asks it questions and reads the answers.
"""
from __future__ import annotations

import backend

try:  # present in the Alexa-hosted Python runtime
    from ask_sdk_core.skill_builder import SkillBuilder
    from ask_sdk_core.dispatch_components import AbstractRequestHandler, AbstractExceptionHandler
    from ask_sdk_core.utils import is_request_type, is_intent_name
    from ask_sdk_model import Response
    from ask_sdk_model.ui import SimpleCard

    SDK_AVAILABLE = True
except Exception:  # unit tests inject a stub module; the handlers below are unchanged
    SkillBuilder = None                    # type: ignore
    AbstractRequestHandler = object        # type: ignore
    AbstractExceptionHandler = object      # type: ignore
    Response = None                        # type: ignore
    SimpleCard = None                      # type: ignore
    SDK_AVAILABLE = False

    def is_request_type(name):             # type: ignore
        return lambda h: h.request_envelope.request.object_type == name

    def is_intent_name(name):              # type: ignore
        return lambda h: h.request_envelope.request.intent.name == name


SKILL_NAME = "HomeGuardian"


def unavailable_speech(exc) -> str:
    """Distinguish "it refused" from "it isn't there". Both mean no data; they are not the same statement."""
    reason = getattr(exc, "reason", "unreachable")
    if reason == "refused":
        return ("The HomeGuardian service is running, but it refused that request, so I can't tell you. "
                "Nothing has been changed, and I won't guess.")
    if reason in ("unparseable", "unexpected_shape"):
        return ("The HomeGuardian service answered with something I could not read, so I can't tell you. "
                "I won't guess.")
    return UNAVAILABLE_SPEECH
UNAVAILABLE_SPEECH = ("I can't reach the HomeGuardian service right now. Nothing has been changed, "
                      "and I won't guess at the state of your home.")
HELP_BODY = ("You can ask about home status, alerts, camera availability, detected activity, household "
             "routines, or the emergency advisory summary.")
HELP_SPEECH = f"{SKILL_NAME} is ready. {HELP_BODY}"


class _Response:
    """Small stand-in for the SDK response so the logic is testable without SDK objects."""

    def __init__(self):
        self._speech = ""
        self._card = None

    def speak(self, text):
        self._speech = text
        return self

    def card(self, title, body):
        if title:
            self._card = (title, body or "")
        return self

    def build(self):
        if SDK_AVAILABLE:
            resp = Response()
            resp.output_speech = self._speech
            resp.should_end_session = False
            if self._card:
                resp.card = SimpleCard(title=self._card[0], content=self._card[1])
            return resp
        return {"output_speech": self._speech, "should_end_session": False,
                "card": {"title": self._card[0], "content": self._card[1]} if self._card else None}


def _speak(text, title=None, body=None):
    return _Response().speak(text).card(title, body).build()


def home_status_speech() -> str:
    """Per-source honesty: each statement is reported only if that source actually answered.

    A refused /api/status must not discard a verified alert count, and a verified zero must not be turned
    into "unreachable" -- the skill says exactly as much as it established, and no more.
    """
    parts = []
    try:
        alerts, highest, _ids = backend.describe_alerts()
        if alerts == 0:
            parts.append("There are no recorded alerts.")
        elif alerts == 1:
            parts.append(f"There is 1 recorded alert, highest severity {highest}.")
        else:
            parts.append(f"There are {alerts} recorded alerts, highest severity {highest}.")
    except backend.BackendUnavailable as exc:
        parts.append(unavailable_speech(exc))
    try:
        capture = backend.capture_enabled()
    except backend.BackendUnavailable:
        capture = None
    if capture is True:
        parts.append("Capture is enabled on the registered devices.")
    elif capture is False:
        parts.append("Capture is currently disabled.")
    else:
        parts.append("I could not establish the capture state, and I won't guess it.")
    return " ".join(parts)


def alert_summary_speech() -> str:
    try:
        alerts, highest, ids = backend.describe_alerts()
    except backend.BackendUnavailable as exc:
        return unavailable_speech(exc)
    if alerts == 0:
        return "There are no recorded alerts from the HomeGuardian service."
    plural = "alert" if alerts == 1 else "alerts"
    tail = f" The most recent alert identifier is {ids[0]}." if ids else ""
    return (f"There {'is' if alerts == 1 else 'are'} {alerts} recorded {plural}, highest severity "
            f"{highest}.{tail} The detail comes from the HomeGuardian service, not from me.")


def camera_speech() -> str:
    """Availability and health only. No stream, no image, no camera credential is requested."""
    try:
        status = backend.get_status()
    except backend.BackendUnavailable as exc:
        return unavailable_speech(exc)
    camera = status.get("camera") if isinstance(status.get("camera"), dict) else None
    if camera is None:
        return ("HomeGuardian's service does not report camera availability over this interface, so I "
                "can't tell you anything about the cameras. I won't guess.")
    return (f"The camera interface reports state {camera.get('state', 'unknown')}. "
            "No video or audio is available through me.")


def animal_activity_speech() -> str:
    """A count of authorized observations, nothing more. No interpretation, no prediction."""
    try:
        status = backend.get_status()
    except backend.BackendUnavailable as exc:
        return unavailable_speech(exc)
    observations = status.get("observations")
    count = observations.get("recent") if isinstance(observations, dict) else None
    if not isinstance(count, int):
        return ("I can check recent authorized observations, but the service isn't reporting a count "
                "over this interface right now, so I can't tell you. I won't guess.")
    if count == 0:
        return "There are no recent authorized observations to report."
    return (f"There {'is' if count == 1 else 'are'} {count} recent authorized "
            f"{'observation' if count == 1 else 'observations'}. That is a count of detected activity, "
            "not a statement about its cause, and I don't predict earthquakes or other emergencies from "
            "animal behaviour.")


def routine_speech() -> str:
    try:
        status = backend.get_status()
    except backend.BackendUnavailable as exc:
        return unavailable_speech(exc)
    routines = status.get("routines")
    if not isinstance(routines, list):
        return ("The service isn't reporting household routine status over this interface, so I can't "
                "tell you. I won't guess.")
    if not routines:
        return "No household routines are registered in the HomeGuardian service."
    return (f"{len(routines)} household {'routine is' if len(routines) == 1 else 'routines are'} "
            "registered with the service.")


def emergency_advisory_speech() -> str:
    """Reports what the service has, and explicitly refuses to predict."""
    try:
        alerts, highest, _ids = backend.describe_alerts()
    except backend.BackendUnavailable as exc:
        return unavailable_speech(exc)
    head = ("HomeGuardian has no recorded alerts to advise on." if alerts == 0 else
            f"HomeGuardian has {alerts} recorded {'alert' if alerts == 1 else 'alerts'}, highest "
            f"severity {highest}.")
    return (head + " To be clear about what this is: HomeGuardian reports detected events, and it does not "
            "predict earthquakes, fires or other emergencies. For an emergency, contact the emergency "
            "services.")


class LaunchHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_request_type("LaunchRequest")(handler_input)

    def handle(self, handler_input):
        try:
            backend.get_health()
            ready = True
        except backend.BackendUnavailable:
            ready = False
        if ready:
            return _speak(f"{SKILL_NAME} is ready. You can ask about home status, alerts, or detected "
                          "activity.", SKILL_NAME, "Ready")
        # Not "is ready": the service is unreachable, and saying otherwise two sentences apart would be
        # the same kind of contradiction this skill refuses elsewhere.
        return _speak(f"{SKILL_NAME} is running, but its home service is unreachable, so I can only help "
                      f"with this skill itself. {HELP_BODY}", SKILL_NAME, "Service unreachable")


class HelpHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("AMAZON.HelpIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(HELP_SPEECH, SKILL_NAME, "Help")


class HomeStatusHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("HomeStatusIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(home_status_speech(), "Home status")


class AlertSummaryHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("GetAlertSummaryIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(alert_summary_speech(), "Alerts")


class CameraHealthHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("CameraAvailabilityIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(camera_speech(), "Camera availability")


class AnimalActivityHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("AnimalActivityIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(animal_activity_speech(), "Detected activity")


class RoutineStatusHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("GetRoutineStatusIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(routine_speech(), "Household routines")


class EmergencyAdvisoryHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_intent_name("EmergencyAdvisoryIntent")(handler_input)

    def handle(self, handler_input):
        return _speak(emergency_advisory_speech(), "Emergency advisory")


class StopCancelHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return (is_intent_name("AMAZON.StopIntent")(handler_input) or
                is_intent_name("AMAZON.CancelIntent")(handler_input))

    def handle(self, handler_input):
        return _speak("Goodbye.", SKILL_NAME, "Stopped")


class SessionEndedHandler(AbstractRequestHandler):
    def can_handle(self, handler_input):
        return is_request_type("SessionEndedRequest")(handler_input)

    def handle(self, handler_input):
        return _speak("")


class CatchAllHandler(AbstractRequestHandler):
    """Anything unrecognised gets help -- never an invented answer."""

    def can_handle(self, handler_input):
        return True

    def handle(self, handler_input):
        return _speak("I didn't understand that. " + HELP_SPEECH, SKILL_NAME, "Help")


class AllExceptionsHandler(AbstractExceptionHandler):
    def can_handle(self, handler_input, exception):
        return True

    def handle(self, handler_input, exception):
        # The exception text is never spoken and never logged with payloads.
        return _speak(UNAVAILABLE_SPEECH, SKILL_NAME, "Error")


HANDLERS = (LaunchHandler, HelpHandler, HomeStatusHandler, AlertSummaryHandler, CameraHealthHandler,
            AnimalActivityHandler, RoutineStatusHandler, EmergencyAdvisoryHandler, StopCancelHandler,
            SessionEndedHandler, CatchAllHandler)


def build_skill():
    if not SDK_AVAILABLE:
        raise RuntimeError("ask_sdk_core is not available in this environment")
    builder = SkillBuilder()
    for handler_cls in HANDLERS:
        builder.add_request_handler(handler_cls())
    builder.add_exception_handler(AllExceptionsHandler())
    return builder


if SDK_AVAILABLE:
    lambda_handler = build_skill().lambda_handler
else:
    def lambda_handler(event, context):  # pragma: no cover - only when the SDK is absent
        raise RuntimeError("ask_sdk_core is not available in this environment")
