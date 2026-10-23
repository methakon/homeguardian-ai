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

async function startLogin() {
  const res = await request('GET', '/login');
  const location = res.headers.location ? new URL(res.headers.location) : null;
  const state = location ? location.searchParams.get('state') : null;
  const nonce = location ? location.searchParams.get('nonce') : null;
  if (location) {
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
  return request(
    'GET',
    `/auth/callback?code=${encodeURIComponent(code)}&state=${encodeURIComponent(state)}`
  );
}

function handoffCode(callbackResponse) {
  const location = callbackResponse.headers.location || '';
  const match = /^\/#session=([A-Za-z0-9_-]+)$/.exec(location);
  return match ? match[1] : null;
}

async function exchange(code) {
  return request('POST', '/api/session/exchange', { body: { code } });
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
  ok(Boolean(handoff) && handoff.length >= 32, 'callback redirects to /#session=<one-time code>');
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
  ok(Array.isArray(alertsRes.json.alert_ids) && alertsRes.json.alert_ids.length === 3,
    '/api/alerts exposes only capped, non-sensitive alert ids');
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
    ['/api/devices', 'device_identities'],
    ['/api/feed', 'media_feed'],
    ['/api/camera/frame', 'camera_frames'],
    ['/api/camera/status', 'camera_state'],
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

  const shim = await request('GET', '/assets/portal-session.js');
  ok(shim.status === 200 && (shim.headers['content-type'] || '').includes('javascript'),
    'the session shim is served as JavaScript');
  ok(!shim.raw.includes('document.cookie') && !shim.raw.includes('localStorage'),
    'the shim never touches cookies or browser storage');
  ok(shim.raw.includes('credentials: \'omit\'') || shim.raw.includes("credentials: 'omit'"),
    'shim requests are cookieless by construction');
  ok(shim.raw.includes('/api/session/exchange'), 'the shim exchanges the one-time fragment code');

  // ------------------------------------------------------------ accounting --
  section('15. Global invariants');
  ok(cookieViolations.length === 0, `no response in the whole suite set a cookie (${cookieViolations.length} found)`);
  ok(stub.unexpectedUrls.length === 0, 'no request left the synthetic JWKS/token stubs (offline)');
  ok(gateway.sessions.size >= 0, 'session store is inspectable for audit');
  ok(typeof gateway.csp === 'string' && gateway.csp.length > 0, 'gateway exposes its CSP for audit');

  await gateway.close();

  console.log('\n=========================================================');
  console.log(`Assertions: ${passed + failed}   passed: ${passed}   failed: ${failed}`);
  if (failed > 0) {
    console.log('Failures:');
    for (const label of failures) console.log(`  - ${label}`);
  }
  console.log('=========================================================');
  process.exit(failed === 0 ? 0 : 1);
}

run().catch((err) => {
  console.error('\nTest suite crashed:', err);
  process.exit(1);
});
