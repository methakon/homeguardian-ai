'use strict';

/**
 * Unit tests for the HomeGuardian Alexa skill handlers.
 *
 * These run WITHOUT any deployed endpoint, without AWS, and without the Alexa
 * simulator: they invoke the handlers directly with synthesized Alexa request
 * envelopes. They cover supported intents, malformed/invalid requests, backend
 * error responses, and session-ended handling.
 *
 * Run with:  node test/handler.test.js   (Node.js built-in test runner)
 */

const test = require('node:test');
const assert = require('node:assert');

const {
  Handlers,
  backend,
} = require('../index.js');

// --- helpers to synthesize request envelopes and a minimal responseBuilder ---

function responseBuilder() {
  const res = {
    _spoken: '',
    _reprompt: '',
    _shouldEndSession: null,
    speak(text) {
      this._spoken = text;
      return this;
    },
    reprompt(text) {
      this._reprompt = text;
      return this;
    },
    withShouldEndSession(flag) {
      this._shouldEndSession = flag;
      return this;
    },
    getResponse() {
      return {
        outputSpeech: this._spoken ? { type: 'SSML', ssml: `<speak>${this._spoken}</speak>` } : undefined,
        reprompt: this._reprompt ? { outputSpeech: { type: 'SSML', ssml: `<speak>${this._reprompt}</speak>` } } : undefined,
        shouldEndSession: this._shouldEndSession,
      };
    },
  };
  return res;
}

function handlerInputFor(request) {
  // The ASK SDK exposes responseBuilder as a property (not a function) on
  // handlerInput, so handlers can call handlerInput.responseBuilder.speak(...).
  return { requestEnvelope: { request }, responseBuilder: responseBuilder() };
}

function launchRequest() {
  return { type: 'LaunchRequest', requestId: 'r-launch', timestamp: '2026-10-10T00:00:00Z', locale: 'en-US' };
}
function intentRequest(name) {
  return {
    type: 'IntentRequest',
    requestId: 'r-intent',
    timestamp: '2026-10-10T00:00:00Z',
    locale: 'en-US',
    intent: { name, confirmationStatus: 'NONE' },
  };
}
function sessionEndedRequest() {
  return { type: 'SessionEndedRequest', requestId: 'r-end', timestamp: '2026-10-10T00:00:00Z', locale: 'en-US', reason: 'USER_INITIATED' };
}

// --- tests ---

test('LaunchRequest responds with a welcome and a reprompt', () => {
  const hi = handlerInputFor(launchRequest());
  assert.ok(Handlers.LaunchRequestHandler.canHandle(hi));
  const res = Handlers.LaunchRequestHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /Home Guardian/i);
  assert.ok(res.reprompt, 'launch should reprompt to keep the session open');
});

test('HomeStatusIntent reports status and states capture is disabled', async () => {
  const hi = handlerInputFor(intentRequest('HomeStatusIntent'));
  assert.ok(Handlers.HomeStatusIntentHandler.canHandle(hi));
  const res = await Handlers.HomeStatusIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /running/i);
  // Safety: the response must never imply a camera/microphone is active.
  assert.match(res.outputSpeech.ssml, /disabled/i);
});

test('GetAlertSummaryIntent handles the zero-alert mock', async () => {
  const hi = handlerInputFor(intentRequest('GetAlertSummaryIntent'));
  assert.ok(Handlers.GetAlertSummaryIntentHandler.canHandle(hi));
  const res = await Handlers.GetAlertSummaryIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /no active alerts/i);
});

test('GetRoutineStatusIntent handles the zero-routine mock', async () => {
  const hi = handlerInputFor(intentRequest('GetRoutineStatusIntent'));
  assert.ok(Handlers.GetRoutineStatusIntentHandler.canHandle(hi));
  const res = await Handlers.GetRoutineStatusIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /no configured routines/i);
});

test('AMAZON.HelpIntent provides help and a reprompt', () => {
  const hi = handlerInputFor(intentRequest('AMAZON.HelpIntent'));
  assert.ok(Handlers.HelpIntentHandler.canHandle(hi));
  const res = Handlers.HelpIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /home status|alert|routine/i);
  assert.ok(res.reprompt);
});

test('AMAZON.StopIntent ends with a goodbye', () => {
  const hi = handlerInputFor(intentRequest('AMAZON.StopIntent'));
  assert.ok(Handlers.CancelAndStopIntentHandler.canHandle(hi));
  const res = Handlers.CancelAndStopIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /goodbye/i);
});

test('AMAZON.CancelIntent ends with a goodbye', () => {
  const hi = handlerInputFor(intentRequest('AMAZON.CancelIntent'));
  assert.ok(Handlers.CancelAndStopIntentHandler.canHandle(hi));
  const res = Handlers.CancelAndStopIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /goodbye/i);
});

test('SessionEndedRequest is handled without speech', () => {
  const hi = handlerInputFor(sessionEndedRequest());
  assert.ok(Handlers.SessionEndedRequestHandler.canHandle(hi));
  const res = Handlers.SessionEndedRequestHandler.handle(hi);
  assert.strictEqual(res.outputSpeech, undefined);
});

test('Unknown intent falls back to a safe help response', () => {
  const hi = handlerInputFor(intentRequest('SomeUnknownIntent'));
  // The specific handlers must NOT claim it.
  assert.ok(!Handlers.HomeStatusIntentHandler.canHandle(hi));
  assert.ok(!Handlers.GetAlertSummaryIntentHandler.canHandle(hi));
  assert.ok(!Handlers.GetRoutineStatusIntentHandler.canHandle(hi));
  // The catch-all MUST handle it.
  assert.ok(Handlers.UnhandledIntentHandler.canHandle(hi));
  const res = Handlers.UnhandledIntentHandler.handle(hi);
  assert.match(res.outputSpeech.ssml, /home status|alert|routine/i);
});

test('Malformed request envelope is handled by the catch-all without leaking state', () => {
  const hi = handlerInputFor({ type: 'Totally.Bogus.Request', intent: { name: 'x' } });
  assert.ok(Handlers.UnhandledIntentHandler.canHandle(hi));
  const res = Handlers.UnhandledIntentHandler.handle(hi);
  assert.ok(res.outputSpeech);
  // Must not contain any raw backend/exception text.
  assert.doesNotMatch(res.outputSpeech.ssml, /Error|undefined|object/i);
});

test('Backend error path returns a generic apology, not internals', async () => {
  const hi = handlerInputFor(intentRequest('HomeStatusIntent'));
  const orig = backend.getStatus.bind(backend);
  backend.getStatus = async () => {
    throw new Error('SECRET_SHOULD_NOT_LEAK: connection string pw=hunter2');
  };
  try {
    const res = await Handlers.HomeStatusIntentHandler.handle(hi);
    assert.match(res.outputSpeech.ssml, /trouble|try again/i);
    assert.doesNotMatch(res.outputSpeech.ssml, /SECRET_SHOULD_NOT_LEAK|hunter2|Error/i);
  } finally {
    backend.getStatus = orig;
  }
});

test('Backend invalid-data path returns a safe message', async () => {
  const hi = handlerInputFor(intentRequest('GetAlertSummaryIntent'));
  const orig = backend.getAlertSummary.bind(backend);
  backend.getAlertSummary = async () => ({ count: 'not-a-number' });
  try {
    const res = await Handlers.GetAlertSummaryIntentHandler.handle(hi);
    assert.match(res.outputSpeech.ssml, /not available/i);
  } finally {
    backend.getAlertSummary = orig;
  }
});

test('Mock backend exposes no capture capability and no sensitive detail', async () => {
  const status = await backend.getStatus();
  assert.strictEqual(status.capture_enabled, false);
  const alerts = await backend.getAlertSummary();
  assert.strictEqual(alerts.count, 0);
  assert.strictEqual(alerts.highest_severity, null);
  assert.deepStrictEqual(alerts.alerts, []);
  const routines = await backend.getRoutineStatus();
  assert.strictEqual(routines.count, 0);
  assert.deepStrictEqual(routines.routines, []);
});
