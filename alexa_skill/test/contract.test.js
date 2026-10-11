'use strict';

/**
 * Adapter boundary tests for the HomeGuardian Alexa integration contract.
 *
 * These tests prove the JavaScript skill side can talk to the HomeGuardian
 * application through a versioned, authenticated, READ-ONLY contract WITHOUT a
 * deployed endpoint, AWS, real credentials, or network access. They use a mock
 * client that mirrors the C++ AlexaContract semantics (src/core/AlexaContract.*).
 *
 * They do NOT prove a live Alexa-to-application integration: that requires a
 * deployed authenticated endpoint and is documented as pending.
 *
 * Run with:  node --test test/contract.test.js
 */

const test = require('node:test');
const assert = require('node:assert');

const CONTRACT_VERSION = 1;
const MAX_REQUEST_BYTES = 4096;

// --- mock backend client mirroring the C++ contract ---

class MockBackendClient {
  constructor({ tokens, snapshot }) {
    this.tokens = tokens; // { token: { allow_status, allow_alert_summary, expires_at } }
    this.snapshot = snapshot; // { available, state, capture_enabled, alert_count, highest_severity, alert_ids }
  }

  // Returns { ok, error, ...fields }. Never throws; fails closed.
  request(body, now) {
    // Size bound.
    if (typeof body !== 'string' || body.length === 0 || body.length > MAX_REQUEST_BYTES) {
      return { ok: false, error: 'malformed' };
    }
    let j;
    try {
      j = JSON.parse(body);
    } catch {
      return { ok: false, error: 'malformed' };
    }
    if (typeof j !== 'object' || j === null || Array.isArray(j)) {
      return { ok: false, error: 'malformed' };
    }
    // Unknown fields rejected.
    const allowed = new Set(['version', 'operation', 'credential', 'request_id']);
    for (const k of Object.keys(j)) {
      if (!allowed.has(k)) return { ok: false, error: 'malformed' };
    }
    // Version.
    if (!Number.isInteger(j.version)) return { ok: false, error: 'malformed' };
    if (j.version !== CONTRACT_VERSION) return { ok: false, error: 'unsupported_version' };
    // Operation: read-only whitelist.
    if (j.operation !== 'status' && j.operation !== 'alert_summary') {
      return { ok: false, error: 'malformed' };
    }
    // Credential shape (contents verified below, never trusted by shape alone).
    if (typeof j.credential !== 'string' || j.credential.length === 0) {
      return { ok: false, error: 'malformed' };
    }
    // Authenticate.
    const entry = this.tokens[j.credential];
    if (!entry) return { ok: false, error: 'unauthorized' };
    if (now >= entry.expires_at) return { ok: false, error: 'unauthorized' };
    // Authorize the operation.
    const permitted =
      (j.operation === 'status' && entry.allow_status) ||
      (j.operation === 'alert_summary' && entry.allow_alert_summary);
    if (!permitted) return { ok: false, error: 'forbidden' };
    // Availability.
    if (!this.snapshot.available) return { ok: false, error: 'unavailable' };
    // Read-only response.
    const ids = this.snapshot.alert_ids.slice(0, 5);
    return {
      ok: true,
      error: null,
      state: this.snapshot.state,
      capture_enabled: this.snapshot.capture_enabled,
      alert_count: this.snapshot.alert_count,
      highest_severity: this.snapshot.highest_severity,
      alert_ids: ids,
    };
  }
}

function body(op, token) {
  return JSON.stringify({ version: CONTRACT_VERSION, operation: op, credential: token });
}

const NOW = Date.now();
const FAR = NOW + 365 * 24 * 3600 * 1000;

function makeClient(overrides = {}) {
  const snapshot = Object.assign(
    {
      available: true,
      state: 'Running',
      capture_enabled: false,
      alert_count: 0,
      highest_severity: '',
      alert_ids: [],
    },
    overrides.snapshot || {}
  );
  const tokens = Object.assign(
    {
      'tok-full': { allow_status: true, allow_alert_summary: true, expires_at: FAR },
      'tok-status': { allow_status: true, allow_alert_summary: false, expires_at: FAR },
      'tok-noperm': { allow_status: false, allow_alert_summary: false, expires_at: FAR },
      'tok-expired': { allow_status: true, allow_alert_summary: true, expires_at: NOW - 1000 },
    },
    overrides.tokens || {}
  );
  return new MockBackendClient({ tokens, snapshot });
}

// --- tests ---

test('valid authenticated status request returns read-only state', () => {
  const c = makeClient();
  const r = c.request(body('status', 'tok-full'), NOW);
  assert.equal(r.ok, true);
  assert.equal(r.state, 'Running');
  assert.equal(r.capture_enabled, false);
});

test('valid authenticated alert_summary returns aggregate only', () => {
  const c = makeClient({
    snapshot: { alert_count: 2, highest_severity: 'warning', alert_ids: ['a-1', 'a-2'] },
  });
  const r = c.request(body('alert_summary', 'tok-full'), NOW);
  assert.equal(r.ok, true);
  assert.equal(r.alert_count, 2);
  assert.equal(r.highest_severity, 'warning');
  assert.deepEqual(r.alert_ids, ['a-1', 'a-2']);
});

test('safe response when no alerts exist', () => {
  const c = makeClient();
  const r = c.request(body('alert_summary', 'tok-full'), NOW);
  assert.equal(r.ok, true);
  assert.equal(r.alert_count, 0);
  assert.equal(r.highest_severity, '');
  assert.deepEqual(r.alert_ids, []);
});

test('missing credential is rejected (malformed)', () => {
  const c = makeClient();
  const r = c.request(JSON.stringify({ version: CONTRACT_VERSION, operation: 'status' }), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'malformed');
});

test('invalid credential fails closed (unauthorized)', () => {
  const c = makeClient();
  const r = c.request(body('status', 'tok-bogus'), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'unauthorized');
});

test('expired credential fails closed (unauthorized)', () => {
  const c = makeClient();
  const r = c.request(body('status', 'tok-expired'), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'unauthorized');
});

test('authenticated but unauthorized operation is forbidden', () => {
  const c = makeClient();
  const r = c.request(body('alert_summary', 'tok-status'), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'forbidden');
});

test('authenticated principal with no permissions is forbidden', () => {
  const c = makeClient();
  const r = c.request(body('status', 'tok-noperm'), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'forbidden');
});

test('malformed and oversized requests are rejected', () => {
  const c = makeClient();
  assert.equal(c.request('', NOW).error, 'malformed');
  assert.equal(c.request('not json', NOW).error, 'malformed');
  assert.equal(c.request('[]', NOW).error, 'malformed');
  assert.equal(c.request('x'.repeat(MAX_REQUEST_BYTES + 1), NOW).error, 'malformed');
});

test('unsupported contract version is rejected distinctly', () => {
  const c = makeClient();
  const r = c.request(JSON.stringify({ version: 2, operation: 'status', credential: 'tok-full' }), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'unsupported_version');
});

test('application unavailable fails closed', () => {
  const c = makeClient({ snapshot: { available: false } });
  const r = c.request(body('status', 'tok-full'), NOW);
  assert.equal(r.ok, false);
  assert.equal(r.error, 'unavailable');
});

test('contract exposes no mutating operation', () => {
  const c = makeClient();
  for (const op of ['enable_capture', 'grant_consent', 'set_config', 'activate_sensor']) {
    assert.equal(c.request(body(op, 'tok-full'), NOW).error, 'malformed', op);
  }
});
