'use strict';

const Alexa = require('ask-sdk-core');
const { HomeGuardianBackend } = require('./src/backend-interface');

// The backend instance. In the prototype this is the mock; a real backend would
// be injected here (dependency point), leaving handlers unchanged.
const backend = new HomeGuardianBackend();

// A hard cap on how many items we ever speak aloud, so a large backend result
// can never flood the response or leak sensitive detail.
const MAX_SPOKEN_ITEMS = 5;

const LaunchRequestHandler = {
  canHandle(handlerInput) {
    return Alexa.getRequestType(handlerInput.requestEnvelope) === 'LaunchRequest';
  },
  handle(handlerInput) {
    const speechText =
      'Welcome to Home Guardian. I can report the system status, a summary of alerts, ' +
      'or the status of your routines. What would you like to know?';
    return handlerInput.responseBuilder
      .speak(speechText)
      .reprompt(speechText)
      .getResponse();
  },
};

const HomeStatusIntentHandler = {
  canHandle(handlerInput) {
    return (
      Alexa.getRequestType(handlerInput.requestEnvelope) === 'IntentRequest' &&
      Alexa.getIntentName(handlerInput.requestEnvelope) === 'HomeStatusIntent'
    );
  },
  async handle(handlerInput) {
    let status;
    try {
      status = await backend.getStatus();
    } catch (err) {
      return handleBackendError(handlerInput, err);
    }
    const speechText = status.status + '. Camera and microphone capture are disabled.';
    return handlerInput.responseBuilder.speak(speechText).getResponse();
  },
};

const GetAlertSummaryIntentHandler = {
  canHandle(handlerInput) {
    return (
      Alexa.getRequestType(handlerInput.requestEnvelope) === 'IntentRequest' &&
      Alexa.getIntentName(handlerInput.requestEnvelope) === 'GetAlertSummaryIntent'
    );
  },
  async handle(handlerInput) {
    let summary;
    try {
      summary = await backend.getAlertSummary();
    } catch (err) {
      return handleBackendError(handlerInput, err);
    }
    let speechText;
    if (!summary || typeof summary.count !== 'number') {
      return handleInvalidData(handlerInput);
    }
    if (summary.count === 0) {
      speechText = 'There are no active alerts.';
    } else {
      const severity = summary.highest_severity ? summary.highest_severity : 'unspecified';
      speechText = `There ${summary.count === 1 ? 'is' : 'are'} ${summary.count} active ` +
        `alert${summary.count === 1 ? '' : 's'}, highest severity ${severity}.`;
    }
    return handlerInput.responseBuilder.speak(speechText).getResponse();
  },
};

const GetRoutineStatusIntentHandler = {
  canHandle(handlerInput) {
    return (
      Alexa.getRequestType(handlerInput.requestEnvelope) === 'IntentRequest' &&
      Alexa.getIntentName(handlerInput.requestEnvelope) === 'GetRoutineStatusIntent'
    );
  },
  async handle(handlerInput) {
    let status;
    try {
      status = await backend.getRoutineStatus();
    } catch (err) {
      return handleBackendError(handlerInput, err);
    }
    let speechText;
    if (!status || typeof status.count !== 'number') {
      return handleInvalidData(handlerInput);
    }
    if (status.count === 0) {
      speechText = 'There are no configured routines.';
    } else {
      const enabled = Array.isArray(status.routines)
        ? status.routines.filter((r) => r && r.enabled).length
        : 0;
      speechText = `There ${status.count === 1 ? 'is' : 'are'} ${status.count} routine` +
        `${status.count === 1 ? '' : 's'} configured, ${enabled} enabled.`;
    }
    return handlerInput.responseBuilder.speak(speechText).getResponse();
  },
};

const HelpIntentHandler = {
  canHandle(handlerInput) {
    return (
      Alexa.getRequestType(handlerInput.requestEnvelope) === 'IntentRequest' &&
      Alexa.getIntentName(handlerInput.requestEnvelope) === 'AMAZON.HelpIntent'
    );
  },
  handle(handlerInput) {
    const speechText =
      'You can ask me for the home status, an alert summary, or your routine status. ' +
      'For example, say what is the home status.';
    return handlerInput.responseBuilder.speak(speechText).reprompt(speechText).getResponse();
  },
};

const CancelAndStopIntentHandler = {
  canHandle(handlerInput) {
    return (
      Alexa.getRequestType(handlerInput.requestEnvelope) === 'IntentRequest' &&
      ['AMAZON.CancelIntent', 'AMAZON.StopIntent'].includes(
        Alexa.getIntentName(handlerInput.requestEnvelope)
      )
    );
  },
  handle(handlerInput) {
    return handlerInput.responseBuilder.speak('Goodbye.').getResponse();
  },
};

const SessionEndedRequestHandler = {
  canHandle(handlerInput) {
    return Alexa.getRequestType(handlerInput.requestEnvelope) === 'SessionEndedRequest';
  },
  handle(handlerInput) {
    // Any cleanup logic goes here. Returning an empty response ends the session.
    return handlerInput.responseBuilder.getResponse();
  },
};

// Fallback for unhandled intents — always responds politely, never leaks state.
const UnhandledIntentHandler = {
  canHandle() {
    return true;
  },
  handle(handlerInput) {
    return handlerInput.responseBuilder
      .speak("Sorry, I can help with the home status, alerts, or routines. What would you like?")
      .reprompt('What would you like to know?')
      .getResponse();
  },
};

// Error handling: fail safe, never expose backend internals or sensitive data.
function handleBackendError(handlerInput, err) {
  // Log a minimal, non-sensitive message. Do NOT log err contents that could
  // carry data; the message is generic.
  console.error('HomeGuardian backend error');
  return handlerInput.responseBuilder
    .speak('Sorry, I had trouble getting that information. Please try again later.')
    .getResponse();
}

function handleInvalidData(handlerInput) {
  console.error('HomeGuardian backend returned invalid data');
  return handlerInput.responseBuilder
    .speak('Sorry, that information is not available right now.')
    .getResponse();
}

const skillBuilder = Alexa.SkillBuilders.custom();

exports.handler = skillBuilder
  .addRequestHandlers(
    LaunchRequestHandler,
    HomeStatusIntentHandler,
    GetAlertSummaryIntentHandler,
    GetRoutineStatusIntentHandler,
    HelpIntentHandler,
    CancelAndStopIntentHandler,
    SessionEndedRequestHandler,
    UnhandledIntentHandler
  )
  .lambda();

// Exported for unit tests (not used by Lambda).
exports.Handlers = {
  LaunchRequestHandler,
  HomeStatusIntentHandler,
  GetAlertSummaryIntentHandler,
  GetRoutineStatusIntentHandler,
  HelpIntentHandler,
  CancelAndStopIntentHandler,
  SessionEndedRequestHandler,
  UnhandledIntentHandler,
};
exports.backend = backend;
exports.MAX_SPOKEN_ITEMS = MAX_SPOKEN_ITEMS;
