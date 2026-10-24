/**
 * test_oauth.js — Offline verification of the HomeGuardian cookieless OIDC gateway.
 *
 * SYNTHETIC FIXTURES ONLY. No network: the RSA keypair, the JWKS document and the
 * Google token endpoint are generated/stubbed in-process. The stub throws on any
 * URL it does not own, so an accidental live call fails the suite loudly.
 *
 * Ten required cases:
 *   1  valid ID token -> VIEWER session
 *   2  bad signature (and alg confusion: none / HS256)
 *   3  bad issuer, bad audience, bad azp
 *   4  expired (and not-yet-valid) token
 *   5  replayed state (and replayed one-time hand-off code)
 *   6  nonce mismatch / missing nonce
 *   7  anonymous access denied everywhere
 *   8  VIEWER mutation denied
 *   9  household isolation & default-deny of private data
 *  10  authorized MCP read, MCP unavailable honesty, logout & expiry
 *
 * Run: node web_mcp/test_oauth.js
 */

'use strict';

const crypto = require('node:crypto');
const http = require('node:http');
const path = require('node:path');

const { createGateway } = require('./gateway');
const { OidcConfig } = require('./oauth');

const CLIENT_ID = 'synthetic-client-id.apps.googleusercontent.com';
const ISSUER = 'https://accounts.google.com';
const MCP_TOKEN_SENTINEL = 'mcp-bearer-token-must-never-surface-9f3a';

let passed = 0;
let failed = 0;
const failures = [];

function ok(condition, label) {
  if (condition) {
    passed += 1;
    console.log(`  \u2713 ${label}`);
  } else {
    failed += 1;
    failures.push(label);
    console.error(`  \u2717 FAIL: ${label}`);
  }
}

function section(title) {
  console.log(`\n${title}`);
}

// ---------------------------------------------------------------------------
// Synthetic crypto: two RSA keypairs, one "Google" JWKS, one rogue key
// ---------------------------------------------------------------------------

const { privateKey, publicKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });
const rogue = crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });

const KEY_ID = 'synthetic-key-1';
const ROTATED_KEY_ID = 'synthetic-key-2';
const { privateKey: rotatedPrivateKey, publicKey: rotatedPublicKey } =
  crypto.generateKeyPairSync('rsa', { modulusLength: 2048 });

function publicJwk(key, kid) {
  const jwk = key.export({ format: 'jwk' });
  return { kty: jwk.kty, n: jwk.n, e: jwk.e, kid, use: 'sig', alg: 'RS256' };
}

function b64u(input) {
  return Buffer.from(input).toString('base64url');
}

function signJwt(payload, { key = privateKey, kid = KEY_ID, alg = 'RS256', typ = 'JWT' } = {}) {
  const header = { alg, kid, typ };
  const signingInput = `${b64u(JSON.stringify(header))}.${b64u(JSON.stringify(payload))}`;
  if (alg !== 'RS256') {
    // Deliberately malformed non-RS256 tokens (alg confusion probes).
    return `${signingInput}.${b64u('not-a-real-signature')}`;
  }
  const signature = crypto.sign('RSA-SHA256', Buffer.from(signingInput, 'utf8'), key);
  return `${signingInput}.${b64u(signature)}`;
}

// ---------------------------------------------------------------------------
// Stubbed upstream: JWKS document + token endpoint
// ---------------------------------------------------------------------------

const stub = {
  jwksKeys: [publicJwk(publicKey, KEY_ID)],
  tokens: new Map(), // authorization code -> id_token
  pending: { nonce: null, challenge: null },
  calls: { jwks: 0, token: 0 },
  tokenRequests: [],
  pkceVerified: null,
  unexpectedUrls: [],

  response(body, status = 200) {
    return {
      ok: status >= 200 && status < 300,
      status,
      json: async () => body,
      text: async () => JSON.stringify(body),
    };
  },

  async fetchImpl(url, init = {}) {
    const href = String(url);
    if (href.includes('/token')) {
      this.calls.token += 1;
      const params = new URLSearchParams(String(init.body || ''));
      this.tokenRequests.push(params);
      const verifier = params.get('code_verifier') || '';
      const derived = b64u(crypto.createHash('sha256').update(verifier, 'utf8').digest());
      this.pkceVerified = Boolean(this.pending.challenge) && derived === this.pending.challenge;
      const idToken = this.tokens.get(params.get('code'));
      if (!idToken) {
        return this.response({ error: 'invalid_grant' }, 400);
      }
      return this.response({
        access_token: 'synthetic-access-token',
        id_token: idToken,
        token_type: 'Bearer',
        expires_in: 3600,
      });
    }
    if (href.includes('certs') || href.includes('/jwks')) {
      this.calls.jwks += 1;
      return this.response({ keys: this.jwksKeys });
    }
    this.unexpectedUrls.push(href);
    throw new Error(`offline test suite refused unexpected upstream URL: ${href}`);
  },
};

// ---------------------------------------------------------------------------
// Fake native MCP backend (spy + failure switch)
// ---------------------------------------------------------------------------

const mcp = {
  available: true,
  calls: [],
  responses: {
    get_homeguardian_status: { state: 'RUNNING', capture_enabled: false, read_only: true },
    get_alert_summary: {
      alert_count: 3,
      highest_severity: 'high',
      alert_ids: ['alert-1', 'alert-2', 'alert-3'],
      read_only: true,
    },
  },
  async callTool(name) {
    this.calls.push(name);
    if (!this.available) {
      throw new Error(`connect ECONNREFUSED 127.0.0.1:8790 (Authorization: Bearer ${MCP_TOKEN_SENTINEL})`);
    }
    const payload = this.responses[name];
    if (!payload) throw new Error(`unknown tool ${name}`);
    return payload;
  },
};

// ---------------------------------------------------------------------------
// Clock (deterministic; drives session TTL, token exp and JWKS freshness)
// ---------------------------------------------------------------------------

const clock = { t: Date.parse('2026-10-24T00:00:00Z') };
const now = () => clock.t;

// ---------------------------------------------------------------------------
// HTTP client against the gateway
// ---------------------------------------------------------------------------

let BASE = '';
const cookieViolations = [];

function request(method, requestPath, { headers = {}, body = null, token = null } = {}) {
  return new Promise((resolve, reject) => {
    const url = new URL(requestPath, BASE);
    const payload = body === null ? null : Buffer.from(typeof body === 'string' ? body : JSON.stringify(body));
    const reqHeaders = { ...headers };
    if (payload) {
      reqHeaders['Content-Type'] = reqHeaders['Content-Type'] || 'application/json';
      reqHeaders['Content-Length'] = String(payload.length);
    }
    if (token) reqHeaders.Authorization = `Bearer ${token}`;

    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port,
        path: url.pathname + url.search,
        method,
        headers: reqHeaders,
        agent: false,
        timeout: 5000,
      },
      (res) => {
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => {
          const raw = Buffer.concat(chunks).toString('utf8');
          if (res.headers['set-cookie']) cookieViolations.push(`${method} ${url.pathname}`);
          let json = null;
          try {
            json = JSON.parse(raw);
          } catch (_) {
            json = null;
          }
          resolve({ status: res.statusCode, headers: res.headers, raw, json });
        });
      }
    );
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

// ---------------------------------------------------------------------------
// Flow helpers
// ---------------------------------------------------------------------------

let codeSeq = 0;

function claimsFor(nonce, overrides = {}) {
  const nowSeconds = Math.floor(clock.t / 1000);
  return {
    iss: ISSUER,
    aud: CLIENT_ID,
    azp: CLIENT_ID,
    sub: 'google-sub-alice',
    email: 'alice@example.test',
    email_verified: true,
    name: 'Alice Synthetic',
    iat: nowSeconds - 5,
    exp: nowSeconds + 600,
    nonce,
    ...overrides,
  };
}

const browserProofs = new Map();
const handoffProofs = new Map();
async function startLogin() {
  const verifier = crypto.randomBytes(32).toString('base64url');
  const challenge = crypto.createHash('sha256').update(verifier).digest('base64url');
  const started = await request('POST', '/api/login/start', {
    headers: { Origin: 'https://portal.example.test' }, body: { challenge } });
  const res = await request('GET', started.json?.login_url || '/login');
  const location = res.headers.location ? new URL(res.headers.location) : null;
  const state = location ? location.searchParams.get('state') : null;
  const nonce = location ? location.searchParams.get('nonce') : null;
  if (location) {
    browserProofs.set(state, verifier);
    stub.pending = { nonce, challenge: location.searchParams.get('code_challenge') };
  }
  return { res, location, state, nonce };
}

function queueToken(nonce, overrides = {}, signing = {}) {
  const code = `auth-code-${++codeSeq}`;
  stub.tokens.set(code, signJwt(claimsFor(nonce, overrides), signing));
  return code;
}

async function callback(state, code) {
  const res = await request(
    'GET',
    `/auth/callback?code=${encodeURIComponent(code)}&state=${encodeURIComponent(state)}`
  );
  const handoff = handoffCode(res);
  if (handoff) handoffProofs.set(handoff, browserProofs.get(state));
  return res;
}

function handoffCode(callbackResponse) {
  const location = callbackResponse.headers.location || '';
  const match = /^\/auth\/complete#session=([A-Za-z0-9_-]+)$/.exec(location);
  return match ? match[1] : null;
}

async function exchange(code) {
  return request('POST', '/api/session/exchange', { body: { code, verifier: handoffProofs.get(code) } });
}

/** Full happy-path login for a given subject. Returns the Bearer token. */
async function loginAs(sub, extraClaims = {}) {
  const start = await startLogin();
  const code = queueToken(start.nonce, { sub, ...extraClaims });
  const cb = await callback(start.state, code);
  const handoff = handoffCode(cb);
  const exchanged = await exchange(handoff);
  return { token: exchanged.json && exchanged.json.token, start, cb, exchanged };
}

// ---------------------------------------------------------------------------
// Suite
// ---------------------------------------------------------------------------

// Execute the actual served external script against a small synthetic DOM.
// No browser dependencies or persistent storage APIs are available in this realm.
async function testSessionUi(source) {
  const vm = require('node:vm');
  function boot(hash = '', live = false, granted = false, review = false) {
    const nodes = new Map();
    function element(tag) {
      return { tagName: tag, children: [], attributes: {}, textContent: '', hidden: false,
        setAttribute(k, v) { this.attributes[k] = v; },
        getAttribute(k) { return this.attributes[k]; },
        removeAttribute(k) { delete this[k]; delete this.attributes[k]; },
        appendChild(n) { this.children.push(n); if (n.id) nodes.set(n.id, n); return n; },
        addEventListener(k, fn) { this['on' + k] = fn; },
      };
    }
    const document = { documentElement: element('html'), head: element('head'),
      body: element('body'), readyState: 'loading', createElement: element,
      getElementById(id) { return nodes.get(id) || null; },
      querySelectorAll() { return ['runBtn','scenarioSel','uploadBtn','refreshBtn','camReconnect','camFullscreen'].map(id => nodes.get(id)).filter(Boolean); },
      addEventListener(k, fn) { var previous = this['on' + k]; this['on' + k] = function () { if (previous) previous(); fn(); }; },
    };
    for (const id of ['status','metrics','scenDesc','scenarioSel','camBadge','camBanner','camOverlay','camMetrics','integrations','alertCount','alerts','runBtn','uploadBtn','refreshBtn','camReconnect','camFullscreen','camFrame']) {
      const node = element('div'); node.id = id; node.textContent = '0'; document.body.appendChild(node);
    }
    const calls = [];
    const timers = new Map(); let timerId = 0;
    let status = 403;
    const location = { origin: 'https://portal.example.test', href: 'https://portal.example.test/' + hash,
      pathname: '/', search: '', hash, assign(url) { calls.push({ navigation: url }); } };
    const popup = {};
    const window = { dispatchEvent() {}, addEventListener(k, fn) { this['on' + k] = fn; },
      open() { return popup; }, fetch: async (url, init) => {
      calls.push({ url, init });
      if (live) {
        const res = await request(init?.method || 'GET', url, {
          headers: { ...(init?.headers instanceof Headers ? Object.fromEntries(init.headers) : init?.headers),
            Origin: location.origin }, body: init?.body || null });
        return { ok: res.status >= 200 && res.status < 300, status: res.status, json: async () => res.json };
      }
      if (url === '/api/login/start') return { ok: true, status: 200,
        json: async () => ({ login_url: '/login?request=' + 'z'.repeat(43) }) };
      if (url === '/api/session/exchange') return { ok: true, status: 200,
        json: async () => ({ token: 'a'.repeat(64), role: 'VIEWER', expires_in: 3600 }) };
      if (url === '/api/logout') return { ok: true, status: 200, json: async () => ({ ok: true }) };
      if (url === '/api/me') return { ok: true, status: 200, json: async () => ({ household_authorized: granted, outdoor_review_authorized: review }) };
      if (review && url === '/api/run-scenario?scenario=normal') return { ok: true, status: 200,
        json: async () => ({ synthetic: true, isolated: true, scenario: { ok: true, outcome: 'Synthetic normal fixture' } }) };
      if ((granted || review) && url === '/api/status') return { ok: true, status: 200,
        json: async () => ({ status: { state: 'Running', capture_enabled: true } }) };
      if ((granted || review) && url === '/api/camera/frame') return { ok: true, status: 200,
        blob: async () => new Blob([Buffer.from([255,216,255,217])], { type: 'image/jpeg' }) };
      if (granted && url === '/api/devices') return { ok: true, status: 200,
        json: async () => ({ device_count: 2, device_labels: ['Gate'], visibility: 'owner-approved-only' }) };
      if (granted && url === '/api/alerts') return { ok: true, status: 200,
        json: async () => ({ total: 3, highest_severity: 'high' }) };
      return { ok: status === 200, status, json: async () => ({ error: 'household_authorization_required' }) };
    } };
    vm.runInNewContext(source, { window, document, location, history: { replaceState() { location.hash = ''; } },
      URL, Headers, Date, Promise, crypto: crypto.webcrypto, TextEncoder, Uint8Array,
      setTimeout(fn, delay) { timers.set(++timerId, { fn, delay }); return timerId; },
      clearTimeout(id) { timers.delete(id); },
      btoa: s => Buffer.from(s, 'binary').toString('base64'), CustomEvent: function () {} });
    document.readyState = 'complete';
    if (document.onDOMContentLoaded) document.onDOMContentLoaded();
    return { window, document, nodes, calls, location, popup, timers,
      async tick(timeoutOnly = false) {
        const batch = [...timers.entries()].filter(([, t]) => timeoutOnly ? t.delay >= 600000 : t.delay < 600000);
        for (const [id, t] of batch) { timers.delete(id); await t.fn(); }
      },
      setStatus(s) { status = s; } };
  }
  async function signIn(realm) {
    await realm.nodes.get('hg-sign-in').onclick({ preventDefault() {} });
    realm.window.onmessage({ origin: realm.location.origin, source: realm.popup,
      data: { type: 'hg:oauth', code: 'x'.repeat(43) } });
    return realm;
  }
  const anon = boot();
  ok(anon.nodes.has('hg-session-gate'), 'external script creates the sign-in UI');
  ok(anon.nodes.get('hg-sign-in')?.href === '/login', 'sign-in button links to /login');
  ok(anon.document.head.children.some(n => n.textContent.includes('.wrap') && n.textContent.includes('display:none')),
    'dashboard visibility is fail-closed before original scripts run');
  const signed = await signIn(boot());
  // An original inline script starts polling immediately, before exchange resolves.
  const denied = await signed.window.fetch('/api/status').then(() => false, () => true);
  ok(denied, '403 rejects the original poll instead of allowing its false zero fallback');
  ok(signed.calls.find(c => c.url === '/api/status')?.init.headers.get('Authorization') === 'Bearer ' + 'a'.repeat(64),
    'first inline poll waits for exchange and carries the bearer token');
  ok(signed.nodes.get('hg-session-message')?.textContent.includes('VIEWER') &&
    signed.nodes.get('hg-session-message')?.textContent.includes('not granted'),
    'signed-in UI truthfully states VIEWER and household access not granted');
  ok(signed.document.documentElement.attributes['data-hg-auth'] === 'viewer' &&
    signed.document.head.children.some(n => n.textContent.includes('html:not([data-hg-auth="viewer"]) .wrap')),
    'signed-in VIEWER unlocks the original dashboard shell independently of household permission');
  ok(signed.nodes.get('runBtn').disabled && signed.nodes.get('uploadBtn').disabled &&
    signed.nodes.get('alertCount').textContent === 'Household access not granted',
    'VIEWER mutation controls disabled and ungranted counts never show fabricated zeros');
  await signed.window.fetch('/api/camera/status').catch(() => {});
  ok(signed.nodes.get('hg-session-message')?.textContent.includes('not granted'),
    'background private-feature polls preserve the household denial state');
  signed.setStatus(503);
  await signed.window.fetch('/api/status').catch(() => {});
  ok(signed.nodes.get('hg-session-message')?.textContent.includes('unavailable'), 'outage shows an explicit unavailable banner');
  signed.setStatus(200);
  await signed.window.fetch('/api/status');
  ok(signed.document.documentElement.attributes['data-hg-household'] !== 'granted', 'status success alone does not override /api/me household denial');
  const reviewed = await signIn(boot('', false, false, true));
  await reviewed.window.__hg.ready;
  ok(reviewed.document.documentElement.attributes['data-hg-household'] === 'granted' &&
    !reviewed.nodes.get('runBtn').disabled && !reviewed.nodes.get('scenarioSel').disabled &&
    reviewed.nodes.get('uploadBtn').disabled, 'judge review enables simulations but never camera management or uploads');
  ok(['camBadge','camBanner','camOverlay','camMetrics'].every(id => reviewed.nodes.get(id).textContent !== 'Household access not granted'),
    'successful outdoor authorization clears stale household-denial camera placeholders');
  reviewed.nodes.get('scenarioSel').value = 'normal';
  await reviewed.nodes.get('runBtn').onclick();
  const simulationRequest = reviewed.calls.find(c => c.url === '/api/run-scenario?scenario=normal');
  ok(simulationRequest && simulationRequest.init.method === 'POST' &&
    simulationRequest.init.headers.get('Authorization') === 'Bearer ' + 'a'.repeat(64) &&
    !simulationRequest.init.headers.has('X-HG-CSRF'), 'served simulation UI uses memory-only viewer bearer, not an operator secret');
  ok(reviewed.nodes.get('scenDesc').textContent.includes('isolated from live camera alerts'),
    'served simulation UI labels its result separately from physical observations');
  await reviewed.window.refreshIntegrations();
  ok(!reviewed.calls.some(c => c.url === '/api/devices') && reviewed.nodes.get('integrations').textContent.includes('restricted'),
    'judge UI does not request private device inventory or trigger a blanket household denial');
  await reviewed.window.fetch('/api/devices').catch(() => {});
  ok(reviewed.document.documentElement.attributes['data-hg-household'] === 'granted',
    'private inventory denial cannot revoke an already granted outdoor review view');
  await reviewed.window.__hg.logout();
  ok(!reviewed.window.__hg.authenticated(), 'judge logout revokes access without persistent storage');
  const granted = await signIn(boot('', false, true));
  await granted.window.__hg.ready;
  ok(granted.document.documentElement.attributes['data-hg-household'] === 'granted' &&
    !granted.nodes.get('refreshBtn').disabled && !granted.nodes.get('camFullscreen').disabled &&
    granted.nodes.get('runBtn').disabled && granted.nodes.get('uploadBtn').disabled,
    '/api/me owner grant unlocks read controls only after popup OAuth');
  await granted.window.camTickFrame();
  await granted.window.refreshIntegrations();
  await granted.window.refreshAlerts();
  granted.window.renderStatus({ state: 'Running', capture_enabled: true });
  ok(granted.nodes.get('camFrame').src?.startsWith('blob:') &&
    granted.nodes.get('integrations').textContent.includes('Device count: 2') &&
    granted.nodes.get('alerts').textContent.includes('Aggregate alert summary: 3') &&
    granted.nodes.get('status').textContent.includes('Running'),
    'granted dashboard renders actual MCP contract and fetches camera blob without operator secret');
  await granted.window.__hg.logout();
  ok(!granted.nodes.get('camFrame').src, 'logout removes household camera blob from dashboard');
  signed.setStatus(401);
  await signed.window.fetch('/api/status').catch(() => {});
  ok(!signed.window.__hg.authenticated(), '401 clears expired/revoked session and returns to sign-in');
  const switched = await signIn(boot());
  await switched.window.__hg.ready;
  await switched.nodes.get('hg-switch-account')?.onclick();
  const logoutIndex = switched.calls.findIndex(c => c.url === '/api/logout');
  const loginIndex = switched.calls.findIndex((c, i) => i > logoutIndex && c.url === '/api/login/start');
  ok(logoutIndex >= 0 && loginIndex > logoutIndex && !switched.window.__hg.authenticated(),
    'switch-account revokes session before starting a new browser-bound /login');
  const refreshed = boot();
  ok(!refreshed.window.__hg.authenticated(), 'refresh without hand-off loses token and requires login again');
  const sharedUrl = boot('#session=' + 'x'.repeat(43));
  await sharedUrl.window.__hg.ready;
  ok(!sharedUrl.window.__hg.authenticated() && !sharedUrl.calls.some(c => c.url === '/api/session/exchange'),
    'shared hand-off URLs never sign in a different browser/tab');
  const pending = boot();
  await pending.nodes.get('hg-sign-in').onclick({ preventDefault() {} });
  pending.setStatus(401);
  await pending.window.fetch('/api/camera/status').catch(() => {});
  for (const [origin, source] of [['https://attacker.example.test', pending.popup], [pending.location.origin, {}]]) {
    pending.window.onmessage({ origin, source, data: { type: 'hg:oauth', code: 'x'.repeat(43) } });
  }
  ok(!pending.calls.some(c => c.url === '/api/session/exchange'), 'popup hand-off checks both exact origin and exact source window');
  pending.window.onmessage({ origin: pending.location.origin, source: pending.popup,
    data: { type: 'hg:oauth', code: 'x'.repeat(43) } });
  await pending.window.__hg.ready;
  ok(pending.calls.some(c => c.url === '/api/session/exchange'),
    'anonymous background 401 polling cannot destroy the pending browser login proof');
  const startBody = JSON.parse(pending.calls.find(c => c.url === '/api/login/start').init.body);
  const exchangeBody = JSON.parse(pending.calls.find(c => c.url === '/api/session/exchange').init.body);
  ok(crypto.createHash('sha256').update(exchangeBody.verifier).digest('base64url') === startBody.challenge,
    'browser-memory verifier proves the challenge supplied before Google login');
  ok(!pending.popup.location.includes(exchangeBody.verifier), 'browser verifier never appears in popup URL');
  const timedOut = boot();
  await timedOut.nodes.get('hg-sign-in').onclick({ preventDefault() {} });
  await timedOut.tick(true);
  ok(timedOut.timers.size === 0 && timedOut.nodes.get('hg-session-message').textContent.includes('timed out') &&
    timedOut.calls.some(c => c.url === '/api/login/result' && JSON.parse(c.init.body).cancel),
    'independent login deadline cancels proof and timers even without a polling response');

  const severed = boot('', true);
  await severed.nodes.get('hg-sign-in').onclick({ preventDefault() {} });
  const login = await request('GET', severed.popup.location);
  const google = new URL(login.headers.location);
  stub.pending = { nonce: google.searchParams.get('nonce'), challenge: google.searchParams.get('code_challenge') };
  await severed.tick(); // real HTTP pending poll before Google completes
  const cb = await callback(google.searchParams.get('state'), queueToken(google.searchParams.get('nonce'), { sub: 'openerless-ungranted' }));
  const completeScript = await request('GET', '/assets/oauth-complete.js');
  const completionNode = { textContent: '' };
  let closed = false;
  const callbackLocation = { origin: severed.location.origin, pathname: '/auth/complete', hash: new URL(cb.headers.location, BASE).hash };
  vm.runInNewContext(completeScript.raw, {
    window: { opener: null, close() { closed = true; } }, location: callbackLocation,
    document: { getElementById() { return completionNode; } },
    history: { replaceState() { callbackLocation.hash = ''; } },
  });
  ok(closed && completionNode.textContent.includes('Sign-in complete'), 'callback with opener=null truthfully completes and attempts close');
  clock.t += 1001;
  await severed.tick();
  await severed.window.__hg.ready;
  ok(severed.window.__hg.authenticated() && severed.window.__hg.role() === 'VIEWER',
    'actual original-tab script retrieves real gateway result despite severed popup opener');
  ok(severed.nodes.get('hg-session-message').textContent.includes('VIEWER') &&
    severed.document.documentElement.attributes['data-hg-auth'] === 'viewer' &&
    severed.nodes.get('alertCount').textContent === 'Household access not granted',
    'openerless completion unlocks original dashboard shell with VIEWER restrictions and no zeros');
  await severed.window.__hg.logout();
  ok(!severed.window.__hg.authenticated() && severed.timers.size === 0,
    'logout clears browser session and stops polling timers');
}

async function run() {
  console.log('=========================================================');
  console.log('HomeGuardian cookieless OIDC gateway — offline test suite');
  console.log('JWKS: locally generated RSA keypair (synthetic)');
  console.log('Token endpoint: stubbed in-process (no network)');
  console.log('=========================================================');

  // ---------------------------------------------------------------- config --
  section('Config contract');
  const config = new OidcConfig({
    HG_GOOGLE_CLIENT_ID: CLIENT_ID,
    HG_GOOGLE_REDIRECT_URI: 'https://portal.example.test/auth/callback',
    HG_PUBLIC_ORIGIN: 'https://portal.example.test',
    HG_SESSION_TTL_SECONDS: '3600',
  });
  ok(config.isConfigured() === true, 'env-driven config is configured when client id + redirect uri are set');
  ok(config.jwksUrl === 'https://www.googleapis.com/oauth2/v3/certs', 'default JWKS URL is Google certs');
  ok(
    config.issuers.includes('accounts.google.com') && config.issuers.includes('https://accounts.google.com'),
    'default issuer allowlist accepts both documented Google forms'
  );
  const unconfigured = new OidcConfig({});
  ok(
    unconfigured.isConfigured() === false &&
      unconfigured.missingDependencies().includes('HG_GOOGLE_CLIENT_ID'),
    'missing client id is a reported owner dependency, not a crash'
  );

  const gateway = createGateway({
    config,
    fetchImpl: (url, init) => stub.fetchImpl(url, init),
    mcpClient: mcp,
    now,
    env: { HG_PORTAL_PATH: path.join(__dirname, '..', 'src', 'web', 'index.html') },
    logger: { info: () => {}, warn: () => {} },
  });
  const address = await gateway.listen(0, '127.0.0.1');
  BASE = `http://127.0.0.1:${address.port}`;
  ok(address.address === '127.0.0.1', 'gateway binds loopback only');

  // ------------------------------------------------------- unconfigured 503 --
  section('1. Owner dependency: Google client id missing');
  const bare = createGateway({
    config: unconfigured,
    fetchImpl: (url, init) => stub.fetchImpl(url, init),
    mcpClient: mcp,
    now,
    logger: { info: () => {}, warn: () => {} },
  });
  const bareAddress = await bare.listen(0, '127.0.0.1');
  const bareLogin = await new Promise((resolve, reject) => {
    const req = http.request(
      { hostname: '127.0.0.1', port: bareAddress.port, path: '/login', method: 'GET' },
      (res) => {
        let raw = '';
        res.on('data', (c) => (raw += c));
        res.on('end', () => resolve({ status: res.statusCode, json: JSON.parse(raw) }));
      }
    );
    req.on('error', reject);
    req.end();
  });
  ok(bareLogin.status === 503 && bareLogin.json.error === 'google_client_id_missing',
    '/login reports 503 google_client_id_missing instead of crashing');
  ok(Array.isArray(bareLogin.json.dependency) && bareLogin.json.dependency.length > 0,
    'unconfigured /login names the missing dependency');
  const bareHealth = await new Promise((resolve, reject) => {
    const req = http.request(
      { hostname: '127.0.0.1', port: bareAddress.port, path: '/health', method: 'GET' },
      (res) => {
        let raw = '';
        res.on('data', (c) => (raw += c));
        res.on('end', () => resolve({ status: res.statusCode, json: JSON.parse(raw) }));
      }
    );
    req.on('error', reject);
    req.end();
  });
  ok(bareHealth.status === 200 && bareHealth.json.oidc_configured === false,
    '/health stays honest about OIDC being unconfigured');
  await bare.close();

  section('Browser-proof rendezvous');
  const poll = (verifier, extra = {}, origin = 'https://portal.example.test') => request('POST', '/api/login/result', {
    headers: origin ? { Origin: origin } : {}, body: { verifier, ...extra } });
  const rendezvous = await startLogin();
  const proof = browserProofs.get(rendezvous.state);
  const duplicate = await request('POST', '/api/login/start', {
    headers: { Origin: 'https://portal.example.test' },
    body: { challenge: crypto.createHash('sha256').update(proof).digest('base64url') } });
  ok(duplicate.status === 409, 'duplicate public challenge cannot overwrite pending login');
  ok((await poll(proof)).status === 202, 'proof rendezvous is pending before callback');
  ok((await poll(proof)).status === 429, 'pending poll rate is bounded');
  clock.t += 1001;
  for (const origin of [null, 'https://attacker.example.test']) {
    ok((await poll(proof, {}, origin)).status === 403, 'result requires exact Origin');
  }
  const rendezvousCb = await callback(rendezvous.state, queueToken(rendezvous.nonce));
  const wrong = await poll('q'.repeat(43));
  ok(wrong.status === 400 && !wrong.json.token && !wrong.json.code, 'wrong verifier reveals no completion');
  const publicChallenge = crypto.createHash('sha256').update(proof).digest('base64url');
  ok((await poll(publicChallenge)).status === 400, 'public challenge cannot retrieve completion');
  const raced = await Promise.all([poll(proof), exchange(handoffCode(rendezvousCb))]);
  ok(raced.filter(r => r.status === 200).length === 1, 'poll and popup exchange have exactly one winner');
  const winner = raced.find(r => r.status === 200);
  ok(winner && (await request('GET', '/api/me', { token: winner.json.token })).status === 200,
    'benign double-completion cannot revoke the winning session');
  ok((await poll(proof)).status === 400, 'completion polling is one-use');
  const popupFirst = await startLogin();
  const popupCb = await callback(popupFirst.state, queueToken(popupFirst.nonce));
  const popupResult = await exchange(handoffCode(popupCb));
  ok(popupResult.status === 200 && (await poll(browserProofs.get(popupFirst.state))).status === 400 &&
    (await request('GET', '/api/me', { token: popupResult.json.token })).status === 200,
    'popup-first completion wins once and later polling cannot revoke its session');
  const completedExpiry = await startLogin();
  const completedCb = await callback(completedExpiry.state, queueToken(completedExpiry.nonce));
  clock.t += config.exchangeCodeTtlSeconds * 1000 + 1;
  ok((await poll(browserProofs.get(completedExpiry.state))).status === 400 &&
    (await exchange(handoffCode(completedCb))).status === 400, 'completed result expires on both delivery paths');
  const expiring = await startLogin();
  clock.t += 600001;
  ok((await poll(browserProofs.get(expiring.state))).status === 400, 'pending rendezvous expires');
  const cancelled = await startLogin();
  const cancelProof = browserProofs.get(cancelled.state);
  await poll(cancelProof, { cancel: true });
  ok((await poll(cancelProof)).status === 400, 'logout cancellation removes pending rendezvous');
  const cancelledCb = await callback(cancelled.state, queueToken(cancelled.nonce));
  ok(cancelledCb.status === 400, 'cancelled pending login cannot resurrect on callback');
  const replaced = await startLogin();
  const replacedProof = browserProofs.get(replaced.state);
  await poll(replacedProof, { cancel: true });
  const duplicateStart = await request('POST', '/api/login/start', {
    headers: { Origin: 'https://portal.example.test' },
    body: { challenge: crypto.createHash('sha256').update(replacedProof).digest('base64url') } });
  ok(duplicateStart.status === 200, 'cancelled rendezvous frees its bounded capacity');
  const oldCb = await callback(replaced.state, queueToken(replaced.nonce));
  ok(oldCb.status === 400, 'public challenge reuse cannot reattach a cancelled OAuth state');

  // --------------------------------------------------------------- /login ----
  section('2. Login redirect (PKCE S256 + state + nonce)');
  const first = await startLogin();
  ok(first.res.status === 302, '/login responds 302');
  ok(first.location.origin === 'https://accounts.google.com', 'login redirects to accounts.google.com');
  ok(first.location.searchParams.get('client_id') === CLIENT_ID, 'login sends the configured client id');
  ok(first.location.searchParams.get('response_type') === 'code', 'login uses the authorization code flow');
  ok(first.location.searchParams.get('code_challenge_method') === 'S256', 'PKCE uses S256');
  ok((first.location.searchParams.get('code_challenge') || '').length === 43,
    'PKCE challenge is a 43-char base64url SHA-256 digest');
  ok((first.location.searchParams.get('state') || '').length >= 32, 'state is present and high-entropy');
  ok((first.location.searchParams.get('nonce') || '').length >= 32, 'nonce is present and high-entropy');
  ok(first.location.searchParams.get('state') !== first.location.searchParams.get('nonce'),
    'state and nonce are independent values');
  ok(!first.location.toString().includes('client_secret'), 'client secret never appears in the redirect');
  ok((first.location.searchParams.get('scope') || '').includes('openid'), 'openid scope requested');

  // --------------------------------------------------- case 1: valid token --
  section('3. Case 1 — valid ID token yields a VIEWER session');
  ok(first.res.headers['set-cookie'] === undefined, 'no Set-Cookie on the login redirect');
  const goodCode = queueToken(first.nonce);
  const goodCb = await callback(first.state, goodCode);
  ok(goodCb.status === 302, 'callback 302 after full validation');
  const handoff = handoffCode(goodCb);
  ok(Boolean(handoff) && handoff.length >= 32, 'callback redirects to popup completion with a one-time fragment code');
  ok((goodCb.headers.location || '').includes('#session='), 'session hand-off uses a URL fragment only');

  const exchanged = await exchange(handoff);
  ok(exchanged.status === 200 && exchanged.json.ok === true, 'hand-off code exchanges for a session');
  const aliceToken = exchanged.json.token;
  ok(/^[0-9a-f]{64}$/.test(aliceToken || ''), 'session token is a 256-bit opaque hex value');
  ok(exchanged.json.role === 'VIEWER', 'exchanged session role is VIEWER');
  ok(exchanged.json.expires_in > 0 && exchanged.json.expires_in <= 3600, 'session reports a bounded TTL');
  ok(exchanged.json.token_type === 'Bearer', 'session token is presented as Authorization: Bearer');
  ok(exchanged.json.cookie_set === false, 'session exchange asserts no cookie was set');

  const replayExchange = await exchange(handoff);
  ok(replayExchange.status === 400 && replayExchange.json.error === 'invalid_or_used_code',
    'one-time hand-off code cannot be replayed');

  for (const verifier of [undefined, 'q'.repeat(43)]) {
    const start = await startLogin();
    const cb = await callback(start.state, queueToken(start.nonce));
    const code = handoffCode(cb);
    const denied = await request('POST', '/api/session/exchange', { body: { code, verifier } });
    ok(denied.status === 400 && denied.json.error === 'browser_proof_invalid' && !denied.json.token,
      `handoff rejects ${verifier ? 'wrong' : 'missing'} browser proof`);
    const retry = await exchange(code);
    ok(retry.status === 400 && retry.json.error === 'invalid_or_used_code', 'invalid proof consumes the handoff exactly once');
  }
  const directLogin = await request('GET', '/login');
  ok(directLogin.status === 400 && directLogin.json.error === 'browser_login_required', 'direct/shared login URL cannot start an unbound login');
  for (const origin of [undefined, 'https://attacker.example.test']) {
    const denied = await request('POST', '/api/login/start', { headers: origin ? { Origin: origin } : {},
      body: { challenge: 'a'.repeat(43) } });
    ok(denied.status === 403, 'login start rejects missing/cross-site Origin');
  }

  ok(stub.pkceVerified === true, 'token exchange sent a code_verifier matching the S256 challenge');
  ok(stub.tokenRequests[0].get('grant_type') === 'authorization_code', 'token exchange uses grant_type=authorization_code');
  ok(stub.tokenRequests[0].get('client_id') === CLIENT_ID, 'token exchange is server-side with the configured client id');

  const me = await request('GET', '/api/me', { token: aliceToken });
  ok(me.status === 200 && me.json.authenticated === true, '/api/me accepts the Bearer session');
  ok(me.json.sub === 'google-sub-alice', '/api/me maps to the stable Google sub');
  ok(me.json.role === 'VIEWER', '/api/me reports role VIEWER');
  ok(me.json.email === 'alice@example.test' && me.json.email_verified === true,
    'verified email is surfaced; unverified email would be dropped');
  ok(!me.raw.includes('id_token') && !me.raw.includes('access_token'), 'API responses never echo Google tokens');
  ok(!me.raw.includes(MCP_TOKEN_SENTINEL), 'API responses never echo the MCP bearer token');
  ok((me.headers['content-security-policy'] || '').includes("default-src 'self'"),
    'API responses carry the strict CSP');

  // claims can never elevate
  const adminish = await loginAs('google-sub-adminish', {
    role: 'ADMIN',
    email_verified: false,
    email: 'unverified@example.test',
  });
  const adminMe = await request('GET', '/api/me', { token: adminish.token });
  ok(adminMe.status === 200 && adminMe.json.role === 'VIEWER', 'a role claim in the ID token cannot elevate privileges');
  ok(adminMe.json.email === null, 'unverified email is not trusted');
  await request('POST', '/api/logout', { token: adminish.token });

  // Authentication alone never authorizes a private household.
  const callsBeforeDefaultDeny = mcp.calls.length;
  for (const route of ['/api/status', '/api/alerts']) {
    const denied = await request('GET', route, { token: aliceToken });
    ok(denied.status === 403 && denied.json.error === 'household_authorization_required',
      `${route} default denies a signed-in VIEWER without a household grant`);
    ok(!denied.raw.includes('alert-1') && !denied.raw.includes('RUNNING'),
      `${route} denial exposes no private household data`);
  }
  ok(mcp.calls.length === callsBeforeDefaultDeny, 'default household denials make zero backend calls');

  // Explicit server-side seam in this synthetic fixture only; no production grant.
  await gateway.close();
  const authorizedGateway = createGateway({
    config, sessions: gateway.sessions, provider: gateway.provider, codes: gateway.codes,
    fetchImpl: (url, init) => stub.fetchImpl(url, init), mcpClient: mcp, now,
    authorizeHousehold: async (sub) => sub === 'google-sub-alice',
    logger: { info: () => {}, warn: () => {} },
  });
  const authorizedAddress = await authorizedGateway.listen(0, '127.0.0.1');
  BASE = `http://127.0.0.1:${authorizedAddress.port}`;


  // ------------------------------------------- MCP reads via VIEWER session --
  section('4. Case 10a — authorized read-only MCP proxy');
  const statusRes = await request('GET', '/api/status', { token: aliceToken });
  ok(statusRes.status === 200 && statusRes.json.ok === true, '/api/status is served for VIEWER');
  ok(statusRes.json.backend === 'mcp' && statusRes.json.source === 'mcp', '/api/status is sourced from the MCP backend');
  ok(statusRes.json.status && statusRes.json.status.state === 'RUNNING', 'MCP status payload is passed through');
  ok(statusRes.json.role === 'VIEWER' && statusRes.json.scope === 'read_only', 'proxied reads are labelled VIEWER/read-only');
  ok(mcp.calls.includes('get_homeguardian_status'), 'the allowlisted status tool was actually called');
  ok(!statusRes.raw.includes(MCP_TOKEN_SENTINEL), 'proxied status response redacts the MCP token');

  const alertsRes = await request('GET', '/api/alerts', { token: aliceToken });
  ok(alertsRes.status === 200 && alertsRes.json.total === 3, '/api/alerts proxies the aggregate alert count');
  ok(alertsRes.json.highest_severity === 'high', '/api/alerts reports the highest severity');
  ok(!('alert_ids' in alertsRes.json) && !alertsRes.raw.includes('alert-1'),
    '/api/alerts never exposes household alert identifiers');
  ok(alertsRes.json.alerts.length === 0, '/api/alerts does not fabricate alert payloads');
  ok(!("message" in (alertsRes.json.alerts[0] || {})), 'no alert message bodies are exposed to VIEWER');
  ok(alertsRes.json.read_only === true, '/api/alerts is read-only');

  // ---------------------------------- case 8: VIEWER mutations are denied ----
  section('5. Case 8 — VIEWER mutation attempts are denied');
  const mcpCallsBeforeMutations = mcp.calls.length;
  const mutationAttempts = [
    ['POST', '/api/devices/cam-1/consent', { granted: true }],
    ['POST', '/api/mcp/scenario', { scenario: 'intrusion' }],
    ['POST', '/api/upload', { name: 'x.jpg' }],
    ['POST', '/api/delete', { id: 'x' }],
    ['PUT', '/api/devices/cam-1', { label: 'renamed' }],
    ['PATCH', '/api/me', { role: 'ADMIN' }],
  ];
  for (const [method, route, body] of mutationAttempts) {
    const res = await request(method, route, { token: aliceToken, body });
    ok(res.status === 403 && res.json.error === 'forbidden_mutation',
      `${method} ${route} is denied for VIEWER (403)`);
  }
  const wrongMethod = await request('POST', '/api/status', { token: aliceToken, body: {} });
  ok(wrongMethod.status === 405 && wrongMethod.json.error === 'method_not_allowed_read_only',
    'POST on a read-only proxy route is refused with 405');
  const deleteAlerts = await request('DELETE', '/api/alerts', { token: aliceToken });
  ok(deleteAlerts.status === 405 && deleteAlerts.json.error === 'method_not_allowed_read_only',
    'DELETE on an allowlisted read route is refused with 405');
  ok(mcp.calls.length === mcpCallsBeforeMutations, 'no mutation attempt ever reached the MCP backend');

  // ---------------------------- case 9: private data + household isolation ---
  section('6. Case 9 — private data default-deny and household isolation');
  const privateRoutes = [
    ['/api/feed', 'media_feed'],
    ['/api/consent', 'consent'],
    ['/api/scenarios', 'scenario_control'],
  ];
  const mcpCallsBeforePrivate = mcp.calls.length;
  for (const [route, reason] of privateRoutes) {
    const res = await request('GET', route, { token: aliceToken });
    ok(res.status === 403 && res.json.error === 'forbidden_private_data' && res.json.reason === reason,
      `GET ${route} is denied for VIEWER with reason ${reason}`);
  }
  const unknownApi = await request('GET', '/api/unknown-private-thing', { token: aliceToken });
  ok(unknownApi.status === 403 && unknownApi.json.error === 'not_allowlisted',
    'unknown /api routes default-deny instead of falling through');
  ok(mcp.calls.length === mcpCallsBeforePrivate, 'private route denials never touch the MCP backend');

  const bob = await loginAs('google-sub-bob', { email: 'bob@example.test', name: 'Bob Synthetic' });
  const bobMe = await request('GET', '/api/me', { token: bob.token });
  ok(bobMe.status === 200 && bobMe.json.sub === 'google-sub-bob', 'a second identity gets its own stable sub');
  const beforeBob = mcp.calls.length;
  for (const route of ['/api/status', '/api/alerts']) {
    const denied = await request('GET', route, { token: bob.token,
      headers: { 'x-hg-sub': 'google-sub-alice', 'x-hg-role': 'ADMIN' } });
    ok(denied.status === 403 && denied.json.error === 'household_authorization_required',
      `${route} rejects ungranted subject even with authorized-subject spoof headers`);
  }
  ok(mcp.calls.length === beforeBob, 'ungranted second subject makes zero backend calls');
  const aliceAgain = await request('GET', '/api/me', { token: aliceToken });
  ok(aliceAgain.json.sub === 'google-sub-alice', 'sessions are isolated per subject');
  const spoofed = await request('GET', '/api/me', {
    token: bob.token,
    headers: { 'x-hg-sub': 'google-sub-alice', 'x-hg-role': 'ADMIN' },
  });
  ok(spoofed.json.sub === 'google-sub-bob' && spoofed.json.role === 'VIEWER',
    'identity/role headers cannot override the validated session');
  ok(aliceToken !== bob.token, 'distinct logins produce distinct session tokens');

  // ------------------------------------------- case 2/3/4: token validation --
  section('7. Cases 2-4 — signature, issuer, audience, expiry');
  async function loginWithToken(signing, overrides = {}) {
    const start = await startLogin();
    const code = queueToken(start.nonce, overrides, signing);
    return callback(start.state, code);
  }

  const rogueRes = await loginWithToken({ key: rogue.privateKey });
  ok(rogueRes.status === 400 && rogueRes.json.error === 'invalid_signature',
    'a token signed by a foreign key is rejected (invalid_signature)');
  ok(!handoffCode(rogueRes), 'no session hand-off is produced for a bad signature');

  const algNone = await loginWithToken({ alg: 'none' });
  ok(algNone.status === 400 && algNone.json.error === 'unsupported_alg', 'alg=none is rejected');
  const algHs = await loginWithToken({ alg: 'HS256' });
  ok(algHs.status === 400 && algHs.json.error === 'unsupported_alg', 'alg=HS256 key-confusion is rejected');

  const badIssuer = await loginWithToken({}, { iss: 'https://evil.example.test' });
  ok(badIssuer.status === 400 && badIssuer.json.error === 'invalid_issuer', 'wrong issuer is rejected');
  const bareIssuerAlias = await loginWithToken({}, { iss: 'accounts.google.com' });
  ok(bareIssuerAlias.status === 302, 'the documented bare issuer form is accepted');
  if (handoffCode(bareIssuerAlias)) {
    const aliasToken = (await exchange(handoffCode(bareIssuerAlias))).json.token;
    await request('POST', '/api/logout', { token: aliasToken });
  }

  const badAudience = await loginWithToken({}, { aud: 'someone-elses-client-id' });
  ok(badAudience.status === 400 && badAudience.json.error === 'invalid_audience', 'wrong audience is rejected');
  const badAzp = await loginWithToken({}, { azp: 'someone-elses-client-id' });
  ok(badAzp.status === 400 && badAzp.json.error === 'invalid_authorized_party', 'wrong azp is rejected');
  const multiAudNoAzp = await loginWithToken({}, { aud: [CLIENT_ID, 'other'], azp: undefined });
  ok(multiAudNoAzp.status === 400, 'multi-audience token without azp is rejected');

  const expiredNow = Math.floor(clock.t / 1000) - 3600;
  const expired = await loginWithToken({}, { exp: expiredNow });
  ok(expired.status === 400 && expired.json.error === 'token_expired', 'expired ID token is rejected');
  const futureNbf = await loginWithToken({}, { nbf: Math.floor(clock.t / 1000) + 600 });
  ok(futureNbf.status === 400 && futureNbf.json.error === 'token_not_yet_valid', 'nbf in the future is rejected');
  const missingSub = await loginWithToken({}, { sub: '' });
  ok(missingSub.status === 400 && missingSub.json.error === 'invalid_subject', 'a token without a stable sub is rejected');

  // ------------------------------------------------ case 5/6: state, nonce ----
  section('8. Cases 5-6 — replayed state and nonce mismatch');
  const nonceStart = await startLogin();
  const goodNonceCode = queueToken(nonceStart.nonce);
  const goodNonceCb = await callback(nonceStart.state, goodNonceCode);
  ok(goodNonceCb.status === 302, 'baseline login for the replay test succeeds');
  const replayStateCode = queueToken(nonceStart.nonce);
  const replayed = await callback(nonceStart.state, replayStateCode);
  ok(replayed.status === 400 && replayed.json.error === 'invalid_state',
    'a replayed state is rejected (single-use state store)');
  const unknownState = await callback('not-a-state-we-issued', queueToken(nonceStart.nonce));
  ok(unknownState.status === 400 && unknownState.json.error === 'invalid_state',
    'an unknown state is rejected');
  const missingState = await callback('', queueToken(nonceStart.nonce));
  ok(missingState.status === 400, 'a callback without state is rejected');

  const nonceStart2 = await startLogin();
  const wrongNonceCode = queueToken('nonce-from-a-different-login');
  const wrongNonce = await callback(nonceStart2.state, wrongNonceCode);
  ok(wrongNonce.status === 400 && wrongNonce.json.error === 'nonce_mismatch',
    'a token bound to another nonce is rejected');
  const nonceStart3 = await startLogin();
  const noNonceClaims = claimsFor('placeholder');
  delete noNonceClaims.nonce;
  const noNonceCode = `auth-code-${++codeSeq}`;
  stub.tokens.set(noNonceCode, signJwt(noNonceClaims));
  const noNonce = await callback(nonceStart3.state, noNonceCode);
  ok(noNonce.status === 400 && noNonce.json.error === 'nonce_mismatch',
    'a token with no nonce claim is rejected');

  // ------------------------------------------------- case 7: anonymous ------
  section('9. Case 7 — anonymous access is denied');
  const mcpCallsBeforeAnon = mcp.calls.length;
  const anonymousResults = await Promise.all([
    request('GET', '/api/me'),
    request('GET', '/api/status'),
    request('GET', '/api/alerts'),
    request('GET', '/api/devices'),
    request('POST', '/api/logout'),
    request('GET', '/api/status', { token: 'f'.repeat(64) }),
  ]);
  for (const res of anonymousResults) {
    ok(res.status === 401 && res.json.error === 'unauthenticated', 'request without a valid session is 401');
  }
  ok((anonymousResults[0].headers['www-authenticate'] || '').startsWith('Bearer'),
    '401 responses advertise the Bearer scheme');
  ok(mcp.calls.length === mcpCallsBeforeAnon, 'anonymous requests cannot reach the MCP backend');

  // ------------------------------------------------- case 10b: MCP down -----
  section('10. Case 10b — backend unavailable is reported honestly');
  mcp.available = false;
  const downStatus = await request('GET', '/api/status', { token: aliceToken });
  ok(downStatus.status === 503, 'MCP outage surfaces as HTTP 503');
  ok(downStatus.json.status === 'unavailable' && downStatus.json.availability === 'unavailable',
    'outage body says unavailable');
  ok(downStatus.json.error === 'backend_unavailable', 'outage body names backend_unavailable');
  ok(!('data' in downStatus.json) && !('status_code' in downStatus.json),
    'outage body contains no fabricated data payload');
  ok(!downStatus.raw.includes(MCP_TOKEN_SENTINEL), 'outage detail redacts the MCP bearer token');

  const downAlerts = await request('GET', '/api/alerts', { token: aliceToken });
  ok(downAlerts.status === 503, 'alerts also 503 during an outage');
  ok(!('total' in downAlerts.json) && !('alert_count' in downAlerts.json),
    'alerts outage never reports a misleading zero count');

  mcp.available = true;
  const recovered = await request('GET', '/api/status', { token: aliceToken });
  ok(recovered.status === 200 && recovered.json.ok === true, 'reads recover once the backend is back');

  // -------------------------------------------------- case 10c: logout ------
  section('11. Case 10c — logout revokes the session server-side');
  const logout = await request('POST', '/api/logout', { token: aliceToken, body: {} });
  ok(logout.status === 200 && logout.json.logged_out === true, 'logout acknowledges revocation');
  const afterLogoutMe = await request('GET', '/api/me', { token: aliceToken });
  ok(afterLogoutMe.status === 401, 'revoked token can no longer read /api/me');
  const afterLogoutStatus = await request('GET', '/api/status', { token: aliceToken });
  ok(afterLogoutStatus.status === 401, 'revoked token can no longer read proxied MCP data');
  ok(logout.headers['set-cookie'] === undefined, 'logout does not set a cookie');

  // ------------------------------------------------------- expiry -----------
  section('12. Case 10d — session expiry');
  const carol = await loginAs('google-sub-carol', { email: 'carol@example.test' });
  const beforeExpiry = await request('GET', '/api/me', { token: carol.token });
  ok(beforeExpiry.status === 200, 'fresh session works before its TTL elapses');
  clock.t += 3601 * 1000; // one second past the 1 hour TTL
  const expiredMe = await request('GET', '/api/me', { token: carol.token });
  ok(expiredMe.status === 401, 'expired session is refused after the TTL');
  const expiredStatus = await request('GET', '/api/status', { token: carol.token });
  ok(expiredStatus.status === 401, 'expired session cannot read MCP data');

  // -------------------------------------------- JWKS rotation (kid change) ---
  section('13. JWKS rotation — unknown kid is refreshed, not trusted');
  const rotatedRes = await loginWithToken({ key: rotatedPrivateKey, kid: ROTATED_KEY_ID });
  ok(rotatedRes.status === 400 && rotatedRes.json.error === 'unknown_signing_key',
    'a token signed with an unpublished kid is rejected before rotation');
  stub.jwksKeys = [publicJwk(publicKey, KEY_ID), publicJwk(rotatedPublicKey, ROTATED_KEY_ID)];
  clock.t += 31 * 1000; // past the minimum refresh interval
  const rotatedStart = await startLogin();
  const rotatedCode = queueToken(rotatedStart.nonce, {}, { key: rotatedPrivateKey, kid: ROTATED_KEY_ID });
  const rotatedOk = await callback(rotatedStart.state, rotatedCode);
  ok(rotatedOk.status === 302, 'after JWKS rotation the new kid verifies');
  ok(stub.calls.jwks >= 2, 'JWKS was refetched to resolve the rotated kid');

  // ---------------------------------------------- browser/static surface ----
  section('14. Portal page, CSP and cookieless session shim');
  const page = await request('GET', '/');
  ok(page.status === 200 && page.raw.includes('HomeGuardian'), 'GET / serves the HomeGuardian portal');
  ok(page.raw.includes('/assets/portal-session.js'), 'the served page loads the in-memory session shim');
  ok(!page.raw.includes('localStorage') && !page.raw.includes('sessionStorage'),
    'the served page uses no browser storage');
  ok(!page.raw.includes(MCP_TOKEN_SENTINEL), 'no MCP credential is embedded in the page');
  const csp = page.headers['content-security-policy'] || '';
  ok(csp.includes("default-src 'self'"), 'CSP defaults to same-origin only');
  ok(csp.includes('frame-ancestors none') || csp.includes("frame-ancestors 'none'"), 'CSP forbids framing');
  ok(csp.includes("object-src 'none'"), 'CSP forbids plugins/objects');
  const scriptSrc = (csp.match(/script-src [^;]+/) || [''])[0];
  ok(!scriptSrc.includes("'unsafe-inline'"), 'CSP script-src forbids inline scripts without hashes');
  ok((scriptSrc.match(/sha256-/g) || []).length >= 2, 'CSP pins the page inline scripts by hash');
  ok(page.headers['x-content-type-options'] === 'nosniff', 'nosniff is set on the portal');
  ok(page.headers['referrer-policy'] === 'no-referrer', 'referrer policy is no-referrer');
  ok(page.headers['set-cookie'] === undefined, 'portal response sets no cookie');

  const shimTag = page.raw.match(/<script src="\/assets\/portal-session.js"[^>]*><\/script>/);
  ok(shimTag && !/defer|async/.test(shimTag[0]) &&
    page.raw.indexOf(shimTag[0]) < page.raw.indexOf('<script>'),
    'session bootstrap is parser-blocking before every original inline script');

  const shim = await request('GET', '/assets/portal-session.js');
  ok(shim.status === 200 && (shim.headers['content-type'] || '').includes('javascript'),
    'the session shim is served as JavaScript');
  ok(!shim.raw.includes('document.cookie') && !shim.raw.includes('localStorage'),
    'the shim never touches cookies or browser storage');
  ok(shim.raw.includes('credentials: \'omit\'') || shim.raw.includes("credentials: 'omit'"),
    'shim requests are cookieless by construction');
  ok(shim.raw.includes('/api/session/exchange'), 'the shim exchanges the one-time fragment code');

  await testSessionUi(shim.raw);
  const completion = await request('GET', '/assets/oauth-complete.js');
  const messages = [];
  const callbackRealm = {
    location: { origin: 'https://portal.example.test', pathname: '/auth/complete', hash: '#session=' + 'x'.repeat(43) },
    history: { replaceState() {} },
    window: { opener: { postMessage(...args) { messages.push(args); } }, close() {} },
    document: { getElementById() { return { textContent: '' }; } },
  };
  require('node:vm').runInNewContext(completion.raw, callbackRealm);
  ok(messages.length === 1 && messages[0][1] === 'https://portal.example.test',
    'callback popup posts handoff only to exact same-origin opener');
  callbackRealm.window.opener = null;
  require('node:vm').runInNewContext(completion.raw, callbackRealm);
  ok(messages.length === 1, 'standalone callback performs no session exchange and has no sign-in target');

  // ------------------------------------------------------------ accounting --
  section('15. Global invariants');
  clock.t += 600001;
  const capacityStatuses = [];
  for (let i = 0; i < 257; i++) {
    const started = await request('POST', '/api/login/start', {
      headers: { Origin: 'https://portal.example.test' },
      body: { challenge: crypto.randomBytes(32).toString('base64url') } });
    capacityStatuses.push(started.status);
  }
  ok(capacityStatuses.filter(s => s === 200).length === 256 && capacityStatuses[256] === 429,
    'rendezvous capacity is strictly bounded to 256 live entries');
  ok(cookieViolations.length === 0, `no response in the whole suite set a cookie (${cookieViolations.length} found)`);
  ok(stub.unexpectedUrls.length === 0, 'no request left the synthetic JWKS/token stubs (offline)');
  ok(gateway.sessions.size >= 0, 'session store is inspectable for audit');
  ok(typeof gateway.csp === 'string' && gateway.csp.length > 0, 'gateway exposes its CSP for audit');

  await authorizedGateway.close();
  await testProductionHousehold(config, gateway.provider, now);

  console.log('\n=========================================================');
  console.log(`Assertions: ${passed + failed}   passed: ${passed}   failed: ${failed}`);
  if (failed > 0) {
    console.log('Failures:');
    for (const label of failures) console.log(`  - ${label}`);
  }
  console.log('=========================================================');
  process.exit(failed === 0 ? 0 : 1);
}

async function testProductionHousehold(config, provider, now) {
  section('16. Production protected-file household grants and private bridge');
  const fs = require('node:fs');
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || process.cwd(), 'hg-grant-test-'));
  const grantPath = path.join(dir, 'subjects.json'), secretPath = path.join(dir, 'bridge.secret');
  const subject = '123456789012345678901'; // Synthetic, never a real identity.
  const grant = { household_id: 'home', subjects: [subject], read_only: true };
  function writeGrant(value = grant, mode = 0o600) {
    fs.rmSync(grantPath, { force: true });
    fs.writeFileSync(grantPath, typeof value === 'string' ? value : JSON.stringify(value), { mode });
  }
  writeGrant();
  const secret = 'synthetic-private-bridge-secret';
  fs.writeFileSync(secretPath, secret + '\n', { mode: 0o600 });
  let bridgeMode = 'normal', configured = true;
  const received = [];
  const jpeg = Buffer.from([0xff, 0xd8, 0xff, 0xd9]);
  const bridge = http.createServer((req, res) => {
    received.push({ url: req.url, headers: req.headers });
    if (req.headers['x-hg-secret'] !== secret) { res.writeHead(401); res.end(); return; }
    res.setHeader('Set-Cookie', 'private=never-forward');
    res.setHeader('X-Backend-Token', secret);
    if (bridgeMode === 'error') { res.writeHead(500); res.end(secret); return; }
    if (bridgeMode === 'redirect') { res.writeHead(302, { Location: 'http://example.invalid/' }); res.end(); return; }
    if (req.url === '/api/camera/status') {
      res.setHeader('Content-Type', 'application/json');
      res.end(JSON.stringify({ configured, camera: { state: 'LIVE', width: 640, height: 480,
        locator: 'rtsp://user:password@private/', secret }, secret }));
    } else {
      res.setHeader('Content-Type', bridgeMode === 'wrongtype' ? 'text/html' : 'image/jpeg');
      res.end(bridgeMode === 'oversized' ? Buffer.alloc(512 * 1024 + 1) : jpeg);
    }
  });
  await new Promise(resolve => bridge.listen(0, '127.0.0.1', resolve));
  const env = { HG_HOUSEHOLD_SUBJECTS_FILE: grantPath,
    HG_PRIVATE_BRIDGE_URL: `http://127.0.0.1:${bridge.address().port}`,
    HG_PRIVATE_BRIDGE_SECRET_FILE: secretPath };
  const calls = [];
  const g = createGateway({ config, provider, now, env, mcp: { async callTool(name) {
    calls.push(name);
    if (name === 'get_homeguardian_devices') return { device_count: 2, device_labels: ['Gate', 'Hall'], visibility: 'visible' };
    return name === 'get_alert_summary' ? { alert_count: 3, highest_severity: 'high' }
      : { state: 'RUNNING', capture_enabled: true };
  } }, logger: { info() {}, warn() {} } });
  const address = await g.listen(); BASE = `http://127.0.0.1:${address.port}`;
  const signedIn = await loginAs(subject);
  const token = signedIn.token;
  ok(signedIn.exchanged.status === 200 && signedIn.exchanged.json.role === 'VIEWER',
    'full verified OAuth login reaches production protected-file authorizer');
  const other = g.sessions.create({ sub: subject + '0', role: 'VIEWER', email: 'owner@example.test', email_verified: true }).token;
  try {
    const me = await request('GET', '/api/me', { token });
    ok(me.json.household_authorized === true && me.json.role === 'VIEWER' && !me.raw.includes(grantPath),
      'production exact numeric subject grants household read without role elevation or file disclosure');
    ok((await request('GET', '/api/me', { token: other })).json.household_authorized === false,
      'subject prefix and email never match the exact grant');
    for (const route of ['/api/status','/api/alerts','/api/devices','/api/camera/status','/api/camera/frame']) {
      ok((await request('GET', route, { token })).status === 200, `protected production grant permits ${route}`);
      ok((await request('GET', route, { token: other })).status === 403, `different Google subject denied ${route}`);
      ok((await request('GET', route)).status === 401, `anonymous denied ${route}`);
    }
    const devices = await request('GET', '/api/devices', { token });
    ok(devices.json.device_count === 2 && devices.json.device_labels[0] === 'Gate' && calls.includes('get_homeguardian_devices'),
      'device visibility is sourced from the native MCP tool contract');
    const camera = await request('GET', '/api/camera/status', { token,
      headers: { Cookie: 'browser=private', 'X-HG-Secret': 'attacker', Authorization: 'Bearer ' + token } });
    ok(camera.json.camera.width === 640 && !camera.raw.includes(secret) && !camera.raw.includes('rtsp:') &&
      !camera.headers['set-cookie'] && !camera.headers['x-backend-token'], 'camera status is shaped JSON only, no secrets or upstream cookies');
    ok(received.every(r => r.headers['x-hg-secret'] === secret && !r.headers.authorization && !r.headers.cookie),
      'bridge secret stays server-side; browser cookies and bearer never reach bridge');
    await request('GET', '/api/camera/frame?t=123', { token });
    ok(received.at(-1).url === '/api/camera/frame', 'frame cache-buster not forwarded upstream');
    configured = false;
    const absent = await request('GET', '/api/camera/status', { token });
    ok(absent.status === 200 && absent.json.configured === false && !absent.json.camera, 'no camera configured remains honest');
    configured = true;
    for (const mode of ['error','redirect','wrongtype','oversized']) {
      bridgeMode = mode;
      const result = await request('GET', '/api/camera/frame', { token });
      ok(result.status === 503 && result.json.status === 'unavailable' && !result.raw.includes(secret), `camera ${mode} fails unavailable without leaking backend data`);
    }
    bridgeMode = 'normal';
    const before = received.length;
    ok((await request('GET', '/api/camera/status?url=http://example.invalid', { token })).status === 400 && received.length === before,
      'caller cannot choose a bridge URL or path');
    for (const route of ['/api/feed','/api/consent','/api/scenarios','/api/upload','/api/admin','/api/integrations']) {
      ok((await request('GET', route, { token })).status === 403, `household grant does not authorize ${route}`);
    }
    ok((await request('POST', '/api/camera/status', { token, body: {} })).status === 405, 'camera bridge is GET-only');
    for (const invalid of [
      {}, { ...grant, read_only: false }, { ...grant, household_id: 'other' },
      { ...grant, subjects: ['owner@example.test'] }, { ...grant, subjects: [123] },
      { ...grant, subjects: [' ' + subject] }, { ...grant, subjects: ['1'.repeat(256)] },
      { ...grant, subjects: [subject], allow_all: true }, '{broken', ' '.repeat(16385),
    ]) {
      writeGrant(invalid);
      ok((await request('GET', '/api/status', { token })).status === 403, 'malformed/unsafe production grant fails closed');
    }
    for (const mode of [0o644,0o400,0o660,0o700]) {
      writeGrant(grant, mode);
      ok((await request('GET', '/api/me', { token })).json.household_authorized === false, `grant mode ${mode.toString(8)} rejected`);
    }
    writeGrant(); fs.renameSync(grantPath, grantPath + '.real'); fs.symlinkSync(grantPath + '.real', grantPath);
    ok((await request('GET', '/api/status', { token })).status === 403, 'symlink grant rejected');
    writeGrant();
    ok((await request('GET', '/api/status', { token })).status === 200, 'valid fresh grant restores existing session read');
    writeGrant({ ...grant, subjects: [] });
    const backendBefore = calls.length + received.length;
    for (const route of ['/api/status','/api/alerts','/api/devices','/api/camera/status','/api/camera/frame']) {
      ok((await request('GET', route, { token })).status === 403, `live revocation denies ${route} without re-login`);
    }
    ok(calls.length + received.length === backendBefore, 'revoked requests never touch MCP or bridge');
    fs.rmSync(grantPath);
    ok((await request('GET', '/api/me', { token })).json.household_authorized === false, 'missing grant revokes /api/me');
    writeGrant(); fs.chmodSync(secretPath, 0o644);
    ok((await request('GET', '/api/camera/status', { token })).status === 503, 'unsafe bridge secret file fails closed');
  } finally {
    await g.close(); await new Promise(resolve => bridge.close(resolve));
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

run().catch((err) => {
  console.error('\nTest suite crashed:', err);
  process.exit(1);
});
