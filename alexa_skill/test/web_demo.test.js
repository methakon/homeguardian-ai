'use strict';

/**
 * Web demo integration tests.
 *
 * Starts the REAL hg_demo_server.js (which serves the web UI and proxies to the
 * REAL compiled hg_alexa_bridge) and exercises the full demo workflow over
 * HTTP: scenario run, status, alert summary, and the fail-closed capture-off
 * path. No mock of the C++ boundary.
 *
 * Prerequisites: build the bridge first:
 *   cmake --build hg_build --target hg_alexa_bridge
 *
 * Run with:  node --test test/web_demo.test.js
 */

const test = require('node:test');
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const http = require('node:http');
const net = require('node:net');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const BRIDGE = path.join(REPO_ROOT, 'hg_build', 'hg_alexa_bridge');
const DEMO_SERVER = path.join(REPO_ROOT, 'src', 'bridge', 'hg_demo_server.js');
const CONFIG_ON = path.join(REPO_ROOT, 'config', 'homeguardian.demo.json');
const CONFIG_OFF = path.join(REPO_ROOT, 'config', 'homeguardian.json');
const TOKEN = 'demo-token';
const OPERATOR_SECRET = 'demo-operator-secret-2026';

let tmpDir;
let credsPath;

function setup() {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-webdemo-'));
  credsPath = path.join(tmpDir, 'creds.json');
  fs.writeFileSync(credsPath, JSON.stringify({
    [TOKEN]: { principal: 'demo', allow_status: true, allow_alert_summary: true, expires_at_ms: 0, revoked: false },
  }));
  fs.chmodSync(credsPath, 0o600);
}

function teardown() {
  if (tmpDir) fs.rmSync(tmpDir, { recursive: true, force: true });
}

function startDemo(configPath) {
  return new Promise((resolve, reject) => {
    const child = spawn('node', [DEMO_SERVER, '--bridge', BRIDGE, '--config', configPath,
      '--creds', credsPath, '--port', '0', '--token', TOKEN, '--secret', OPERATOR_SECRET], {
      cwd: tmpDir, stdio: ['ignore', 'pipe', 'pipe'],
    });
    let buf = '';
    const timer = setTimeout(() => { child.kill('SIGKILL'); reject(new Error('demo start timeout')); }, 20000);
    child.stdout.on('data', (d) => {
      buf += d.toString();
      const m = buf.match(/DEMO_READY (\d+)/);
      if (m) { clearTimeout(timer); resolve({ child, port: parseInt(m[1], 10) }); }
    });
    child.stderr.on('data', () => {});
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
  });
}

function get(port, urlPath) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method: 'GET', path: urlPath }, (res) => {
      let data = '';
      res.on('data', (d) => { data += d.toString(); });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(data); } catch { j = { __raw: data }; }
        resolve({ status: res.statusCode, body: j });
      });
    });
    req.on('error', reject);
    req.setTimeout(15000, () => { req.destroy(new Error('timeout')); });
    req.end();
  });
}

function post(port, urlPath, headers = {}, body = '') {
  return new Promise((resolve, reject) => {
    const payload = typeof body === 'string' ? body : JSON.stringify(body);
    const reqHeaders = {
      'host': '127.0.0.1',
      ...headers,
    };
    if (payload && !reqHeaders['content-type'] && !reqHeaders['Content-Type']) {
      reqHeaders['content-type'] = 'application/json';
    }
    if (payload && !reqHeaders['content-length'] && !reqHeaders['Content-Length']) {
      reqHeaders['content-length'] = Buffer.byteLength(payload);
    }
    const req = http.request({ host: '127.0.0.1', port, method: 'POST', path: urlPath, headers: reqHeaders }, (res) => {
      let data = '';
      res.on('data', (d) => { data += d.toString(); });
      res.on('end', () => {
        let j = null;
        try { j = JSON.parse(data); } catch { j = { __raw: data }; }
        resolve({ status: res.statusCode, body: j });
      });
    });
    req.on('error', reject);
    req.setTimeout(15000, () => { req.destroy(new Error('timeout')); });
    if (payload) req.write(payload);
    req.end();
  });
}

// --- tests ---

test('web demo: scenario runs against the real backend and produces a real alert (authenticated)', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const sessionRes = await post(h.port, '/api/session', { 'x-hg-secret': OPERATOR_SECRET });
    assert.equal(sessionRes.status, 200);
    assert.ok(sessionRes.body.csrf, 'csrf token generated');

    const r = await post(h.port, '/api/run-scenario?cycles=5', { 'x-hg-csrf': sessionRes.body.csrf });
    assert.equal(r.status, 200);
    assert.equal(r.body.scenario.ok, true);
    // Real backend counters, not fabricated.
    assert.ok(r.body.scenario.events_processed > 0, 'events processed');
    assert.ok(r.body.scenario.alerts_generated > 0, 'alerts generated');
    assert.ok(r.body.scenario.alerts_delivered >= 1, 'at least one delivered');
    assert.equal(r.body.scenario.synthetic, true);
    // The alert summary reflects durable persistence.
    assert.equal(r.body.summary.ok, true);
    assert.ok(r.body.summary.alert_count >= 1, 'persisted alert visible');
    assert.ok(r.body.summary.alert_ids.length >= 1);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: security gates enforce POST, secret auth, single-use CSRF, and no URL credentials', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);

    // 1. GET /api/run-scenario rejected with 405 Method Not Allowed
    const getScen = await get(h.port, '/api/run-scenario');
    assert.equal(getScen.status, 405);

    // 2. POST /api/session without valid secret rejected with 401
    const unauthSession = await post(h.port, '/api/session', { 'x-hg-secret': 'wrong-secret' });
    assert.equal(unauthSession.status, 401);

    // 3. POST /api/run-scenario without CSRF rejected with 403
    const noCsrf = await post(h.port, '/api/run-scenario');
    assert.equal(noCsrf.status, 403);

    // 4. Legitimate session obtains CSRF
    const sessionRes = await post(h.port, '/api/session', { 'x-hg-secret': OPERATOR_SECRET });
    assert.equal(sessionRes.status, 200);
    const csrf = sessionRes.body.csrf;
    assert.ok(csrf);

    // 5. POST /api/run-scenario with CSRF succeeds
    const okRun = await post(h.port, '/api/run-scenario?cycles=1', { 'x-hg-csrf': csrf });
    assert.equal(okRun.status, 200);

    // 6. Token replay is rejected with 403 (single-use invariant)
    const replayed = await post(h.port, '/api/run-scenario?cycles=1', { 'x-hg-csrf': csrf });
    assert.equal(replayed.status, 403);

    // 7. Credentials in URL query parameters rejected with 400
    const queryCred = await get(h.port, '/api/status?secret=' + OPERATOR_SECRET);
    assert.equal(queryCred.status, 400);
    assert.equal(queryCred.body.error, 'credentials_in_url_forbidden');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: status reflects real capture/consent state', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const r = await get(h.port, '/api/status');
    assert.equal(r.status, 200);
    assert.equal(r.body.status.ok, true);
    assert.equal(r.body.status.capture_enabled, true);  // demo config enables capture
    assert.equal(r.body.status.state, 'Running');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: capture-disabled config fails closed (no observations)', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_OFF);
    const sessionRes = await post(h.port, '/api/session', { 'x-hg-secret': OPERATOR_SECRET });
    assert.equal(sessionRes.status, 200);

    const r = await post(h.port, '/api/run-scenario?cycles=5', { 'x-hg-csrf': sessionRes.body.csrf });
    assert.equal(r.status, 200);
    // Scenario ran but the gate blocked everything.
    assert.equal(r.body.scenario.ok, true);
    assert.equal(r.body.scenario.capture_enabled, false);
    assert.equal(r.body.scenario.events_processed, 0);
    assert.equal(r.body.scenario.alerts_delivered, 0);
    assert.ok(r.body.scenario.note, 'explains fail-closed');
    // No alerts persisted.
    assert.equal(r.body.summary.alert_count, 0);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: serves the UI index page', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const r = await new Promise((resolve, reject) => {
      const req = http.request({ host: '127.0.0.1', port: h.port, method: 'GET', path: '/' }, (res) => {
        let data = '';
        res.on('data', (d) => { data += d.toString(); });
        res.on('end', () => resolve({ status: res.statusCode, body: data, ctype: res.headers['content-type'] }));
      });
      req.on('error', reject);
      req.end();
    });
    assert.equal(r.status, 200);
    assert.ok(r.ctype.includes('text/html'));
    assert.ok(r.body.includes('HomeGuardian'));
    assert.ok(r.body.includes('Simulated'), 'UI must be labelled as simulated');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: path traversal is blocked', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const r = await get(h.port, '/../../../../etc/passwd');
    assert.equal(r.status, 404);  // normalized inside public dir -> not found
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: health is minimal (no secrets/paths/tokens)', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const r = await get(h.port, '/api/health');
    assert.equal(r.status, 200);
    assert.equal(r.body.ok, true);
    assert.equal(r.body.simulated, true);
    const s = JSON.stringify(r.body);
    assert.ok(!s.includes(TOKEN), 'no token in health');
    assert.ok(!s.includes(tmpDir), 'no path in health');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: binds loopback only (LAN IP unreachable)', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    // Derive a non-loopback IPv4 address.
    const nets = os.networkInterfaces();
    let lanIp = null;
    for (const name of Object.keys(nets)) {
      for (const ni of nets[name]) {
        if (ni.family === 'IPv4' && !ni.internal) { lanIp = ni.address; break; }
      }
      if (lanIp) break;
    }
    if (lanIp) {
      const connected = await new Promise((resolve) => {
        const sock = net.connect({ host: lanIp, port: h.port, timeout: 1500 },
          () => { sock.destroy(); resolve(true); });
        sock.on('error', () => resolve(false));
        sock.on('timeout', () => { sock.destroy(); resolve(false); });
      });
      assert.equal(connected, false, 'must not be reachable on LAN IP ' + lanIp);
    }
    // Loopback still works.
    const r = await get(h.port, '/api/health');
    assert.equal(r.status, 200);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('web demo: graceful shutdown on SIGTERM', async () => {
  setup();
  let h;
  try {
    h = await startDemo(CONFIG_ON);
    const exited = await new Promise((resolve) => {
      h.child.on('close', (code) => resolve(code));
      h.child.kill('SIGTERM');
      setTimeout(() => resolve('timeout'), 8000);
    });
    assert.ok(exited === 0 || exited === null, 'clean exit, got ' + exited);
  } finally { teardown(); }
});
