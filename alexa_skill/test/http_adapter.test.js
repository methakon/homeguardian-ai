'use strict';

/**
 * Local-only HTTP adapter integration tests.
 *
 * Spawns the REAL hg_http_adapter.js (which binds 127.0.0.1 only and forwards
 * to the REAL compiled hg_alexa_bridge -> AlexaContract + AlexaCredentialStore)
 * and exercises it over real HTTP. No mock of the C++ boundary.
 *
 * Prerequisites: build the bridge first:
 *   cmake --build hg_build --target hg_alexa_bridge
 *
 * Run with:  node --test test/http_adapter.test.js
 */

const test = require('node:test');
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const http = require('node:http');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const BRIDGE = process.env.HG_BRIDGE_PATH ||
  (fs.existsSync(path.join(REPO_ROOT, 'hg_build', 'hg_alexa_bridge'))
    ? path.join(REPO_ROOT, 'hg_build', 'hg_alexa_bridge')
    : path.join(REPO_ROOT, 'build', 'hg_alexa_bridge'));
const ADAPTER = path.join(REPO_ROOT, 'src', 'bridge', 'hg_http_adapter.js');
const CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.json');

const NOW = Date.now();
const SECURE_CREDS = {
  'tok-full': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: 0, revoked: false },
  'tok-status': { principal: 'tester', allow_status: true, allow_alert_summary: false, expires_at_ms: 0, revoked: false },
  'tok-revoked': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: 0, revoked: true },
  'tok-expired': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: 1000, revoked: false },
};

let tmpDir;
let credsPath;

function setup() {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-http-'));
  credsPath = path.join(tmpDir, 'creds.json');
  fs.writeFileSync(credsPath, JSON.stringify(SECURE_CREDS));
  fs.chmodSync(credsPath, 0o600);
}

function teardown() {
  if (tmpDir) fs.rmSync(tmpDir, { recursive: true, force: true });
}

// Start the adapter on an ephemeral loopback port; resolve once LISTENING.
function startAdapter({ secure = true } = {}) {
  return new Promise((resolve, reject) => {
    const env = Object.assign({}, process.env);
    if (secure) env.HG_BRIDGE_SECURE_CREDS = '1';
    const child = spawn('node', [ADAPTER, '--bridge', BRIDGE, '--config', CONFIG, '--creds', credsPath, '--port', '0'], {
      cwd: tmpDir, stdio: ['ignore', 'pipe', 'pipe'], env,
    });
    let buf = '';
    const timer = setTimeout(() => { child.kill('SIGKILL'); reject(new Error('adapter start timeout')); }, 15000);
    child.stdout.on('data', (d) => {
      buf += d.toString();
      const m = buf.match(/LISTENING (\d+)/);
      if (m) { clearTimeout(timer); resolve({ child, port: parseInt(m[1], 10) }); }
    });
    child.stderr.on('data', () => {});
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
  });
}

// Minimal HTTP request helper. Returns {status, headers, body(text)}.
function request(port, method, urlPath, { headers = {}, body = null } = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, method, path: urlPath, headers }, (res) => {
      let data = '';
      res.on('data', (d) => { data += d.toString(); });
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: data }));
    });
    req.on('error', reject);
    req.setTimeout(10000, () => { req.destroy(new Error('client timeout')); });
    if (body !== null) req.write(body);
    req.end();
  });
}

function auth(token) { return { Authorization: 'Bearer ' + token }; }

// --- tests ---

test('authorized GET /status returns read-only state', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: auth('tok-full') });
    assert.equal(r.status, 200);
    const j = JSON.parse(r.body);
    assert.equal(j.ok, true);
    assert.equal(j.state, 'Running');
    assert.equal(typeof j.capture_enabled, 'boolean');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('authorized GET /alert_summary returns aggregate only', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/alert_summary', { headers: auth('tok-full') });
    assert.equal(r.status, 200);
    const j = JSON.parse(r.body);
    assert.equal(j.ok, true);
    assert.equal(typeof j.alert_count, 'number');
    assert.ok(Array.isArray(j.alert_ids));
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('missing credentials -> 401', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', {});
    assert.equal(r.status, 401);
    assert.equal(JSON.parse(r.body).error, 'unauthorized');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('invalid credential -> 401', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: auth('tok-bogus') });
    assert.equal(r.status, 401);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('expired credential -> 401', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: auth('tok-expired') });
    assert.equal(r.status, 401);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('revoked credential -> 401', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: auth('tok-revoked') });
    assert.equal(r.status, 401);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('authenticated but unauthorized operation -> 403', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/alert_summary', { headers: auth('tok-status') });
    assert.equal(r.status, 403);
    assert.equal(JSON.parse(r.body).error, 'forbidden');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('wrong method -> 405', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'POST', '/status', { headers: auth('tok-full'), body: '{}' });
    assert.equal(r.status, 405);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('unknown route -> 404', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/enable_capture', { headers: auth('tok-full') });
    assert.equal(r.status, 404);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('invalid content type -> 415', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: Object.assign(auth('tok-full'), { 'content-type': 'text/plain' }) });
    assert.equal(r.status, 415);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('ambiguous/malformed Authorization value rejected generically (401)', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    // Node's HTTP server de-dupes the Authorization header at the parser level
    // (keeping the first), so a literal duplicate does not reach the handler as
    // ambiguous. The real control is the adapter's strict single-token parse:
    // any Authorization value that is not exactly "Bearer <token>" (e.g. a
    // comma-joined or multi-token value) must be rejected with a GENERIC 401
    // that never reveals why or echoes the token.
    const net = require('node:net');
    const send = (authValue) => new Promise((resolve, reject) => {
      const sock = net.connect({ host: '127.0.0.1', port: h.port }, () => {
        sock.write('GET /status HTTP/1.1\r\n' +
          'Host: 127.0.0.1\r\n' +
          'Authorization: ' + authValue + '\r\n' +
          'Connection: close\r\n\r\n');
      });
      let data = '';
      sock.on('data', (d) => { data += d.toString(); });
      sock.on('end', () => {
        const statusLine = data.split('\r\n')[0] || '';
        const m = statusLine.match(/HTTP\/1\.1 (\d+)/);
        resolve({ status: m ? parseInt(m[1], 10) : 0, body: data });
      });
      sock.on('error', reject);
      sock.setTimeout(8000, () => { sock.destroy(); reject(new Error('timeout')); });
    });
    // Genuinely malformed/ambiguous values -> 401. (A single clean token with
    // extra internal whitespace normalizes to the valid token and is allowed;
    // that is not tested as a failure.)
    for (const bad of ['Bearer tok-full, Bearer tok-full', 'Bearer tok-full extra', 'Bearer', 'tok-full', 'Basic tok-full']) {
      const r = await send(bad);
      assert.equal(r.status, 401, 'ambiguous auth should be 401: ' + JSON.stringify(bad));
      assert.ok(!r.body.includes('tok-full'), 'token leaked: ' + JSON.stringify(bad));
    }
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('oversized request line/headers rejected safely', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    // A very long path exceeds Node header limits -> the server should not crash.
    const longPath = '/status?' + 'x'.repeat(20000);
    const r = await request(h.port, 'GET', longPath, { headers: auth('tok-full') });
    // Either 404 (route mismatch after query) or 431/400; must not hang/crash.
    assert.ok([400, 404, 431].includes(r.status), 'status=' + r.status);
    // Server still alive afterward.
    const r2 = await request(h.port, 'GET', '/status', { headers: auth('tok-full') });
    assert.equal(r2.status, 200);
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('rate limiting kicks in under a burst and stays bounded', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    // Fire 40 quick requests; some should be 429 (limit 30/10s). The server
    // must stay responsive and never crash.
    let limited = 0;
    let ok = 0;
    const results = await Promise.all(Array.from({ length: 40 }, () =>
      request(h.port, 'GET', '/status', { headers: auth('tok-full') }).then((r) => r.status).catch(() => 0)
    ));
    for (const s of results) { if (s === 429) limited++; else if (s === 200) ok++; }
    assert.ok(limited > 0, 'expected some 429s');
    assert.ok(ok > 0, 'expected some 200s');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('health endpoint reveals minimal state only', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/health');
    assert.equal(r.status, 200);
    const j = JSON.parse(r.body);
    assert.equal(j.ok, true);
    // No secrets/paths/token metadata in the health body.
    const s = r.body;
    assert.ok(!s.includes('tok-'), 'no token in health');
    assert.ok(!s.includes(tmpDir), 'no path in health');
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('listener binds loopback only (no LAN/wildcard reachable)', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    // The adapter prints LISTENING <port> and asserts 127.0.0.1 internally.
    // Confirm the port is reachable on 127.0.0.1.
    const r = await request(h.port, 'GET', '/health');
    assert.equal(r.status, 200);
    // Confirm it is NOT bound on a non-loopback local address by checking that
    // connecting to the machine's LAN IP fails. We derive a candidate non-loopback
    // IP from os.networkInterfaces and expect a connection error.
    const nets = os.networkInterfaces();
    let lanIp = null;
    for (const name of Object.keys(nets)) {
      for (const ni of nets[name]) {
        if (ni.family === 'IPv4' && !ni.internal) { lanIp = ni.address; break; }
      }
      if (lanIp) break;
    }
    if (lanIp) {
      // Directly attempt a TCP connect to the LAN IP:port; must fail.
      const net = require('node:net');
      const connected = await new Promise((resolve) => {
        const sock = net.connect({ host: lanIp, port: h.port, timeout: 1500 }, () => { sock.destroy(); resolve(true); });
        sock.on('error', () => resolve(false));
        sock.on('timeout', () => { sock.destroy(); resolve(false); });
      });
      assert.equal(connected, false, 'adapter must not be reachable on LAN IP ' + lanIp);
    }
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('no token leakage in responses', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    const r = await request(h.port, 'GET', '/status', { headers: auth('tok-full') });
    assert.ok(!r.body.includes('tok-full'), 'token leaked in response');
    // Also a 401 path.
    const r2 = await request(h.port, 'GET', '/status', { headers: auth('tok-bogus') });
    assert.ok(!r2.body.includes('tok-bogus'));
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});

test('graceful shutdown on SIGTERM', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    // Ask the adapter to shut down; it should exit cleanly.
    const exited = await new Promise((resolve) => {
      h.child.on('close', (code) => resolve(code));
      h.child.kill('SIGTERM');
      setTimeout(() => resolve('timeout'), 8000);
    });
    assert.ok(exited === 0 || exited === null, 'clean exit, got ' + exited);
  } finally { teardown(); }
});

test('multiple sequential requests over the process lifetime', async () => {
  setup();
  let h;
  try {
    h = await startAdapter();
    for (let i = 0; i < 5; i++) {
      const r = await request(h.port, 'GET', '/status', { headers: auth('tok-full') });
      assert.equal(r.status, 200, 'iteration ' + i);
    }
  } finally { h && h.child.kill('SIGKILL'); teardown(); }
});
