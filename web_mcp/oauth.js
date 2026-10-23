// HomeGuardian AI — Cookieless Google OpenID Connect (Authorization Code + PKCE)
//
// Design constraints (owner-mandated):
//   - No cookies, no localStorage/sessionStorage. The browser holds the gateway
//     session token in memory only and presents it as `Authorization: Bearer`.
//   - The ID token is validated in full: RS256 signature against the JWKS,
//     issuer, audience, azp, exp, nbf, iat and nonce. Claims never grant roles.
//   - Every authenticated principal is role VIEWER. Private resources default-deny.
//   - The Google client *secret* (when configured) is used server-side only and is
//     never placed in a redirect, a response body, a log line or the browser.
//
// Nothing in this module performs network I/O until a login or token exchange is
// actually requested, so it is safe to require from tests and from --check paths.

'use strict';

const crypto = require('node:crypto');

const ROLE_VIEWER = 'VIEWER';
const DEFAULT_ISSUERS = Object.freeze(['accounts.google.com', 'https://accounts.google.com']);
const GOOGLE_AUTHORIZATION_ENDPOINT = 'https://accounts.google.com/o/oauth2/v2/auth';
const GOOGLE_TOKEN_ENDPOINT = 'https://oauth2.googleapis.com/token';
const GOOGLE_JWKS_URL = 'https://www.googleapis.com/oauth2/v3/certs';
const GOOGLE_SCOPES = 'openid email profile';

const MIN_CODE_VERIFIER_BYTES = 32; // 43 base64url chars, RFC 7636 minimum length
const MAX_TOKEN_CHARS = 8192;

/** Typed OIDC failure. `status` is the HTTP status the gateway should use. */
class OidcError extends Error {
  constructor(code, message, status = 400) {
    super(message || code);
    this.name = 'OidcError';
    this.code = code;
    this.status = status;
  }
}

function isPlainObject(value) {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** base64url encode (no padding) — accepts string | Buffer | TypedArray. */
function base64urlEncode(input) {
  return Buffer.from(input).toString('base64url');
}

/** Strict base64url decode. Rejects padded/non-URL alphabets and empty input. */
function base64urlDecode(value) {
  if (typeof value !== 'string' || value.length === 0 || value.length > MAX_TOKEN_CHARS) {
    throw new OidcError('malformed_token', 'base64url segment is missing or oversized');
  }
  if (!/^[A-Za-z0-9_-]+$/.test(value)) {
    throw new OidcError('malformed_token', 'base64url segment contains illegal characters');
  }
  return Buffer.from(value, 'base64url');
}

/**
 * Constant-time comparison of two strings. Both sides are hashed first so the
 * comparison is independent of input length (no length oracle on state/nonce).
 */
function constantTimeEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  const left = crypto.createHash('sha256').update(a, 'utf8').digest();
  const right = crypto.createHash('sha256').update(b, 'utf8').digest();
  return crypto.timingSafeEqual(left, right);
}

function intFromEnv(raw, fallback, min, max) {
  const parsed = Number.parseInt(String(raw ?? ''), 10);
  if (!Number.isFinite(parsed)) return fallback;
  if (parsed < min) return min;
  if (parsed > max) return max;
  return parsed;
}

function splitIssuers(raw) {
  if (raw === undefined || raw === null || String(raw).trim() === '') return DEFAULT_ISSUERS.slice();
  const list = String(raw)
    .split(/[\s,]+/)
    .map((entry) => entry.trim())
    .filter(Boolean);
  return list.length > 0 ? list : DEFAULT_ISSUERS.slice();
}

/**
 * Env contract. Every value has a documented default; a missing client id is a
 * *reported owner dependency* (`isConfigured() === false`), never a crash.
 */
class OidcConfig {
  constructor(env = {}) {
    const source = env || {};
    this.clientId = String(source.HG_GOOGLE_CLIENT_ID || '').trim();
    // Optional: Google "Web application" clients normally require it. PKCE is
    // still enforced either way. Never leaves the server.
    this.clientSecret = String(source.HG_GOOGLE_CLIENT_SECRET || '').trim();
    this.publicOrigin = String(source.HG_PUBLIC_ORIGIN || '')
      .trim()
      .replace(/\/+$/, '');
    this.redirectUri =
      String(source.HG_GOOGLE_REDIRECT_URI || '').trim() ||
      (this.publicOrigin ? `${this.publicOrigin}/auth/callback` : '');
    this.jwksUrl = String(source.HG_GOOGLE_JWKS_URL || '').trim() || GOOGLE_JWKS_URL;
    this.issuers = splitIssuers(source.HG_GOOGLE_ISSUERS);
    this.authorizationEndpoint =
      String(source.HG_GOOGLE_AUTH_URL || '').trim() || GOOGLE_AUTHORIZATION_ENDPOINT;
    this.tokenEndpoint = String(source.HG_GOOGLE_TOKEN_URL || '').trim() || GOOGLE_TOKEN_ENDPOINT;
    this.scopes = GOOGLE_SCOPES;

    this.sessionTtlSeconds = intFromEnv(source.HG_SESSION_TTL_SECONDS, 3600, 60, 12 * 3600);
    this.loginStateTtlSeconds = intFromEnv(source.HG_LOGIN_STATE_TTL_SECONDS, 600, 60, 3600);
    this.exchangeCodeTtlSeconds = intFromEnv(source.HG_EXCHANGE_CODE_TTL_SECONDS, 60, 10, 300);
    this.clockSkewSeconds = intFromEnv(source.HG_OIDC_CLOCK_SKEW_SECONDS, 60, 0, 300);
    this.jwksTtlSeconds = intFromEnv(source.HG_GOOGLE_JWKS_TTL_SECONDS, 3600, 60, 86400);
    this.jwksMinRefreshSeconds = intFromEnv(source.HG_GOOGLE_JWKS_MIN_REFRESH_SECONDS, 30, 0, 300);
    this.httpTimeoutMs = intFromEnv(source.HG_OIDC_HTTP_TIMEOUT_MS, 8000, 500, 30000);
  }

  /** True only when a browser login can actually be started and completed. */
  isConfigured() {
    return Boolean(this.clientId && this.redirectUri);
  }

  /** Owner dependencies that are still missing — reported, never fatal. */
  missingDependencies() {
    const missing = [];
    if (!this.clientId) missing.push('HG_GOOGLE_CLIENT_ID');
    if (!this.redirectUri) missing.push('HG_GOOGLE_REDIRECT_URI or HG_PUBLIC_ORIGIN');
    return missing;
  }

  /** Secret-free snapshot for /api/me, logs and diagnostics. */
  describe() {
    return {
      configured: this.isConfigured(),
      missing: this.missingDependencies(),
      client_id_configured: Boolean(this.clientId),
      client_secret_configured: Boolean(this.clientSecret),
      redirect_uri: this.redirectUri || null,
      jwks_url: this.jwksUrl,
      issuers: this.issuers.slice(),
      session_ttl_seconds: this.sessionTtlSeconds,
      scopes: this.scopes,
      auth_method: 'oidc_authorization_code_pkce_s256',
      cookie_based: false,
    };
  }
}

/**
 * Single-use, TTL-bounded store for OIDC login requests (state/nonce/PKCE).
 * `take()` always removes the entry, so a replayed state can never validate.
 */
class LoginStateStore {
  constructor({ ttlMs = 10 * 60 * 1000, now = Date.now, maxEntries = 512 } = {}) {
    this.ttlMs = ttlMs;
    this.now = now;
    this.maxEntries = maxEntries;
    this.entries = new Map();
  }

  issue(entry) {
    this._sweep();
    if (this.entries.size >= this.maxEntries) {
      // Drop the oldest entry rather than grow without bound under a login flood.
      const oldest = this.entries.keys().next();
      if (!oldest.done) this.entries.delete(oldest.value);
    }
    const now = this.now();
    const record = { ...entry, createdAt: now, expiresAt: now + this.ttlMs, consumed: false };
    this.entries.set(entry.state, record);
    return record;
  }

  /** Single-use lookup. Returns null for unknown, expired, consumed or missing state. */
  take(state) {
    if (typeof state !== 'string' || state.length === 0) return null;
    const record = this.entries.get(state);
    if (!record) return null;
    this.entries.delete(state);
    if (record.consumed) return null;
    if (this.now() > record.expiresAt) return null;
    record.consumed = true;
    return record;
  }

  get size() {
    this._sweep();
    return this.entries.size;
  }

  _sweep() {
    const now = this.now();
    for (const [key, record] of this.entries) {
      if (now > record.expiresAt) this.entries.delete(key);
    }
  }
}

/**
 * JWKS cache: kid-indexed RSA public keys, TTL refresh, and one rate-limited
 * refresh when an unknown kid appears (Google key rotation). A failed refresh
 * keeps serving the previous key set until it expires.
 */
class JwksCache {
  constructor({ jwksUrl, fetchJwks, ttlMs = 60 * 60 * 1000, minRefreshIntervalMs = 30 * 1000, now = Date.now } = {}) {
    if (typeof fetchJwks !== 'function') throw new Error('JwksCache requires a fetchJwks function');
    this.jwksUrl = jwksUrl;
    this.fetchJwks = fetchJwks;
    this.ttlMs = ttlMs;
    this.minRefreshIntervalMs = minRefreshIntervalMs;
    this.now = now;
    this.keys = null;
    this.fetchedAt = 0;
    this.loads = 0;
    this.pending = null;
  }

  clear() {
    this.keys = null;
    this.fetchedAt = 0;
  }

  async _refresh() {
    if (this.pending) return this.pending;
    this.pending = (async () => {
      const doc = await this.fetchJwks();
      const keys = JwksCache.parseJwks(doc);
      this.keys = keys;
      this.fetchedAt = this.now();
      this.loads += 1;
      return keys;
    })();
    try {
      return await this.pending;
    } finally {
      this.pending = null;
    }
  }

  static parseJwks(doc) {
    if (!isPlainObject(doc) || !Array.isArray(doc.keys)) {
      throw new OidcError('jwks_invalid', 'JWKS document has no keys array', 502);
    }
    const keys = new Map();
    doc.keys.forEach((jwk, index) => {
      if (!isPlainObject(jwk)) return;
      if (jwk.kty !== 'RSA') return;
      if (jwk.use && jwk.use !== 'sig') return;
      if (jwk.alg && jwk.alg !== 'RS256') return;
      if (typeof jwk.n !== 'string' || typeof jwk.e !== 'string') return;
      let keyObject;
      try {
        keyObject = crypto.createPublicKey({ key: { kty: 'RSA', n: jwk.n, e: jwk.e }, format: 'jwk' });
      } catch (_) {
        return;
      }
      keys.set(typeof jwk.kid === 'string' && jwk.kid ? jwk.kid : `key-${index}`, keyObject);
    });
    if (keys.size === 0) {
      throw new OidcError('jwks_invalid', 'JWKS contained no usable RS256 signing key', 502);
    }
    return keys;
  }

  /** Resolve a signing key. Unknown kid triggers at most one rate-limited refresh. */
  async getKey(kid) {
    const now = this.now();
    const stale = this.keys === null || now - this.fetchedAt >= this.ttlMs;
    if (stale) await this._refresh();

    if (typeof kid === 'string' && kid.length > 0) {
      let key = this.keys.get(kid);
      if (!key && this.now() - this.fetchedAt >= this.minRefreshIntervalMs) {
        await this._refresh();
        key = this.keys.get(kid);
      }
      if (!key) throw new OidcError('unknown_signing_key', `no JWKS key for kid ${kid}`);
      return key;
    }

    // Tokens without a kid are only acceptable when the set is unambiguous.
    if (this.keys.size === 1) return [...this.keys.values()][0];
    throw new OidcError('missing_signing_key_id', 'ID token header has no kid and JWKS is ambiguous');
  }
}

/**
 * Google OIDC provider: PKCE authorization requests, server-side code exchange,
 * and full ID-token validation. All I/O goes through the injected `fetchImpl`,
 * which is what makes the offline test suite possible.
 */
class OidcProvider {
  constructor({ config, jwks, fetchImpl, now, randomBytes, states } = {}) {
    if (!config) throw new Error('OidcProvider requires a config');
    this.config = config;
    this.now = now || Date.now;
    this.fetchImpl = fetchImpl || ((url, init) => globalThis.fetch(url, init));
    this._randomBytes = randomBytes || ((n) => crypto.randomBytes(n));
    this.states =
      states ||
      new LoginStateStore({ ttlMs: config.loginStateTtlSeconds * 1000, now: this.now });
    this.jwks =
      jwks ||
      new JwksCache({
        jwksUrl: config.jwksUrl,
        ttlMs: config.jwksTtlSeconds * 1000,
        minRefreshIntervalMs: config.jwksMinRefreshSeconds * 1000,
        now: this.now,
        fetchJwks: () => this.fetchJwksDocument(),
      });
  }

  _timeoutSignal() {
    if (typeof AbortSignal !== 'undefined' && typeof AbortSignal.timeout === 'function') {
      return AbortSignal.timeout(this.config.httpTimeoutMs);
    }
    return undefined;
  }

  async _request(url, init) {
    let response;
    try {
      response = await this.fetchImpl(url, init);
    } catch (err) {
      throw new OidcError('upstream_unreachable', `OIDC upstream unreachable: ${err.message}`, 502);
    }
    if (!response || response.ok !== true) {
      const status = response && response.status;
      throw new OidcError('upstream_error', `OIDC upstream returned status ${status}`, 502);
    }
    return response;
  }

  async fetchJwksDocument() {
    const response = await this._request(this.config.jwksUrl, {
      method: 'GET',
      headers: { accept: 'application/json' },
      signal: this._timeoutSignal(),
    });
    if (typeof response.json !== 'function') {
      throw new OidcError('jwks_invalid', 'JWKS response is not JSON', 502);
    }
    let doc;
    try {
      doc = await response.json();
    } catch (_) {
      throw new OidcError('jwks_invalid', 'JWKS response body is not valid JSON', 502);
    }
    return doc;
  }

  /**
   * Build a PKCE S256 authorization request. State/nonce/verifier stay
   * server-side; only state, nonce and the S256 challenge go to Google.
   */
  createLoginRequest({ prompt = 'select_account' } = {}) {
    if (!this.config.isConfigured()) {
      throw new OidcError('google_client_id_missing', 'Google OIDC is not configured', 503);
    }
    const state = base64urlEncode(this._randomBytes(32));
    const nonce = base64urlEncode(this._randomBytes(32));
    const codeVerifier = base64urlEncode(this._randomBytes(MIN_CODE_VERIFIER_BYTES));
    const codeChallenge = base64urlEncode(crypto.createHash('sha256').update(codeVerifier, 'utf8').digest());

    this.states.issue({
      state,
      nonce,
      codeVerifier,
      redirectUri: this.config.redirectUri,
      codeChallenge,
    });

    const url = new URL(this.config.authorizationEndpoint);
    url.searchParams.set('client_id', this.config.clientId);
    url.searchParams.set('redirect_uri', this.config.redirectUri);
    url.searchParams.set('response_type', 'code');
    url.searchParams.set('scope', this.config.scopes);
    url.searchParams.set('state', state);
    url.searchParams.set('nonce', nonce);
    url.searchParams.set('code_challenge', codeChallenge);
    url.searchParams.set('code_challenge_method', 'S256');
    if (prompt) url.searchParams.set('prompt', prompt);

    return { url: url.toString(), state, nonce, codeVerifier, codeChallenge };
  }

  /** Single-use consumption of a login state. Returns null when invalid. */
  consumeState(state) {
    return this.states.take(state);
  }

  /** Server-side authorization-code exchange (never in the browser). */
  async exchangeCode({ code, codeVerifier, redirectUri }) {
    if (typeof code !== 'string' || code.length === 0 || code.length > 4096) {
      throw new OidcError('authorization_code_invalid', 'authorization code is missing or malformed');
    }
    if (typeof codeVerifier !== 'string' || codeVerifier.length < 43) {
      throw new OidcError('pkce_verifier_invalid', 'PKCE code verifier is missing or too short');
    }

    const form = new URLSearchParams({
      grant_type: 'authorization_code',
      code,
      client_id: this.config.clientId,
      redirect_uri: redirectUri || this.config.redirectUri,
      code_verifier: codeVerifier,
    });
    if (this.config.clientSecret) form.set('client_secret', this.config.clientSecret);

    const response = await this._request(this.config.tokenEndpoint, {
      method: 'POST',
      headers: {
        'content-type': 'application/x-www-form-urlencoded',
        accept: 'application/json',
      },
      body: form.toString(),
      signal: this._timeoutSignal(),
    });

    let payload;
    try {
      payload = await response.json();
    } catch (_) {
      throw new OidcError('token_response_invalid', 'token endpoint returned non-JSON', 502);
    }
    if (!isPlainObject(payload)) {
      throw new OidcError('token_response_invalid', 'token endpoint returned an unexpected payload', 502);
    }
    if (payload.error) {
      // Never echo token material; the error code itself is safe to surface.
      throw new OidcError('token_exchange_failed', `token endpoint error: ${String(payload.error).slice(0, 64)}`, 502);
    }
    if (typeof payload.id_token !== 'string' || payload.id_token.length === 0) {
      throw new OidcError('id_token_missing', 'token endpoint did not return an id_token', 502);
    }
    return payload;
  }

  /** Verify signature + claims. `expectedNonce` binds the token to this login. */
  async verifyIdToken(idToken, { expectedNonce } = {}) {
    if (typeof idToken !== 'string' || idToken.length === 0 || idToken.length > MAX_TOKEN_CHARS * 2) {
      throw new OidcError('malformed_token', 'ID token is missing or oversized');
    }
    const parts = idToken.split('.');
    if (parts.length !== 3) throw new OidcError('malformed_token', 'ID token must have three segments');

    let header;
    let claims;
    try {
      header = JSON.parse(base64urlDecode(parts[0]).toString('utf8'));
      claims = JSON.parse(base64urlDecode(parts[1]).toString('utf8'));
    } catch (err) {
      if (err instanceof OidcError) throw err;
      throw new OidcError('malformed_token', 'ID token header or payload is not valid JSON');
    }
    if (!isPlainObject(header) || !isPlainObject(claims)) {
      throw new OidcError('malformed_token', 'ID token header and payload must be objects');
    }

    // Algorithm allowlist first: reject `none`, HS*, and any key-confusion attempt.
    if (header.alg !== 'RS256') {
      throw new OidcError('unsupported_alg', `unsupported ID token alg: ${String(header.alg)}`);
    }
    if (header.crit !== undefined) {
      throw new OidcError('unsupported_crit', 'ID token declares unsupported critical headers');
    }

    const key = await this.jwks.getKey(typeof header.kid === 'string' ? header.kid : null);
    const signature = base64urlDecode(parts[2]);
    let signatureValid = false;
    try {
      signatureValid = crypto.verify(
        'RSA-SHA256',
        Buffer.from(`${parts[0]}.${parts[1]}`, 'utf8'),
        key,
        signature
      );
    } catch (_) {
      signatureValid = false;
    }
    if (!signatureValid) throw new OidcError('invalid_signature', 'ID token signature is invalid');

    const nowSeconds = Math.floor(this.now() / 1000);
    const skew = this.config.clockSkewSeconds;

    if (!this.config.issuers.includes(claims.iss)) {
      throw new OidcError('invalid_issuer', `untrusted ID token issuer: ${String(claims.iss)}`);
    }

    const audiences = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
    if (!audiences.some((aud) => typeof aud === 'string' && aud === this.config.clientId)) {
      throw new OidcError('invalid_audience', 'ID token audience does not match this client');
    }
    if (claims.azp !== undefined && claims.azp !== this.config.clientId) {
      throw new OidcError('invalid_authorized_party', 'ID token azp does not match this client');
    }
    if (Array.isArray(claims.aud) && claims.aud.length > 1 && claims.azp === undefined) {
      throw new OidcError('invalid_authorized_party', 'multi-audience ID token is missing azp');
    }

    if (typeof claims.exp !== 'number' || !Number.isFinite(claims.exp)) {
      throw new OidcError('invalid_expiry', 'ID token has no numeric exp');
    }
    if (nowSeconds - skew >= claims.exp) throw new OidcError('token_expired', 'ID token has expired');

    if (claims.nbf !== undefined) {
      if (typeof claims.nbf !== 'number' || !Number.isFinite(claims.nbf)) {
        throw new OidcError('invalid_not_before', 'ID token nbf is not numeric');
      }
      if (nowSeconds + skew < claims.nbf) throw new OidcError('token_not_yet_valid', 'ID token is not valid yet');
    }
    if (claims.iat !== undefined) {
      if (typeof claims.iat !== 'number' || !Number.isFinite(claims.iat)) {
        throw new OidcError('invalid_issued_at', 'ID token iat is not numeric');
      }
      if (claims.iat > nowSeconds + skew) throw new OidcError('invalid_issued_at', 'ID token iat is in the future');
    }

    if (typeof claims.sub !== 'string' || claims.sub.length === 0 || claims.sub.length > 255) {
      throw new OidcError('invalid_subject', 'ID token has no stable subject');
    }
    if (expectedNonce !== undefined) {
      if (typeof claims.nonce !== 'string' || !constantTimeEqual(claims.nonce, expectedNonce)) {
        throw new OidcError('nonce_mismatch', 'ID token nonce does not match this login');
      }
    }

    return claims;
  }

  /** Full callback path: state -> verifier -> exchange -> verify -> principal. */
  async completeLogin({ code, state }) {
    const login = this.consumeState(state);
    if (!login) throw new OidcError('invalid_state', 'login state is unknown, expired or already used');

    const tokens = await this.exchangeCode({
      code,
      codeVerifier: login.codeVerifier,
      redirectUri: login.redirectUri,
    });
    const claims = await this.verifyIdToken(tokens.id_token, { expectedNonce: login.nonce });
    return { claims, principal: principalFromClaims(claims), tokens };
  }
}

/**
 * Map validated claims to a gateway principal. Role is hard-coded VIEWER:
 * no claim (`role`, `hd`, `email`, group, ...) can elevate privileges.
 * Email is only trusted when Google marks it verified; otherwise it is dropped.
 */
function principalFromClaims(claims) {
  const emailVerified = claims.email_verified === true;
  const email =
    emailVerified && typeof claims.email === 'string' && claims.email.length <= 254
      ? claims.email
      : null;
  const name = typeof claims.name === 'string' ? claims.name.slice(0, 120) : null;
  const picture =
    typeof claims.picture === 'string' && /^https:\/\//.test(claims.picture)
      ? claims.picture.slice(0, 512)
      : null;

  return {
    sub: String(claims.sub),
    role: ROLE_VIEWER,
    email,
    emailVerified,
    name,
    picture,
    issuer: claims.iss,
    household: null, // single-household deployment: no cross-household claim is honored
  };
}

/** Serialize a validated principal for API responses (no raw ID token, ever). */
function publicPrincipal(principal, session, now = Date.now()) {
  return {
    authenticated: true,
    sub: principal.sub,
    role: principal.role,
    scope: 'read_only',
    email: principal.email,
    email_verified: principal.emailVerified,
    name: principal.name,
    picture: principal.picture,
    issuer: principal.issuer,
    expires_in: session ? Math.max(0, Math.floor((session.expiresAt - now) / 1000)) : 0,
  };
}

module.exports = {
  ROLE_VIEWER,
  DEFAULT_ISSUERS,
  GOOGLE_JWKS_URL,
  GOOGLE_TOKEN_ENDPOINT,
  GOOGLE_AUTHORIZATION_ENDPOINT,
  OidcError,
  OidcConfig,
  OidcProvider,
  JwksCache,
  LoginStateStore,
  base64urlEncode,
  base64urlDecode,
  constantTimeEqual,
  principalFromClaims,
  publicPrincipal,
};
