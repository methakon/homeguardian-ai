'use strict';

/**
 * Restart + persistence integration tests across REAL process boundaries.
 *
 * The bridge (hg_alexa_bridge) opens its audit database in its working
 * directory. To verify restart semantics with real processes:
 *   1. Seed the audit DB with an alert (simulating a prior process that
 *      delivered a synthetic alert) using a small seeding step.
 *   2. Start a fresh bridge process against the SAME database.
 *   3. Query the real C++ contract for status and alert_summary.
 *   4. Verify the prior alert is reported as part of the aggregate count
 *      (historical), and that a fresh bridge with no seed reports zero.
 *
 * These use isolated TEMPORARY databases only. No real database is touched.
 *
 * Prerequisite: build the bridge (see integration.test.js).
 *
 * Run with:  node --test test/restart.test.js
 */

const test = require('node:test');
const assert = require('node:assert');
const { spawn, execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const BRIDGE = path.join(REPO_ROOT, 'hg_build', 'hg_alexa_bridge');
const CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.json');
// The main app binary, used to produce a REAL synthetic alert into a temp DB.
const APP = path.join(REPO_ROOT, 'hg_build', 'homeguardian');

const NOW = Date.now();
const CREDS = {
  'tok-full': { principal: 'tester', allow_status: true, allow_alert_summary: true, expires_at_ms: 0 },
};

function writeCreds(dir) {
  const p = path.join(dir, 'creds.json');
  fs.writeFileSync(p, JSON.stringify(CREDS));
  return p;
}

function runBridge(credsPath, cwd, requestLines, { timeoutMs = 15000 } = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(BRIDGE, [CONFIG, credsPath], { cwd, stdio: ['pipe', 'pipe', 'pipe'] });
    let out = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('bridge timed out'));
    }, timeoutMs);
    child.stdout.on('data', (d) => { out += d.toString(); });
    child.stderr.on('data', () => {});
    child.on('error', (e) => { if (!settled) { settled = true; clearTimeout(timer); reject(e); } });
    child.on('close', (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      const lines = out.split('\n').filter((l) => l.trim()).map((l) => {
        try { return JSON.parse(l); } catch { return { __raw: l }; }
      });
      resolve({ lines, code });
    });
    for (const line of requestLines) child.stdin.write(line + '\n');
    child.stdin.write('shutdown\n');
    child.stdin.end();
  });
}

function body(op, token) {
  return JSON.stringify({ version: 1, operation: op, credential: token });
}

test('restart: a prior alert is visible to a fresh bridge process', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-restart-'));
  try {
    // 1. Produce a REAL synthetic alert by running the actual app binary with
    //    capture enabled, in the temp dir, so it writes homeguardian_audit.db
    //    there. We point the app at a capture-on config copy.
    const onConfig = path.join(dir, 'on.json');
    const cfg = JSON.parse(fs.readFileSync(CONFIG, 'utf8'));
    cfg.media_capture_enabled = true;
    fs.writeFileSync(onConfig, JSON.stringify(cfg));

    // Run the app briefly (it runs bounded simulator cycles then idles until
    // signaled); SIGTERM after a moment for a clean shutdown.
    await new Promise((resolve, reject) => {
      const app = spawn(APP, [onConfig], { cwd: dir, stdio: ['ignore', 'ignore', 'pipe'] });
      const t = setTimeout(() => app.kill('SIGTERM'), 3500);
      app.on('close', (code) => { clearTimeout(t); resolve(code); });
      app.on('error', reject);
    });

    // The app should have written an audit DB with >=1 alert.
    const dbPath = path.join(dir, 'homeguardian_audit.db');
    assert.ok(fs.existsSync(dbPath), 'app should have created an audit DB');

    // Copy that DB to the bridge's expected name so the bridge reads the SAME
    // historical data (simulating a restart against the persisted store).
    fs.copyFileSync(dbPath, path.join(dir, 'homeguardian_bridge_audit.db'));

    // 2/3. Start a FRESH bridge process against the same directory.
    const credsPath = writeCreds(dir);
    const { lines } = await runBridge(credsPath, dir, [
      body('status', 'tok-full'),
      body('alert_summary', 'tok-full'),
    ]);

    // 4. The bridge reports a live app and a NON-empty aggregate alert count
    //    (the historical alert from the prior process is visible).
    assert.equal(lines[0].ok, true);
    assert.equal(lines[0].state, 'Running');
    assert.equal(lines[1].ok, true);
    assert.ok(lines[1].alert_count >= 1, 'historical alert should be counted');
    assert.ok(Array.isArray(lines[1].alert_ids));
    assert.ok(lines[1].alert_ids.length >= 1);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('restart: a fresh bridge with no seed reports zero alerts', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-fresh-'));
  try {
    const credsPath = writeCreds(dir);
    const { lines } = await runBridge(credsPath, dir, [body('alert_summary', 'tok-full')]);
    assert.equal(lines[0].ok, true);
    assert.equal(lines[0].alert_count, 0);
    assert.deepEqual(lines[0].alert_ids, []);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('degraded: unreadable credentials make every request unauthorized', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-nocreds-'));
  try {
    // Point at a credentials file that does not exist -> verifier fails closed.
    const missing = path.join(dir, 'does-not-exist.json');
    const { lines } = await runBridge(missing, dir, [body('status', 'tok-full')]);
    assert.equal(lines[0].ok, false);
    assert.equal(lines[0].error, 'unauthorized');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('process safety: bridge exits non-zero on bad usage (missing args)', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hg-usage-'));
  try {
    const code = await new Promise((resolve, reject) => {
      const child = spawn(BRIDGE, [], { cwd: dir, stdio: ['ignore', 'pipe', 'pipe'] });
      const t = setTimeout(() => { child.kill('SIGKILL'); resolve('timeout'); }, 8000);
      child.on('close', (c) => { clearTimeout(t); resolve(c); });
      child.on('error', reject);
    });
    assert.equal(code, 2);  // usage error exit code
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
