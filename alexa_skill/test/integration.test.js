'use strict';

/**
 * Local end-to-end integration tests: the JavaScript adapter drives the ACTUAL
 * compiled C++ bridge (hg_alexa_bridge) across a real process boundary over
 * newline-delimited JSON. This is NOT a mock — the child process runs the real
 * AlexaContract, Application status, and PersistenceAudit.
 *
 * Safety:
 *  - The child is spawned with an argument ARRAY (no shell), so no shell
 *    injection is possible.
 *  - Requests and responses are size-bounded; the test fails closed on a
 *    timeout or unexpected output rather than hanging or crashing.
 *  - Credentials are test-only, generated per-run, and kept out of version
 *    control (written to a temp file, removed in cleanup). Never logged.
 *
 * Prerequisite: build the bridge first:
 *   cmake -S . -B hg_build && cmake --build hg_build --target hg_alexa_bridge
 *
 * Run with:  node --test test/integration.test.js
 */

const test = require('node:test');
const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const BRIDGE = path.join(REPO_ROOT, 'hg_build', 'hg_alexa_bridge');
const CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.json');

// Deterministic test-only credentials. NOT real secrets. Written to a temp
// file, removed after. The token strings never appear in assertions or logs.
const NOW = Date.now();
const FAR = NOW + 365 * 24 * 3600 * 1000;
const CREDS = {
  'tok-full': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: 0 },
  'tok-status': { principal: 'tester', allow_status: true, allow_alert_summary: false, expires_at_ms: 0 },
  'tok-noperm': { principal: 'tester', allow_status: false, allow_alert_summary: false, expires_at_ms: 0 },
  'tok-expired': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: NOW - 1000 },
};

let tmpDir;
let credsPath;
let auditDir;

function setup() {
  tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-bridge-'));
  credsPath = path.join(tmpDir, 'creds.json');
  fs.writeFileSync(credsPath, JSON.stringify(CREDS));
  // The bridge writes its audit DB in the CWD; use the temp dir so tests are
  // isolated and never touch a real database.
  auditDir = tmpDir;
}

function teardown() {
  if (tmpDir) fs.rmSync(tmpDir, { recursive: true, force: true });
}

// Spawn the real bridge and collect NDJSON responses for the given request
// lines. Resolves { lines, code, stderr }. Rejects on timeout.
function runBridge(requestLines, { timeoutMs = 15000 } = {}) {
  return new Promise((resolve, reject) => {
    if (!fs.existsSync(BRIDGE)) {
      reject(new Error('bridge binary not built: ' + BRIDGE +
        ' — run: cmake --build hg_build --target hg_alexa_bridge'));
      return;
    }
    // Argument ARRAY, no shell => no shell injection.
    const child = spawn(BRIDGE, [CONFIG, credsPath], {
      cwd: auditDir,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    let out = '';
    let err = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('bridge timed out'));
    }, timeoutMs);

    child.stdout.on('data', (d) => {
      out += d.toString();
      if (out.length > 1024 * 1024) {  // 1MB output bound
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        child.kill('SIGKILL');
        reject(new Error('bridge output exceeded bound'));
      }
    });
    child.stderr.on('data', (d) => { err += d.toString(); });
    child.on('error', (e) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      reject(e);
    });
    child.on('close', (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      const lines = out.split('\n').filter((l) => l.trim().length > 0).map((l) => {
        try { return JSON.parse(l); } catch { return { __raw: l }; }
      });
      resolve({ lines, code, stderr: err });
    });

    // Write all requests, then a shutdown control line, then close stdin.
    for (const line of requestLines) child.stdin.write(line + '\n');
    child.stdin.write('shutdown\n');
    child.stdin.end();
  });
}

function body(op, token, extra = {}) {
  return JSON.stringify(Object.assign({ version: 1, operation: op, credential: token }, extra));
}

// --- tests ---

test('authenticated status request returns real app state', async () => {
  setup();
  try {
    const { lines, code } = await runBridge([body('status', 'tok-full')]);
    assert.equal(lines.length, 1);
    assert.equal(lines[0].ok, true);
    assert.equal(lines[0].error, 'ok');
    assert.equal(lines[0].state, 'Running');
    assert.equal(lines[0].capture_enabled, false);
    assert.equal(code, 0);
  } finally { teardown(); }
});

test('authenticated alert_summary returns real (empty) summary', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('alert_summary', 'tok-full')]);
    assert.equal(lines[0].ok, true);
    assert.equal(lines[0].alert_count, 0);
    assert.deepEqual(lines[0].alert_ids, []);
    assert.equal(lines[0].highest_severity, '');
  } finally { teardown(); }
});

test('missing credential is rejected (malformed)', async () => {
  setup();
  try {
    const { lines } = await runBridge([JSON.stringify({ version: 1, operation: 'status' })]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'malformed');
  } finally { teardown(); }
});

test('invalid credential fails closed (unauthorized)', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('status', 'tok-bogus')]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'unauthorized');
  } finally { teardown(); }
});

test('expired credential fails closed (unauthorized)', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('status', 'tok-expired')]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'unauthorized');
  } finally { teardown(); }
});

test('authenticated but unauthorized operation is forbidden', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('alert_summary', 'tok-status')]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'forbidden');
  } finally { teardown(); }
});

test('authenticated principal with no permissions is forbidden', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('status', 'tok-noperm')]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'forbidden');
  } finally { teardown(); }
});

test('malformed JSON is rejected', async () => {
  setup();
  try {
    const { lines } = await runBridge(['this is not json']);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'malformed');
  } finally { teardown(); }
});

test('unknown field is rejected', async () => {
  setup();
  try {
    const { lines } = await runBridge([body('status', 'tok-full', { extra: 1 })]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'malformed');
  } finally { teardown(); }
});

test('oversized request is rejected', async () => {
  setup();
  try {
    // Exceed the contract's 4096-byte request bound.
    const big = JSON.stringify({ version: 1, operation: 'status', credential: 'x'.repeat(5000) });
    const { lines } = await runBridge([big]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'malformed');
  } finally { teardown(); }
});

test('unsupported contract version is rejected', async () => {
  setup();
  try {
    const { lines } = await runBridge([JSON.stringify({ version: 2, operation: 'status', credential: 'tok-full' })]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'unsupported_version');
  } finally { teardown(); }
});

test('unsupported operation is rejected (no mutation)', async () => {
  setup();
  try {
    for (const op of ['enable_capture', 'grant_consent', 'set_config', 'activate_sensor']) {
      const { lines } = await runBridge([body(op, 'tok-full')]);
      assert.equal(lines[0].ok, false, op);
      assert.equal(lines[0].error, 'malformed', op);
    }
  } finally { teardown(); }
});

test('multiple requests over one connection are handled in order', async () => {
  setup();
  try {
    const { lines } = await runBridge([
      body('status', 'tok-full'),
      body('alert_summary', 'tok-full'),
      body('status', 'tok-bogus'),
    ]);
    assert.equal(lines.length, 3);
    assert.equal(lines[0].ok, true);
    assert.equal(lines[1].ok, true);
    assert.equal(lines[2].error, 'unauthorized');
  } finally { teardown(); }
});

test('clean exit on shutdown control line and EOF', async () => {
  setup();
  try {
    const { code } = await runBridge([body('status', 'tok-full')]);
    assert.equal(code, 0);
  } finally { teardown(); }
});

test('status reflects the real config capture kill-switch (read-only)', async () => {
  setup();
  try {
    // Write a capture-ENABLED config copy; the bridge must REPORT it truthfully
    // but still only serve reads (it cannot enable capture itself).
    const cfg = JSON.parse(fs.readFileSync(CONFIG, 'utf8'));
    cfg.media_capture_enabled = true;
    const onConfig = path.join(tmpDir, 'on.json');
    fs.writeFileSync(onConfig, JSON.stringify(cfg));

    const res = await new Promise((resolve, reject) => {
      const child = spawn(BRIDGE, [onConfig, credsPath], { cwd: auditDir, stdio: ['pipe', 'pipe', 'pipe'] });
      let out = '';
      let settled = false;
      const timer = setTimeout(() => { if (!settled) { settled = true; child.kill('SIGKILL'); reject(new Error('timeout')); } }, 15000);
      child.stdout.on('data', (d) => { out += d.toString(); });
      child.on('close', () => { if (settled) return; settled = true; clearTimeout(timer); resolve(JSON.parse(out.trim().split('\n')[0])); });
      child.stdin.write(body('status', 'tok-full') + '\n');
      child.stdin.write('shutdown\n');
      child.stdin.end();
    });
    assert.equal(res.ok, true);
    assert.equal(res.capture_enabled, true);  // reflects real config, read-only
  } finally { teardown(); }
});

test('config load failure fails safe (capture reported disabled, still serves)', async () => {
  setup();
  try {
    // Point the bridge at a missing config file. It must NOT crash and must
    // fail safe: capture reported disabled, status still served.
    const missing = path.join(tmpDir, 'no-such-config.json');
    const child = spawn(BRIDGE, [missing, credsPath], { cwd: auditDir, stdio: ['pipe', 'pipe', 'pipe'] });
    let out = '';
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => { child.kill('SIGKILL'); reject(new Error('timeout')); }, 15000);
      child.stdout.on('data', (d) => { out += d.toString(); });
      child.on('close', () => { clearTimeout(timer); resolve(); });
      child.stdin.write(body('status', 'tok-full') + '\n');
      child.stdin.write('shutdown\n');
      child.stdin.end();
    });
    const res = JSON.parse(out.trim().split('\n')[0]);
    assert.equal(res.ok, true);
    assert.equal(res.capture_enabled, false);  // fail-safe
  } finally { teardown(); }
});
