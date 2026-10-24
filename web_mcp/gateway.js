// HomeGuardian AI — Cookieless OIDC Gateway
//
// The public front door for the HomeGuardian portal. It terminates Google OIDC
// (Authorization Code + PKCE, server-side token exchange), issues an in-memory
// Bearer session, and proxies ONLY a fixed allowlist of read-only tools to the
// native C++ `hg_mcp_server` through `mcp_adapter.js`.
//
// Hard invariants:
//   * No cookies, ever. No Set-Cookie is emitted and inbound Cookie headers are
//     ignored. The browser keeps its session token in a JS closure only.
//   * Every authenticated principal is VIEWER. Private data (camera, frames,
//     feed, uploads, device identities, raw media, consent, mutations) is denied
//     by default. Household status, summaries, device visibility and camera reads
//     require a fresh protected-file subject grant on every request.
//   * The MCP bearer token never leaves this process. Responses are shaped
//     server-side and error details are redacted.
//   * An unreachable/failing backend is reported as `unavailable` (HTTP 503),
//     never as an empty or zeroed success.
//   * The portal page is the SAME page the C++ bridge serves (src/web/index.html)
//     — this gateway does not ship a second dashboard.
//
// Requires only node builtins plus the already-present ./mcp_adapter module.

'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');

const { HomeGuardianMcpClient } = require('./mcp_adapter');
const { householdAuthorized, bridgeRead } = require('./household_read');
const {
  OidcConfig,
  OidcProvider,
  OidcError,
  ROLE_VIEWER,
  publicPrincipal,
} = require('./oauth');

const DEFAULT_PORTAL_PATH = path.join(__dirname, '..', 'src', 'web', 'index.html');
const PORTAL_ASSET_PATH = '/assets/portal-session.js';
const EVENT_VIDEO_ASSET_PATH = '/assets/event-videos.js';
const EVENT_VIDEO_ID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const MAX_BODY_BYTES = 64 * 1024;
const SESSION_TOKEN_RE = /^[0-9a-f]{64}$/;
const BROWSER_PROOF_RE = /^[A-Za-z0-9_-]{43}$/;
const POPUP_COMPLETE_JS = `(function () {
  'use strict';
  var match = /^#session=([A-Za-z0-9_-]{43})$/.exec(location.hash);
  history.replaceState(null, '', location.pathname);
  if (match && window.opener) {
    try { window.opener.postMessage({type:'hg:oauth', code:match[1]}, location.origin); } catch (_) {}
  }
  if (match) {
    document.getElementById('completion-message').textContent = 'Sign-in complete. Return to the original HomeGuardian tab; it will finish signing you in automatically.';
    window.close();
  }
})();`;

// ---------------------------------------------------------------------------
// Session store (hashed tokens, TTL, explicit revocation)
// ---------------------------------------------------------------------------

function hashToken(token) {
  return crypto.createHash('sha256').update(String(token), 'utf8').digest('hex');
}

class SessionStore {
  constructor({ ttlMs = 3600 * 1000, now = Date.now, maxSessions = 2000 } = {}) {
    this.ttlMs = ttlMs;
    this.now = now;
    this.maxSessions = maxSessions;
    this.sessions = new Map(); // sha256(token) -> session
  }

  create(principal) {
    this._sweep();
    if (this.sessions.size >= this.maxSessions) {
      const oldest = this.sessions.keys().next();
      if (!oldest.done) this.sessions.delete(oldest.value);
    }
    const token = crypto.randomBytes(32).toString('hex');
    const now = this.now();
    const session = {
      tokenHash: hashToken(token),
      principal,
      createdAt: now,
      expiresAt: now + this.ttlMs,
    };
    this.sessions.set(session.tokenHash, session);
    return { token, session };
  }

  /** Look up a live session by raw token. Expired sessions are destroyed. */
  get(token) {
    if (typeof token !== 'string' || !SESSION_TOKEN_RE.test(token)) return null;
    const key = hashToken(token);
    const session = this.sessions.get(key);
    if (!session) return null;
    if (this.now() > session.expiresAt) {
      this.sessions.delete(key);
      return null;
    }
    return session;
  }

  revoke(token) {
    if (typeof token !== 'string' || token.length === 0) return false;
    return this.sessions.delete(hashToken(token));
  }

  get size() {
    this._sweep();
    return this.sessions.size;
  }

  _sweep() {
    const now = this.now();
    for (const [key, session] of this.sessions) {
      if (now > session.expiresAt) this.sessions.delete(key);
    }
  }
}

/**
 * One-time hand-off code between the OAuth redirect and the browser's
 * in-memory token exchange. Singles use + TTL, so a leaked URL fragment cannot
 * be replayed after the first exchange.
 */
class OneTimeCodeStore {
  constructor({ ttlMs = 60 * 1000, now = Date.now, maxCodes = 256 } = {}) {
    this.ttlMs = ttlMs;
    this.now = now;
    this.maxCodes = maxCodes;
    this.codes = new Map();
  }

  issue(payload) {
    this._sweep();
    if (this.codes.size >= this.maxCodes) {
      const oldest = this.codes.keys().next();
      if (!oldest.done) this.codes.delete(oldest.value);
    }
    const code = crypto.randomBytes(32).toString('base64url');
    this.codes.set(code, { payload, expiresAt: this.now() + this.ttlMs });
    return code;
  }

  take(code) {
    if (typeof code !== 'string' || code.length === 0) return null;
    const entry = this.codes.get(code);
    if (!entry) return null;
    this.codes.delete(code); // single use, taken before any validation
    if (this.now() > entry.expiresAt) return null;
    return entry.payload;
  }

  get size() {
    this._sweep();
    return this.codes.size;
  }

  _sweep() {
    const now = this.now();
    for (const [key, entry] of this.codes) {
      if (now > entry.expiresAt) this.codes.delete(key);
    }
  }
}

// ---------------------------------------------------------------------------
// MCP backend (read-only, allowlisted tools only)
// ---------------------------------------------------------------------------

function redactDetail(message) {
  return String(message || '')
    .replace(/Bearer\s+[A-Za-z0-9._~+/=-]+/gi, 'Bearer [REDACTED]')
    .slice(0, 200);
}

class McpBackend {
  constructor({ client, factory, timeoutMs = 6000 } = {}) {
    this.client = client || null;
    this.factory = factory || null;
    this.timeoutMs = timeoutMs;
    this.initialized = false;
    this.initializePromise = null;
  }

  _getClient() {
    if (!this.client) {
      if (!this.factory) throw new Error('no MCP client configured');
      this.client = this.factory();
    }
    return this.client;
  }

  async _ensureInitialized(client) {
    if (this.initialized) return;
    if (typeof client.initialize !== 'function') {
      this.initialized = true;
      return;
    }
    if (!this.initializePromise) {
      this.initializePromise = Promise.resolve()
        .then(() => client.initialize())
        .then(() => {
          this.initialized = true;
        })
        .finally(() => {
          this.initializePromise = null;
        });
    }
    await this.initializePromise;
  }

  /** Invoke one allowlisted read-only tool. Resolves to parsed tool data. */
  async callTool(name, args = {}) {
    const client = this._getClient();
    await this._ensureInitialized(client);
    const result = await this._withTimeout(client.callTool(name, args));
    if (!result || typeof result !== 'object') {
      throw new Error('MCP returned no usable payload');
    }
    if (result.rawText !== undefined && result.data !== undefined) {
      if (result.data === null || typeof result.data !== 'object') {
        throw new Error('MCP tool returned an empty payload');
      }
      return result.data;
    }
    if (Array.isArray(result) || Object.keys(result).length === 0) {
      throw new Error('MCP tool returned an empty payload');
    }
    return result;
  }

  _withTimeout(promise) {
    if (!this.timeoutMs || this.timeoutMs <= 0) return Promise.resolve(promise);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('MCP backend timed out')), this.timeoutMs);
      if (typeof timer.unref === 'function') timer.unref();
      Promise.resolve(promise).then(
        (value) => {
          clearTimeout(timer);
          resolve(value);
        },
        (err) => {
          clearTimeout(timer);
          reject(err);
        }
      );
    });
  }
}

// Household read candidates, NOT grants. A separate subject authorizer must
// explicitly permit access before either route touches MCP.
const VIEWER_READ_ROUTES = Object.freeze({
  '/api/status': { tool: 'get_homeguardian_status', shape: 'status' },
  '/api/alerts': { tool: 'get_alert_summary', shape: 'alerts' },
  '/api/devices': { tool: 'get_homeguardian_devices', shape: 'devices' },
  '/api/camera/status': { shape: 'bridge' },
  '/api/camera/frame': { shape: 'bridge' },
});

// Explicitly named private surfaces, denied with a specific reason for auditability.
const PRIVATE_ROUTE_REASONS = Object.freeze({
  '/api/device': 'device_identities',
  '/api/feed': 'media_feed',
  '/api/upload': 'uploads',
  '/api/uploads': 'uploads',
  '/api/delete': 'mutations',
  '/api/camera': 'camera',
  '/api/consent': 'consent',
  '/api/integrations': 'integration_identities',
  '/api/scenarios': 'scenario_control',
  '/api/run-scenario': 'mutations',
  '/api/mcp/scenario': 'mutations',
  '/api/session': 'operator_credentials',
  '/api/admin': 'admin',
});

// ---------------------------------------------------------------------------
// Portal assets: same page as the C++ bridge, plus CSP hashes and a session shim
// ---------------------------------------------------------------------------

const PORTAL_SESSION_JS = `/* HomeGuardian cookieless session bootstrap (served by the OIDC gateway).
   The session token lives in this closure for the lifetime of the tab: no cookies,
   no persistent browser storage. Google/MCP credentials never reach the browser. */
(function () {
  'use strict';
  var state = { token: null, role: null, expiresAt: 0 };
  var nativeFetch = window.fetch ? window.fetch.bind(window) : null;
  if (!nativeFetch) return;

  function absolute(url) {
    try { return new URL(url, location.href); } catch (e) { return null; }
  }
  function isSameOriginApi(url) {
    if (typeof url !== 'string') return false;
    var parsed = absolute(url);
    if (!parsed) return false;
    if (parsed.origin !== location.origin) return false;
    return parsed.pathname.indexOf('/api/') === 0;
  }
  // Installed in <head> before the dashboard exists. Fail closed even while
  // exchange is pending; no flash of the original private dashboard.
  var style = document.createElement('style');
  style.textContent = 'html:not([data-hg-auth="viewer"]) .wrap{display:none!important}' +
    'html[data-hg-auth="viewer"] #authPanel,html:not([data-hg-household="granted"]) #camFrame{display:none!important}' +
    '#hg-session-gate{max-width:1000px;margin:24px auto;padding:24px;background:#161b22;color:#e6edf3;border:1px solid #2a3441;border-radius:14px}' +
    '#hg-session-gate a,#hg-session-gate button{margin:8px;padding:10px;display:inline-block}' +
    '#hg-session-gate [hidden]{display:none!important}';
  document.head.appendChild(style);
  var message = 'Sign in with Google to verify your identity. Private household access requires a separate owner grant.';
  var generation = 0;
  function restrictDashboard() {
    if (!state.token || !document.querySelectorAll) return;
    document.querySelectorAll('.wrap button,.wrap input,.wrap select').forEach(function (node) {
      var simulation = state.simulationsAllowed && ['runBtn', 'scenarioSel'].indexOf(node.id) !== -1;
      var read = ['refreshBtn', 'camReconnect', 'camFullscreen'].indexOf(node.id) !== -1;
      node.disabled = !(read && householdGranted()) && !simulation;
      if (simulation && state.simulationBusy) node.disabled = true;
      node.title = simulation ? 'Interactive synthetic simulation — isolated from the camera'
        : node.disabled ? 'VIEWER — control not permitted' : 'Read-only camera/status view';
    });
    if (document.documentElement.getAttribute('data-hg-household') === 'granted') return;
    ['status','metrics','integrations','labelLegend','chat','chips','result','alerts','alertCount',
      'camBadge','camBanner','camOverlay','camMetrics','uploadMsg','feed','scenDesc'].forEach(function (id) {
      var node = document.getElementById(id);
      var text = 'Household access not granted';
      if (node && node.textContent !== text) node.textContent = text;
    });
  }
  function render() {
    if (!document.body) return;
    var gate = document.getElementById('hg-session-gate');
    if (!gate) {
      gate = document.createElement('section'); gate.id = 'hg-session-gate';
      gate.setAttribute('aria-label', 'HomeGuardian sign-in');
      var title = document.createElement('h1'); title.textContent = 'HomeGuardian AI'; gate.appendChild(title);
      var welcome = document.createElement('p'); welcome.id = 'hg-sign-in-info';
      welcome.textContent = 'Anyone can sign in with their Gmail or Google account. Private cameras and household data remain accessible only to authorized accounts.';
      gate.appendChild(welcome);
      var text = document.createElement('p'); text.id = 'hg-session-message';
      text.setAttribute('role', 'status'); text.setAttribute('aria-live', 'polite'); gate.appendChild(text);
      var signin = document.createElement('a'); signin.id = 'hg-sign-in';
      signin.href = '/login'; signin.textContent = 'Sign in with Google'; gate.appendChild(signin);
      signin.addEventListener('click', function (event) { event.preventDefault(); return startBrowserLogin(); });
      var out = document.createElement('button'); out.id = 'hg-log-out'; out.textContent = 'Log out';
      out.addEventListener('click', function () { return logout(); }); gate.appendChild(out);
      var change = document.createElement('button'); change.id = 'hg-switch-account'; change.textContent = 'Switch account';
      change.addEventListener('click', function () {
        var next = window.open('about:blank', '_blank');
        return logout().then(function () { return startBrowserLogin(next); });
      });
      gate.appendChild(change);
      document.body.appendChild(gate);
    }
    document.getElementById('hg-session-message').textContent = (state.token ? 'VIEWER — ' : '') + message;
    document.getElementById('hg-sign-in').hidden = !!state.token;
    document.getElementById('hg-log-out').hidden = !state.token;
    document.getElementById('hg-switch-account').hidden = !state.token;
    restrictDashboard();
  }
  function block(text) {
    document.documentElement.setAttribute('data-hg-household', 'denied');
    clearFrame();
    message = text; render();
  }
  function clear() {
    generation += 1;
    if (loginTimer) clearTimeout(loginTimer);
    if (loginDeadlineTimer) clearTimeout(loginDeadlineTimer);
    loginTimer = null; loginDeadlineTimer = null; loginBusy = false;
    loginProof = null; loginPopup = null;
    state.token = null; state.role = null; state.expiresAt = 0;
    state.reviewOnly = false; state.simulationsAllowed = false; state.simulationBusy = false;
    document.documentElement.setAttribute('data-hg-auth', 'anonymous');
    block('Sign in again. This tab keeps no cookies or stored session; refreshing requires a new login.');
  }
  document.addEventListener('DOMContentLoaded', render);
  if (typeof MutationObserver !== 'undefined') {
    new MutationObserver(restrictDashboard).observe(document.documentElement, { childList: true, subtree: true });
  }
  render();
  function announce() {
    document.documentElement.setAttribute('data-hg-auth', state.role ? 'viewer' : 'anonymous');
    try {
      window.dispatchEvent(new CustomEvent('hg:session', {
        detail: { authenticated: !!state.token, role: state.role }
      }));
    } catch (e) {}
  }

  window.fetch = function (input, init) {
    var url = typeof input === 'string' ? input : (input && input.url) || '';
    var api = isSameOriginApi(url);
    var eventVideoPath = api ? absolute(url).pathname : '';
    var eventVideo = eventVideoPath === '/api/event-videos' || eventVideoPath.indexOf('/api/event-videos/') === 0;
    // Original inline polls may start before the one-time exchange resolves.
    return Promise.resolve(window.__hg.ready).then(function () {
      var opts = {};
      if (init) {
        for (var key in init) {
          if (Object.prototype.hasOwnProperty.call(init, key)) opts[key] = init[key];
        }
      }
      if (api) {
        var headers = new Headers(opts.headers || (input && input.headers) || {});
        if (state.token) headers.set('Authorization', 'Bearer ' + state.token);
        opts.headers = headers;
      }
      opts.credentials = 'omit';
      var requestGeneration = generation;
      var requestToken = state.token;
      return nativeFetch(input, opts).then(function (res) {
        if (!api) return res;
        if (requestGeneration !== generation) throw new Error('session_changed');
        var household = ['/api/status', '/api/alerts', '/api/devices', '/api/camera/status', '/api/camera/frame'].indexOf(absolute(url).pathname) !== -1;
        if (!res.ok) {
          if (res.status === 401) {
            // Anonymous legacy polls run while the Google popup is open. They
            // must not discard its memory proof or a newer completed exchange.
            if (requestToken && requestToken === state.token) clear();
          }
          else if (res.status === 403) {
            var privateInventory = state.reviewOnly && absolute(url).pathname === '/api/devices';
            if (household && !privateInventory) block('Private household access not granted. Google sign-in verifies identity only.');
            else { render(); }
          } else if (!eventVideo) { message = 'HomeGuardian data unavailable. No household readings or zero counts are being reported.'; render(); }
          // The old page does not check response.ok: never let its JSON fallbacks
          // convert a denial/outage into empty alerts or zero connected devices.
          throw new Error('gateway_access_denied_or_unavailable');
        }
        return res;
      }, function (err) {
        if (api && !eventVideo && requestGeneration === generation) block('HomeGuardian data unavailable. No readings are being reported.');
        throw err;
      });
    });
  };

  function exchange(code, verifier) {
    var exchangeGeneration = generation;
    return nativeFetch('/api/session/exchange', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: code, verifier: verifier }),
      credentials: 'omit',
      cache: 'no-store'
    }).then(function (res) {
      return res.json().then(function (body) {
        if (!res.ok || !body || !body.token) throw new Error((body && body.error) || 'exchange_failed');
        if (exchangeGeneration !== generation) {
          return nativeFetch('/api/logout', { method: 'POST',
            headers: { Authorization: 'Bearer ' + body.token, 'Content-Type': 'application/json' },
            body: '{}', credentials: 'omit', cache: 'no-store' }).then(function () { return null; });
        }
        return acceptSession(body).then(function () { return body; });
      });
    }).catch(function () {
      return null;
    });
  }

  function acceptSession(body) {
    state.token = body.token; state.role = body.role;
    state.expiresAt = Date.now() + (Number(body.expires_in) || 0) * 1000;
    loginProof = null; loginPopup = null;
    if (loginTimer) clearTimeout(loginTimer);
    if (loginDeadlineTimer) clearTimeout(loginDeadlineTimer);
    loginTimer = null; loginDeadlineTimer = null;
    announce();
    block('Private household access not granted. Google sign-in verifies identity only.');
    return checkHousehold();
  }
  function refreshAfterLogin() {
    if (state.token) {
      if (typeof window.refresh === 'function') window.refresh().catch(function () {});
      else window.fetch('/api/status').catch(function () {});
    }
  }

  function householdGranted() {
    return !!state.token && document.documentElement.getAttribute('data-hg-household') === 'granted';
  }
  function checkHousehold() {
    var token = state.token, g = generation;
    // Bypass the wrapper: login completion itself is window.__hg.ready.
    return nativeFetch('/api/me', { headers: { Authorization: 'Bearer ' + token },
      credentials: 'omit', cache: 'no-store' }).then(function (res) {
      if (!res.ok) throw new Error('identity_unavailable');
      return res.json();
    }).then(function (me) {
      if (g !== generation || token !== state.token) return;
      if (me.household_authorized !== true && me.outdoor_review_authorized !== true) return block('Private household access not granted. Google sign-in verifies identity only.');
      state.simulationsAllowed = me.outdoor_review_authorized === true;
      state.reviewOnly = me.household_authorized !== true && me.outdoor_review_authorized === true;
      document.documentElement.setAttribute('data-hg-household', 'granted');
      // Remove old denial placeholders only after the server grants this scope.
      // Real backend renderers replace these with data or availability errors.
      ['status','metrics','alerts','alertCount','camBadge','camBanner','camOverlay','camMetrics','scenDesc'].forEach(function (id) {
        var node = document.getElementById(id);
        if (node && node.textContent === 'Household access not granted') node.textContent = 'Loading authorized view…';
      });
      message = state.reviewOnly
        ? 'Outdoor camera: read-only for Google-signed-in users and judges. Synthetic simulations: interactive and isolated. Device management remains restricted.'
        : 'Owner-approved household read access. Mutations, consent, uploads and administration remain denied.';
      render();
      if (state.simulationsAllowed && typeof window.loadScenarios === 'function') window.loadScenarios();
      if (typeof window.camRefreshStatus === 'function') window.camRefreshStatus();
      if (typeof window.camTickFrame === 'function') window.camTickFrame();
    }).catch(function () { if (g === generation) block('Household authorization unavailable.'); });
  }
  function text(id, value) {
    var node = document.getElementById(id); if (node) node.textContent = value;
  }
  var frameUrl = null, frameBusy = false;
  function clearFrame() {
    var img = document.getElementById('camFrame');
    if (img && img.removeAttribute) img.removeAttribute('src');
    if (frameUrl) URL.revokeObjectURL(frameUrl);
    frameUrl = null;
  }
  // Adapt the legacy dashboard in memory, leaving its HTML source untouched.
  // MCP has aggregates, NOT timeline records or scenario/consent flags. Do not
  // let the old renderers turn absent fields into fabricated disabled/zero data.
  document.addEventListener('DOMContentLoaded', function () {
    // Synthetic writes use the browser's authenticated session and exact Origin,
    // not the live bridge's operator secret. The server runs a separate sandbox.
    window.runScenario = function () {
      if (!state.simulationsAllowed || state.simulationBusy) return Promise.resolve();
      var select = document.getElementById('scenarioSel');
      var id = select && select.value;
      if (!id) return Promise.resolve();
      state.simulationBusy = true; restrictDashboard();
      text('scenDesc', 'Running an isolated synthetic simulation; live camera and alerts are unchanged.');
      return window.fetch('/api/run-scenario?scenario=' + encodeURIComponent(id), {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}'
      }).then(function (res) { return res.json().then(function (body) {
        if (!res.ok || !body.scenario || body.synthetic !== true || body.isolated !== true) throw new Error('simulation_unavailable');
        window.__lastScenario = body.scenario;
        if (typeof window.renderResult === 'function') window.renderResult(body.scenario);
        text('scenDesc', 'Synthetic simulation result — isolated from live camera alerts. ' + (body.scenario.outcome || ''));
      }); }).catch(function () { text('scenDesc', 'Simulation unavailable or busy. No live-camera state was changed.'); })
        .then(function () { state.simulationBusy = false; restrictDashboard(); });
    };
    var runButton = document.getElementById('runBtn');
    if (runButton) runButton.onclick = window.runScenario;
    var originalCameraStatus = window.camRenderStatus;
    if (typeof originalCameraStatus === 'function') window.camRenderStatus = function (d) {
      originalCameraStatus(d);
      text('camBanner', !d.configured ? 'No camera configured on this portal.' :
        d.source_class === 'synthetic_camera_fixture' ? 'Synthetic camera fixture — not a physical observation.' :
        d.source_class === 'live_physical_camera' ? 'Physical camera observation — read-only snapshot view.' :
        'Camera source provenance unavailable or unverified — read-only snapshot view.');
    };
    window.renderStatus = function (s) {
      text('status', s ? 'State: ' + s.state + ' · capture ' + (s.capture_enabled ? 'enabled' : 'disabled') : 'Status unavailable');
      text('metrics', 'Read-only native MCP status; scenario metrics are not provided.');
    };
    window.refreshIntegrations = function () {
      if (state.reviewOnly || !householdGranted()) {
        text('integrations', 'Outdoor-camera review only. Private device inventory is restricted.');
        text('labelLegend', 'Read-only review access; device management is not available.');
        return Promise.resolve();
      }
      return window.fetch('/api/devices').then(function (r) { return r.json(); }).then(function (d) {
        text('integrations', 'Device count: ' + d.device_count + ' · visibility: ' + d.visibility +
          (d.device_labels.length ? ' · ' + d.device_labels.join(', ') : ''));
        text('labelLegend', 'Device visibility supplied by native MCP. Other integration details are not provided.');
      }).catch(function () { text('integrations', 'Device visibility unavailable'); });
    };
    window.refreshAlerts = function () {
      return window.fetch('/api/alerts').then(function (r) { return r.json(); }).then(function (d) {
        text('alertCount', String(d.total));
        text('alerts', 'Aggregate alert summary: ' + d.total + ' · highest severity: ' + (d.highest_severity || 'none') +
          '. Individual alert details are not supplied by this read-only tool.');
      }).catch(function () { text('alertCount', 'Unavailable'); text('alerts', 'Alert summary unavailable'); });
    };
    window.camTickFrame = function () {
      if (!householdGranted() || frameBusy) return Promise.resolve();
      frameBusy = true; var g = generation;
      return window.fetch('/api/camera/frame', { cache: 'no-store' }).then(function (r) { return r.blob(); }).then(function (blob) {
        if (!householdGranted() || g !== generation) return;
        var img = document.getElementById('camFrame'); if (!img) return;
        clearFrame(); frameUrl = URL.createObjectURL(blob); img.src = frameUrl;
      }).catch(function () {
        clearFrame();
        text('camOverlay', 'No fresh frame available. Camera may be unconfigured or unavailable.');
      }).then(function () { frameBusy = false; });
    };
  });

  function logout() {
    var headers = { 'Content-Type': 'application/json' };
    if (state.token) headers.Authorization = 'Bearer ' + state.token;
    if (loginProof) nativeFetch('/api/login/result', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ verifier: loginProof, cancel: true }), credentials: 'omit', cache: 'no-store'
    }).catch(function () {});
    clear();
    announce();
    return nativeFetch('/api/logout', {
      method: 'POST', headers: headers, body: '{}', credentials: 'omit', cache: 'no-store'
    }).catch(function () {});
  }

  window.__hg = {
    role: function () { return state.role; },
    authenticated: function () { return !!state.token; },
    expiresAt: function () { return state.expiresAt; },
    logout: logout,
    ready: null
  };

  var loginProof = null;
  var loginPopup = null;
  var loginTimer = null;
  var loginDeadlineTimer = null;
  var loginBusy = false;
  function schedulePoll(proof, deadline) {
    if (loginProof !== proof) return;
    loginTimer = setTimeout(function () { return pollResult(proof, deadline); }, 1500);
  }
  function pollResult(proof, deadline) {
    if (loginProof !== proof) return Promise.resolve();
    if (Date.now() >= deadline) {
      return logout().then(function () { block('Google sign-in timed out. Please try again.'); });
    }
    if (loginBusy) { schedulePoll(proof, deadline); return Promise.resolve(); }
    loginBusy = true;
    var pollGeneration = generation;
    return nativeFetch('/api/login/result', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ verifier: proof }), credentials: 'omit', cache: 'no-store'
    }).then(function (res) {
      if (res.status === 202 || res.status === 429) return null;
      return res.json().then(function (body) {
        if (res.ok && body && body.token) {
          if (generation !== pollGeneration || loginProof !== proof) {
            return nativeFetch('/api/logout', { method: 'POST',
              headers: { Authorization: 'Bearer ' + body.token, 'Content-Type': 'application/json' },
              body: '{}', credentials: 'omit', cache: 'no-store' });
          }
          return acceptSession(body).then(refreshAfterLogin);
        } else if (res.status === 400 && loginProof === proof) {
          loginProof = null; loginPopup = null;
          if (loginDeadlineTimer) clearTimeout(loginDeadlineTimer);
          loginDeadlineTimer = null;
          block('Google sign-in expired or was cancelled. Please try again.');
        }
      });
    }).catch(function () { /* transient network failures retry until deadline */ })
      .then(function () { loginBusy = false; schedulePoll(proof, deadline); });
  }
  function base64url(bytes) {
    var binary = ''; bytes.forEach(function (b) { binary += String.fromCharCode(b); });
    return btoa(binary).replace(/\\+/g, '-').replace(/\\//g, '_').replace(/=+$/, '');
  }
  function startBrowserLogin(popup) {
    if (loginProof) return Promise.resolve();
    loginPopup = popup || window.open('about:blank', '_blank');
    if (!loginPopup) { block('Sign-in unavailable: allow the sign-in popup and try again.'); return Promise.resolve(); }
    var bytes = new Uint8Array(32); crypto.getRandomValues(bytes);
    loginProof = base64url(bytes);
    var proof = loginProof;
    return crypto.subtle.digest('SHA-256', new TextEncoder().encode(proof)).then(function (digest) {
      return nativeFetch('/api/login/start', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ challenge: base64url(new Uint8Array(digest)) }),
        credentials: 'omit', cache: 'no-store'
      });
    }).then(function (res) {
      if (!res.ok) throw new Error('login_unavailable');
      return res.json();
    }).then(function (body) {
      if (loginProof !== proof || !/^\\/login\\?request=[A-Za-z0-9_-]{43}$/.test(body.login_url)) throw new Error('login_invalid');
      loginPopup.location = body.login_url;
      loginDeadlineTimer = setTimeout(function () {
        if (loginProof !== proof) return;
        logout();
        block('Google sign-in timed out. Please try again.');
      }, 600000);
      schedulePoll(proof, Date.now() + 600000);
    }).catch(function () { loginProof = null; block('Google sign-in unavailable. Please try again.'); });
  }
  window.addEventListener('message', function (event) {
    if (event.origin !== location.origin || event.source !== loginPopup || !loginProof ||
        !event.data || event.data.type !== 'hg:oauth' || !/^[A-Za-z0-9_-]{43}$/.test(event.data.code)) return;
    if (loginBusy) return; // polling owns the completion; it will retry if pending
    loginBusy = true;
    var proof = loginProof;
    window.__hg.ready = exchange(event.data.code, proof);
    // The old initial polls may have already failed while signed out.
    window.__hg.ready.then(function () {
      loginBusy = false; refreshAfterLogin();
    });
  });
  // Never accept a hand-off URL in this tab. Only this tab holds the independent
  // browser-memory verifier; popup messages and server polling both require it.
  if (location.hash) { try { history.replaceState(null, '', location.pathname + location.search); } catch (e) {} }
  window.__hg.ready = Promise.resolve(null);
  clear();
})();
`;

function sha256Base64(body) {
  return crypto.createHash('sha256').update(body, 'utf8').digest('base64');
}

/**
 * Extract CSP source hashes for every inline <script> and <style> block. The
 * page is never modified on disk; only the served copy gets one extra external
 * script tag, so hashes are computed once from the file bytes.
 */
function computeInlineHashes(html) {
  const scriptHashes = [];
  const styleHashes = [];
  const scriptRe = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  const styleRe = /<style[^>]*>([\s\S]*?)<\/style>/gi;
  let match;
  while ((match = scriptRe.exec(html)) !== null) scriptHashes.push(`'sha256-${sha256Base64(match[1])}'`);
  while ((match = styleRe.exec(html)) !== null) styleHashes.push(`'sha256-${sha256Base64(match[1])}'`);
  return { scriptHashes, styleHashes };
}

function buildCsp(inline, r2Origin = '') {
  const scriptSrc = ["'self'", ...inline.scriptHashes].join(' ');
  // Inline style="" attributes cannot be hashed (30 of them in the bridge page),
  // so styles keep 'unsafe-inline'. Scripts never do: hashes only, no 'unsafe-inline'.
  const styleSrc = ["'self'", "'unsafe-inline'"].join(' ');
  return [
    "default-src 'self'",
    `script-src ${scriptSrc}`,
    `style-src ${styleSrc}`,
    "img-src 'self' data: blob:",
    "media-src 'self' blob:" + (r2Origin ? ' ' + r2Origin : ''),
    "connect-src 'self'",
    "font-src 'self'",
    "object-src 'none'",
    "base-uri 'none'",
    "form-action 'self'",
    "frame-ancestors 'none'",
    "worker-src 'self' blob:",
    "manifest-src 'self'",
  ].join('; ');
}

function securityHeaders(res, csp) {
  res.setHeader('Content-Security-Policy', csp);
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('Referrer-Policy', 'no-referrer');
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin-allow-popups');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
  res.setHeader('Strict-Transport-Security', 'max-age=31536000; includeSubDomains');
}

function loadPortal(portalPath, r2Origin = '') {
  const html = fs.readFileSync(portalPath, 'utf8');
  const inline = computeInlineHashes(html);
  const shimTag = `<script src="${PORTAL_ASSET_PATH}"></script>\n<script defer src="${EVENT_VIDEO_ASSET_PATH}"></script>`;
  // Parser blocking: install fetch and fail-closed visibility before bridge code.
  const served = html.replace(/<head\b[^>]*>/i, (head) => head + '\n' + shimTag);
  return { html: served, inline, csp: buildCsp(inline, r2Origin), sourcePath: portalPath };
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------

function sendJson(res, status, body, extraHeaders = {}) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store, no-cache, must-revalidate',
    Pragma: 'no-cache',
    ...extraHeaders,
  });
  res.end(payload);
}

function sendText(res, status, text, contentType = 'text/plain; charset=utf-8', extraHeaders = {}) {
  res.writeHead(status, {
    'Content-Type': contentType,
    'Content-Length': Buffer.byteLength(text),
    'Cache-Control': 'no-store',
    ...extraHeaders,
  });
  res.end(text);
}

function redirect(res, location, status = 302) {
  res.writeHead(status, {
    Location: location,
    'Cache-Control': 'no-store, no-cache, must-revalidate',
    Pragma: 'no-cache',
    'Content-Length': '0',
  });
  res.end();
}

function readJsonBody(req, strict = false) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) {
        reject(new OidcError('payload_too_large', 'request body exceeds limit', 413));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      const raw = Buffer.concat(chunks).toString('utf8');
      if (!raw.trim()) {
        resolve({});
        return;
      }
      try {
        const parsed = JSON.parse(raw);
        if (strict && (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))) {
          reject(new OidcError('json_object_required', 'request must be a JSON object', 400));
          return;
        }
        resolve(parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {});
      } catch (_) {
        reject(new OidcError('malformed_json', 'request body is not valid JSON', 400));
      }
    });
    req.on('error', reject);
  });
}

function bearerToken(req) {
  const header = req.headers.authorization;
  if (typeof header !== 'string') return null;
  const match = /^Bearer ([0-9a-f]{64})$/.exec(header.trim());
  return match ? match[1] : null;
}

function originAllowed(req, config, hostHeader) {
  const origin = req.headers.origin;
  if (!origin) return true;
  const allowed = new Set();
  if (config.publicOrigin) allowed.add(config.publicOrigin);
  if (hostHeader) {
    allowed.add(`https://${hostHeader}`);
    allowed.add(`http://${hostHeader}`);
  }
  return allowed.has(String(origin).replace(/\/+$/, ''));
}

// ---------------------------------------------------------------------------
// Gateway
// ---------------------------------------------------------------------------

function createGateway(options = {}) {
  const env = options.env || process.env;
  const config = options.config || new OidcConfig(env);
  const now = options.now || Date.now;

  const provider =
    options.provider ||
    new OidcProvider({
      config,
      jwks: options.jwks,
      fetchImpl: options.fetchImpl,
      now,
      randomBytes: options.randomBytes,
    });

  const sessions =
    options.sessions ||
    new SessionStore({ ttlMs: config.sessionTtlSeconds * 1000, now });
  const codes =
    options.codes ||
    new OneTimeCodeStore({ ttlMs: config.exchangeCodeTtlSeconds * 1000, now });
  const loginStarts = new OneTimeCodeStore({ ttlMs: 60000, now });
  const browserBindings = new Map(); // state -> public challenge, bounded + TTL
  // Only SHA256(memory verifier) indexes this bounded, one-use rendezvous.
  // Public challenges select a pending login but cannot prove possession.
  const loginResults = new Map();
  function sweepResults() {
    for (const [key, entry] of loginResults) {
      if (entry.expiresAt <= now()) {
        loginResults.delete(key);
        if (entry.token) sessions.revoke(entry.token);
      }
    }
  }
  function deliverResult(res, entry) {
    if (loginResults.get(entry.challenge) !== entry || entry.expiresAt <= now()) {
      sendJson(res, 400, { ok: false, error: 'invalid_or_used_code' }); return;
    }
    loginResults.delete(entry.challenge); // atomic across both completion paths
    const session = sessions.get(entry.token);
    if (!session) { sendJson(res, 401, { ok: false, error: 'session_expired' }); return; }
    sendJson(res, 200, {
      ok: true, token: entry.token, token_type: 'Bearer',
      expires_in: Math.max(0, Math.floor((session.expiresAt - now()) / 1000)),
      role: session.principal.role, scope: 'read_only', cookie_set: false,
    });
  }

  const mcp =
    options.mcp ||
    new McpBackend({
      client: options.mcpClient,
      factory:
        options.mcpFactory ||
        (() =>
          new HomeGuardianMcpClient({
            baseUrl: env.HG_MCP_URL || 'http://127.0.0.1:8790/mcp',
            token: env.HG_MCP_TOKEN || '',
          })),
      timeoutMs: options.mcpTimeoutMs || Number(env.HG_MCP_TIMEOUT_MS || 6000),
    });

  const portalPath = options.portalPath || env.HG_PORTAL_PATH || DEFAULT_PORTAL_PATH;
  let portal = null;
  let portalError = null;
  try {
    const r2Origin = /^[0-9a-f]{32}$/.test(env.HG_R2_ACCOUNT_ID || '')
      ? `https://${env.HG_R2_ACCOUNT_ID}.r2.cloudflarestorage.com` : '';
    portal = loadPortal(portalPath, r2Origin);
  } catch (err) {
    portalError = err.message; // reported honestly at request time
  }

  const log = options.logger || {
    info: (msg) => console.log(`[gateway] ${msg}`),
    warn: (msg) => console.warn(`[gateway] ${msg}`),
  };

  function unavailable(res, extra = {}) {
    sendJson(res, 503, {
      ok: false,
      status: 'unavailable',
      availability: 'unavailable',
      error: 'backend_unavailable',
      backend: 'mcp',
      role: ROLE_VIEWER,
      scope: 'read_only',
      message: 'The HomeGuardian backend is unavailable; no data is being reported.',
      ...extra,
    });
  }

  // Explicit owner opt-in for judges and other Google-authenticated reviewers.
  // Never grants device inventory, mutations, microphone media or arbitrary routes.
  function outdoorReviewAuthorized(session, pathname) {
    return env.HG_OUTDOOR_REVIEW_ACCESS === 'google_viewers' &&
      session.principal.role === ROLE_VIEWER &&
      ['/api/status', '/api/alerts', '/api/camera/status', '/api/camera/frame'].includes(pathname);
  }

  async function authorized(session, pathname) {
    if (outdoorReviewAuthorized(session, pathname)) return true;
    try {
      return typeof options.authorizeHousehold === 'function'
        ? (await options.authorizeHousehold(session.principal.sub)) === true
        : householdAuthorized(env.HG_HOUSEHOLD_SUBJECTS_FILE, session.principal.sub) === true;
    } catch (_) { return false; }
  }

  let eventVideoService = options.eventVideos || null;
  function videos() {
    if (!eventVideoService) {
      const { EventVideos } = require('./event_videos');
      eventVideoService = new EventVideos({ env, publicOrigin: config.publicOrigin, now });
    }
    return eventVideoService;
  }
  function singleAuthorization(req) {
    return req.rawHeaders.filter((_, i) => i % 2 === 0 && req.rawHeaders[i].toLowerCase() === 'authorization').length === 1;
  }
  function notificationAuthorized(req) {
    if (!singleAuthorization(req)) return false;
    const token = bearerToken(req);
    if (!token) return false;
    let fd;
    try {
      fd = fs.openSync(env.HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
      const st = fs.fstatSync(fd);
      if (!st.isFile() || st.nlink !== 1 || st.size > 128 || (st.mode & 0o7777) !== 0o600 || st.uid !== process.geteuid()) return false;
      const key = fs.readFileSync(fd, 'utf8').trim();
      return SESSION_TOKEN_RE.test(key) && crypto.timingSafeEqual(Buffer.from(token), Buffer.from(key));
    } catch (_) { return false; }
    finally { if (fd !== undefined) fs.closeSync(fd); }
  }
  function videoError(res, err) {
    const status = [400, 404, 409, 413, 429, 507].includes(err.status) ? err.status : 503;
    const errors = { 400: 'invalid_event_video_request', 404: 'event_video_not_found',
      409: 'event_video_conflict', 413: 'event_video_too_large', 429: 'event_video_limit_reached',
      503: 'event_videos_unavailable', 507: 'event_video_storage_full' };
    const error = status === 503 && err.code === 'event_video_storage_unconfigured'
      ? err.code : errors[status];
    sendJson(res, status, { ok: false, error, ...(status === 503 ? { status: 'unavailable' } : {}) });
  }

  let simulationReview = options.simulationReview || null;
  function simulations() {
    if (!simulationReview) {
      const { SimulationReview } = require('./simulation_review');
      simulationReview = new SimulationReview({ binaryPath: env.HG_REVIEW_BRIDGE_BIN });
    }
    return simulationReview;
  }

  async function handleProxy(res, route, session, pathname) {
    if (!(await authorized(session, pathname))) {
      sendJson(res, 403, {
        ok: false, error: 'household_authorization_required', role: ROLE_VIEWER,
        message: 'Private household access has not been granted. Google sign-in verifies identity only.',
      });
      return;
    }
    if (route.shape === 'bridge') {
      try {
        const data = await bridgeRead(env, pathname);
        if (Buffer.isBuffer(data)) sendText(res, 200, data, 'image/jpeg');
        else sendJson(res, 200, data);
      } catch (_) { unavailable(res, { backend: 'private_bridge' }); }
      return;
    }
    let data;
    try {
      data = await mcp.callTool(route.tool, {});
    } catch (err) {
      log.warn(`MCP read '${route.tool}' unavailable`);
      unavailable(res, { tool: route.tool });
      return;
    }

    if (route.shape === 'devices') {
      if (!Number.isSafeInteger(data.device_count) || data.device_count < 0 ||
          !Array.isArray(data.device_labels) || !data.device_labels.every(v => typeof v === 'string') ||
          typeof data.visibility !== 'string') {
        unavailable(res, { tool: route.tool }); return;
      }
      sendJson(res, 200, { ok: true, backend: 'mcp', read_only: true,
        device_count: data.device_count, device_labels: data.device_labels, visibility: data.visibility });
      return;
    }
    if (route.shape === 'alerts') {
      const count = data.alert_count;
      if (!Number.isSafeInteger(count) || count < 0) {
        unavailable(res, { tool: route.tool, detail: 'alert_count missing from MCP payload' });
        return;
      }
      sendJson(res, 200, {
        ok: true,
        role: session.principal.role,
        scope: 'read_only',
        backend: 'mcp',
        source: 'mcp',
        total: count,
        alert_count: count,
        highest_severity: data.highest_severity ?? null,

        alerts: [],
        read_only: true,
        note: 'viewer_scope: aggregate alert summary only; alert payloads and media are not exposed',
      });
      return;
    }

    // Native MCP supplies state and capture_enabled, not scenario metrics.
    if (typeof data.state !== 'string' || typeof data.capture_enabled !== 'boolean') {
      unavailable(res, { tool: route.tool }); return;
    }
    data = { state: data.state, capture_enabled: data.capture_enabled, read_only: true };
    sendJson(res, 200, {
      ok: true,
      role: session.principal.role,
      scope: 'read_only',
      backend: 'mcp',
      source: 'mcp',
      status: { ok: true, ...data },
      data: { ok: true, ...data },
      read_only: true,
    });
  }

  const handler = async (req, res) => {
    const csp = portal ? portal.csp : "default-src 'none'; frame-ancestors 'none'";
    securityHeaders(res, csp);

    let url;
    try {
      url = new URL(req.url, 'http://localhost');
    } catch (_) {
      sendJson(res, 400, { ok: false, error: 'bad_request' });
      return;
    }
    const pathname = url.pathname;

    // No credential may ever ride in a query string.
    for (const banned of ['token', 'session', 'k', 'access_token', 'id_token', 'code_verifier']) {
      if (url.searchParams.has(banned)) {
        sendJson(res, 400, {
          ok: false,
          error: 'credentials_in_url_forbidden',
          message: 'Tokens and session material in URL query strings are forbidden',
        });
        return;
      }
    }

    if (req.method === 'OPTIONS') {
      sendJson(res, 405, { ok: false, error: 'method_not_allowed' });
      return;
    }

    // ---- static: the bridge's portal page ---------------------------------
    if ((pathname === '/' || pathname === '/index.html') && req.method === 'GET') {
      if (!portal) {
        sendJson(res, 503, {
          ok: false,
          status: 'unavailable',
          error: 'portal_unavailable',
          detail: redactDetail(portalError),
        });
        return;
      }
      res.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Content-Length': Buffer.byteLength(portal.html),
        'Cache-Control': 'no-store',
      });
      res.end(req.method === 'HEAD' ? undefined : portal.html);
      return;
    }

    if (pathname === PORTAL_ASSET_PATH && (req.method === 'GET' || req.method === 'HEAD')) {
      res.writeHead(200, {
        'Content-Type': 'application/javascript; charset=utf-8',
        'Content-Length': Buffer.byteLength(PORTAL_SESSION_JS),
        'Cache-Control': 'no-store',
      });
      res.end(req.method === 'HEAD' ? undefined : PORTAL_SESSION_JS);
      return;
    }

    if (pathname === '/favicon.ico') {
      res.writeHead(204);
      res.end();
      return;
    }

    if (pathname === EVENT_VIDEO_ASSET_PATH && (req.method === 'GET' || req.method === 'HEAD')) {
      try {
        const script = fs.readFileSync(path.join(__dirname, 'event_videos_ui.js'));
        res.writeHead(200, { 'Content-Type': 'application/javascript; charset=utf-8',
          'Content-Length': script.length, 'Cache-Control': 'no-store' });
        res.end(req.method === 'HEAD' ? undefined : script);
      } catch (_) { sendJson(res, 503, { ok: false, error: 'event_video_ui_unavailable' }); }
      return;
    }

    // Machine-only metadata/signing handoff. Browser VIEWER sessions never grant
    // uploads; video bytes go directly from the recorder to private R2 storage.
    if (pathname.startsWith('/internal/event-videos/')) {
      if (req.headers.origin) {
        sendJson(res, 403, { ok: false, error: 'browser_ingest_forbidden' }); return;
      }
      try {
        const notification = pathname === '/internal/event-videos/notification';
        if (!(notification ? notificationAuthorized(req) : singleAuthorization(req) && videos().authenticateProducer(req))) {
          sendJson(res, 401, { ok: false, error: 'producer_authentication_required' }); return;
        }
        if ([...url.searchParams].length) {
          sendJson(res, 400, { ok: false, error: 'invalid_event_video_query' }); return;
        }
        if (req.method !== (notification ? 'GET' : 'POST')) {
          sendJson(res, 405, { ok: false, error: 'method_not_allowed' }); return;
        }
        if (notification) { sendJson(res, 200, await videos().latestNotification()); return; }
        const prepare = pathname === '/internal/event-videos/uploads';
        const complete = /^\/internal\/event-videos\/uploads\/([^/]+)\/complete$/.exec(pathname);
        if (!prepare && (!complete || !EVENT_VIDEO_ID_RE.test(complete[1]))) {
          sendJson(res, 400, { ok: false, error: 'invalid_event_video_route' }); return;
        }
        if (!/^application\/json(?:\s*;\s*charset=utf-8)?$/i.test(req.headers['content-type'] || '')) {
          sendJson(res, 400, { ok: false, error: 'json_object_required' }); return;
        }
        const body = await readJsonBody(req, true);
        if (complete && Object.keys(body).length) {
          sendJson(res, 400, { ok: false, error: 'completion_body_must_be_empty' }); return;
        }
        sendJson(res, prepare ? 201 : 200, prepare ? await videos().prepare(body) : await videos().complete(complete[1]));
      } catch (err) { videoError(res, err); }
      return;
    }

    // ---- health (non-sensitive) ------------------------------------------
    if ((pathname === '/health' || pathname === '/api/health') && req.method === 'GET') {
      sendJson(res, 200, {
        status: 'ok',
        service: 'homeguardian-oidc-gateway',
        auth: 'oidc_pkce_s256',
        cookies: false,
        oidc_configured: config.isConfigured(),
      });
      return;
    }

    if (pathname === '/auth/complete' && req.method === 'GET') {
      sendText(res, 200, '<!doctype html><title>HomeGuardian sign-in</title><p id="completion-message">Return to the original HomeGuardian tab to finish sign-in. This page cannot sign in a standalone tab.</p><script src="/assets/oauth-complete.js"></script>', 'text/html; charset=utf-8');
      return;
    }
    if (pathname === '/assets/oauth-complete.js' && req.method === 'GET') {
      sendText(res, 200, POPUP_COMPLETE_JS, 'application/javascript; charset=utf-8');
      return;
    }
    if (pathname === '/api/login/start' && req.method === 'POST') {
      if (!config.publicOrigin || req.headers.origin !== config.publicOrigin) {
        sendJson(res, 403, { ok: false, error: 'forbidden_origin' }); return;
      }
      const body = await readJsonBody(req);
      if (typeof body.challenge !== 'string' || !BROWSER_PROOF_RE.test(body.challenge)) {
        sendJson(res, 400, { ok: false, error: 'browser_challenge_required' }); return;
      }
      sweepResults();
      if (loginResults.has(body.challenge)) {
        sendJson(res, 409, { ok: false, error: 'login_already_pending' }); return;
      }
      if (loginResults.size >= 256) {
        sendJson(res, 429, { ok: false, error: 'login_capacity' }); return;
      }
      const result = { challenge: body.challenge, expiresAt: now() + 600000, nextPollAt: 0 };
      loginResults.set(body.challenge, result);
      const ticket = loginStarts.issue({ challenge: body.challenge, result });
      sendJson(res, 200, { ok: true, login_url: '/login?request=' + ticket });
      return;
    }

    if (pathname === '/api/login/result' && req.method === 'POST') {
      if (!config.publicOrigin || req.headers.origin !== config.publicOrigin) {
        sendJson(res, 403, { ok: false, error: 'forbidden_origin' }); return;
      }
      const body = await readJsonBody(req);
      const challenge = typeof body.verifier === 'string' && BROWSER_PROOF_RE.test(body.verifier)
        ? crypto.createHash('sha256').update(body.verifier).digest('base64url') : '';
      sweepResults();
      const entry = loginResults.get(challenge);
      if (!entry) { sendJson(res, 400, { ok: false, error: 'invalid_login_proof' }); return; }
      if (body.cancel === true) {
        loginResults.delete(challenge);
        if (entry.token) sessions.revoke(entry.token);
        sendJson(res, 200, { ok: true, cancelled: true }); return;
      }
      if (entry.nextPollAt > now()) {
        sendJson(res, 429, { ok: false, error: 'poll_too_fast' }, { 'Retry-After': '1' }); return;
      }
      entry.nextPollAt = now() + 1000;
      if (!entry.token) { sendJson(res, 202, { ok: true, status: 'pending' }); return; }
      deliverResult(res, entry); return;
    }

    // ---- login -----------------------------------------------------------
    if (pathname === '/login' && req.method === 'GET') {
      if (!config.isConfigured()) {
        const missing = config.missingDependencies();
        log.warn(`login requested but OIDC is unconfigured (missing: ${missing.join(', ')})`);
        sendJson(res, 503, {
          ok: false,
          status: 'unavailable',
          availability: 'unavailable',
          error: 'google_client_id_missing',
          dependency: missing,
          message:
            'Google sign-in is not configured on this deployment. The owner must set ' +
            'HG_GOOGLE_CLIENT_ID and HG_GOOGLE_REDIRECT_URI (or HG_PUBLIC_ORIGIN).',
          role: ROLE_VIEWER,
        });
        return;
      }
      const browserStart = loginStarts.take(url.searchParams.get('request'));
      if (!browserStart || loginResults.get(browserStart.challenge) !== browserStart.result || browserStart.result.expiresAt <= now()) {
        sendJson(res, 400, { ok: false, error: 'browser_login_required' }); return;
      }
      try {
        const request = provider.createLoginRequest();
        for (const [key, value] of browserBindings) if (value.expiresAt < now()) browserBindings.delete(key);
        if (browserBindings.size >= 256) browserBindings.delete(browserBindings.keys().next().value);
        const state = new URL(request.url).searchParams.get('state');
        browserBindings.set(state, { challenge: browserStart.challenge, result: browserStart.result, expiresAt: now() + 600000 });
        redirect(res, request.url);
      } catch (err) {
        const status = err instanceof OidcError ? err.status : 500;
        log.warn(`login start failed: ${err.code || redactDetail(err.message)}`);
        sendJson(res, status, { ok: false, error: err.code || 'login_start_failed' });
      }
      return;
    }

    // ---- OAuth callback --------------------------------------------------
    if (pathname === '/auth/callback' && req.method === 'GET') {
      if (!config.isConfigured()) {
        sendJson(res, 503, {
          ok: false,
          status: 'unavailable',
          error: 'google_client_id_missing',
          dependency: config.missingDependencies(),
        });
        return;
      }

      const oauthError = url.searchParams.get('error');
      if (oauthError) {
        log.warn(`callback returned provider error: ${String(oauthError).slice(0, 64)}`);
        sendJson(res, 400, {
          ok: false,
          error: 'authorization_denied',
          message: 'Google sign-in was not completed.',
        });
        return;
      }

      const code = url.searchParams.get('code');
      const state = url.searchParams.get('state');
      if (!code || !state) {
        sendJson(res, 400, { ok: false, error: 'missing_code_or_state' });
        return;
      }

      let completed;
      const binding = browserBindings.get(state);
      browserBindings.delete(state);
      sweepResults();
      const result = binding && loginResults.get(binding.challenge);
      if (!binding || binding.expiresAt < now() || !result || binding.result !== result) {
        sendJson(res, 400, { ok: false, error: 'invalid_state' }); return;
      }
      try {
        completed = await provider.completeLogin({ code, state });
      } catch (err) {
        const status = err instanceof OidcError ? err.status : 400;
        const errorCode = err instanceof OidcError ? err.code : 'login_failed';
        log.warn(`callback rejected: ${errorCode}`);
        sendJson(res, status, {
          ok: false,
          error: errorCode,
          message: 'Sign-in could not be verified.',
        });
        return;
      }

      // Logout/cancellation/expiry may have happened during Google's exchange.
      if (loginResults.get(binding.challenge) !== result || result.expiresAt <= now()) {
        sendJson(res, 400, { ok: false, error: 'invalid_state' }); return;
      }
      const { token } = sessions.create(completed.principal);
      result.token = token;
      result.expiresAt = now() + config.exchangeCodeTtlSeconds * 1000;
      const handoff = codes.issue(result);
      log.info('viewer session established');
      // Fragment only: never sent to a server, never logged, single use.
      redirect(res, `/auth/complete#session=${handoff}`);
      return;
    }

    // ---- session exchange ------------------------------------------------
    if (pathname === '/api/session/exchange' && req.method === 'POST') {
      let body;
      try {
        body = await readJsonBody(req);
      } catch (err) {
        sendJson(res, err.status || 400, { ok: false, error: err.code || 'malformed_json' });
        return;
      }
      if (typeof body.code !== 'string' || body.code.length === 0) {
        sendJson(res, 400, { ok: false, error: 'missing_code' });
        return;
      }
      const payload = codes.take(body.code);
      if (!payload) {
        sendJson(res, 400, { ok: false, error: 'invalid_or_used_code' });
        return;
      }
      const proof = typeof body.verifier === 'string' && BROWSER_PROOF_RE.test(body.verifier)
        ? crypto.createHash('sha256').update(body.verifier).digest('base64url') : '';
      if (!proof || proof !== payload.challenge) {
        sendJson(res, 400, { ok: false, error: 'browser_proof_invalid' }); return;
      }
      deliverResult(res, payload);
      return;
    }

    // ---- everything below requires a live Bearer session ------------------
    const token = bearerToken(req);
    const session = token ? sessions.get(token) : null;
    const needsAuth =
      pathname === '/api/me' ||
      pathname === '/api/logout' ||
      pathname.startsWith('/api/') ||
      pathname === '/logout';

    if (needsAuth && !session) {
      sendJson(
        res,
        401,
        {
          ok: false,
          error: 'unauthenticated',
          message: 'A Bearer session from Google sign-in is required.',
        },
        { 'WWW-Authenticate': 'Bearer realm="homeguardian", error="invalid_token"' }
      );
      return;
    }

    if (pathname === '/api/me' && req.method === 'GET') {
      sendJson(res, 200, {
        ok: true,
        ...publicPrincipal(session.principal, session, now()),
        household_authorized: await authorized(session),
        outdoor_review_authorized: outdoorReviewAuthorized(session, '/api/status'),
        config: {
          oidc_configured: config.isConfigured(),
          session_ttl_seconds: config.sessionTtlSeconds,
          cookie_based: false,
        },
      });
      return;
    }

    if ((pathname === '/api/logout' || pathname === '/logout') && req.method === 'POST') {
      sessions.revoke(token);
      sendJson(res, 200, { ok: true, logged_out: true, role: session.principal.role });
      return;
    }

    // Owner-approved outdoor event recordings: authenticated reads only, using
    // the same explicit reviewer grant as the camera, not household membership.
    if (pathname === '/api/event-videos' || pathname.startsWith('/api/event-videos/')) {
      if (req.method !== 'GET') {
        sendJson(res, 405, { ok: false, error: 'method_not_allowed_read_only' }); return;
      }
      if (req.headers.origin && req.headers.origin !== config.publicOrigin) {
        sendJson(res, 403, { ok: false, error: 'forbidden_origin' }); return;
      }
      if (!(await authorized(session, '/api/camera/status'))) {
        sendJson(res, 403, { ok: false, error: 'household_authorization_required' }); return;
      }
      try {
        if (pathname === '/api/event-videos') {
          sendJson(res, 200, await videos().list(url.searchParams)); return;
        }
        const match = /^\/api\/event-videos\/([^/]+)(\/playback)?$/.exec(pathname);
        if (!match || !EVENT_VIDEO_ID_RE.test(match[1]) || [...url.searchParams].length) {
          sendJson(res, 400, { ok: false, error: 'invalid_event_video_route' }); return;
        }
        sendJson(res, 200, match[2] ? await videos().playback(match[1]) : await videos().detail(match[1]));
      } catch (err) { videoError(res, err); }
      return;
    }

    // Owner-approved synthetic sandbox writes, never live-camera mutations.
    if ((pathname === '/api/scenarios' || pathname === '/api/run-scenario') &&
        env.HG_OUTDOOR_REVIEW_ACCESS === 'google_viewers') {
      const run = pathname === '/api/run-scenario';
      if (req.method !== (run ? 'POST' : 'GET')) {
        sendJson(res, 405, { ok: false, error: 'method_not_allowed' }); return;
      }
      if (!originAllowed(req, config, req.headers.host)) {
        sendJson(res, 403, { ok: false, error: 'forbidden_origin' }); return;
      }
      const keys = [...url.searchParams.keys()];
      const id = url.searchParams.get('scenario');
      if ((!run && keys.length) || (run && (keys.length !== 1 || keys[0] !== 'scenario' ||
          typeof id !== 'string' || !/^[a-z][a-z0-9-]{0,63}$/.test(id)))) {
        sendJson(res, 400, { ok: false, error: 'invalid_scenario' }); return;
      }
      if (run) {
        const body = await readJsonBody(req);
        if (!body || Array.isArray(body) || Object.keys(body).length !== 0) {
          sendJson(res, 400, { ok: false, error: 'simulation_body_must_be_empty' }); return;
        }
      }
      try {
        const data = run ? await simulations().run(id) : await simulations().catalogue();
        sendJson(res, 200, { ...data, synthetic: true, isolated: true,
          scope: 'synthetic_simulation', live_camera_modified: false });
      } catch (err) {
        const status = [400, 404, 429].includes(err.status) ? err.status : 503;
        sendJson(res, status, { ok: false, error: status === 503 ? 'simulation_unavailable' : 'simulation_rejected',
          synthetic: true, isolated: true });
      }
      return;
    }

    // ---- mutations are denied wholesale for VIEWER -----------------------
    // Checked before the private-route map so a mutation is always reported as
    // a mutation; allowlisted read paths fall through to their 405 below.
    if (
      ['POST', 'PUT', 'PATCH', 'DELETE'].includes(req.method) &&
      !Object.prototype.hasOwnProperty.call(VIEWER_READ_ROUTES, pathname)
    ) {
      sendJson(res, 403, {
        ok: false,
        error: 'forbidden_mutation',
        role: session.principal.role,
        message: 'VIEWER is a read-only role; state changes are not permitted.',
      });
      return;
    }

    // ---- explicit private surfaces: default-deny before any backend work ---
    if (Object.prototype.hasOwnProperty.call(PRIVATE_ROUTE_REASONS, pathname)) {
      sendJson(res, 403, {
        ok: false,
        error: 'forbidden_private_data',
        reason: PRIVATE_ROUTE_REASONS[pathname],
        role: session.principal.role,
        message: 'The VIEWER role cannot access private HomeGuardian data.',
      });
      return;
    }

    // ---- allowlisted read-only MCP proxy ---------------------------------
    if (Object.prototype.hasOwnProperty.call(VIEWER_READ_ROUTES, pathname)) {
      if (req.method !== 'GET' && (req.method !== 'HEAD' || VIEWER_READ_ROUTES[pathname].shape === 'bridge')) {
        sendJson(res, 405, {
          ok: false,
          error: 'method_not_allowed_read_only',
          role: session.principal.role,
        });
        return;
      }
      if (!originAllowed(req, config, req.headers.host)) {
        sendJson(res, 403, { ok: false, error: 'forbidden_origin' });
        return;
      }
      if (VIEWER_READ_ROUTES[pathname].shape === 'bridge' &&
          [...url.searchParams.keys()].some(k => k !== 't' || pathname !== '/api/camera/frame')) {
        sendJson(res, 400, { ok: false, error: 'invalid_camera_query' }); return;
      }
      await handleProxy(res, VIEWER_READ_ROUTES[pathname], session, pathname);
      return;
    }

    // ---- default deny ----------------------------------------------------
    if (pathname.startsWith('/api/')) {
      sendJson(res, 403, {
        ok: false,
        error: 'not_allowlisted',
        role: session.principal.role,
        message: 'This route is not on the VIEWER read-only allowlist.',
      });
      return;
    }

    sendJson(res, 404, { ok: false, error: 'not_found' });
  };

  const server = http.createServer((req, res) => {
    handler(req, res).catch((err) => {
      log.warn(`unhandled request error: ${redactDetail(err.message)}`);
      if (!res.headersSent) {
        sendJson(res, 500, { ok: false, error: 'internal_error' });
      } else {
        try {
          res.end();
        } catch (_) {}
      }
    });
  });

  return {
    server,
    config,
    provider,
    sessions,
    codes,
    mcp,
    portal,
    csp: portal ? portal.csp : null,
    listen(port = 0, host = '127.0.0.1') {
      return new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(port, host, () => {
          server.removeListener('error', reject);
          resolve(server.address());
        });
      });
    },
    close() {
      return new Promise((resolve) => server.close(() => resolve()));
    },
  };
}

// ---------------------------------------------------------------------------
// Entrypoint
// ---------------------------------------------------------------------------

function startGateway(options = {}) {
  const env = options.env || process.env;
  const host = env.HG_GATEWAY_BIND || '127.0.0.1';
  const port = Number.parseInt(env.HG_GATEWAY_PORT || '8810', 10);

  const gateway = createGateway({ ...options, env });
  const { config } = gateway;

  if (!/^127\.|^::1$|^localhost$/.test(host)) {
    console.warn(
      `[gateway] WARNING: binding ${host} — the documented deployment is loopback ` +
        'behind a Cloudflare tunnel. TLS must terminate in front of this process.'
    );
  }
  if (!config.isConfigured()) {
    console.warn(
      `[gateway] Google OIDC is not configured; /login will report: ${config.missingDependencies().join(', ')}`
    );
  }
  if (config.publicOrigin && !/^https:\/\//.test(config.publicOrigin) && !/^http:\/\/(127\.0\.0\.1|localhost)(:|$)/.test(config.publicOrigin)) {
    console.warn(
      '[gateway] WARNING: HG_PUBLIC_ORIGIN is not https. In the documented deployment TLS ' +
        'terminates at the Cloudflare tunnel; a plaintext public origin would leak the OAuth code.'
    );
  }
  if (!env.HG_GOOGLE_CLIENT_SECRET) {
    console.warn(
      '[gateway] HG_GOOGLE_CLIENT_SECRET is unset; PKCE is still enforced, but a Google ' +
        '"Web application" client normally requires the secret at the token endpoint.'
    );
  }
  if (!env.HG_MCP_TOKEN) {
    console.warn('[gateway] HG_MCP_TOKEN is unset; the MCP backend will reject reads (reported as unavailable).');
  }

  return gateway.listen(port, host).then((address) => {
    console.log(
      `[gateway] listening on http://${address.address}:${address.port} ` +
        `(cookieless OIDC, role VIEWER, MCP read-only allowlist)`
    );
    return { ...gateway, address };
  });
}

module.exports = {
  createGateway,
  startGateway,
  SessionStore,
  OneTimeCodeStore,
  McpBackend,
  buildCsp,
  computeInlineHashes,
  loadPortal,
  PORTAL_SESSION_JS,
  VIEWER_READ_ROUTES,
  PRIVATE_ROUTE_REASONS,
};

if (require.main === module) {
  startGateway().catch((err) => {
    console.error(`[gateway] failed to start: ${err.message}`);
    process.exitCode = 1;
  });
}
