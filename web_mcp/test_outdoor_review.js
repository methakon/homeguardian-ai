'use strict';
// Synthetic authorization fixtures only. Native scenario execution is tested separately.
const assert = require('node:assert/strict');
const http = require('node:http');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createGateway } = require('./gateway');
const { OidcConfig } = require('./oauth');

(async () => {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'hg-review-policy-'));
  const secretFile = path.join(dir, 'bridge.secret');
  fs.writeFileSync(secretFile, 'synthetic-review-secret\n', { mode: 0o600 });
  let checks = 0, runs = 0, nativeReads = 0;
  const realSimulation = process.env.HG_REVIEW_INTEGRATION === '1'
    ? new (require('./simulation_review').SimulationReview)({ binaryPath: process.env.HG_REVIEW_BRIDGE_BIN || path.resolve(__dirname, '../hg_build/hg_alexa_bridge') }) : null;
  const check = (condition) => { assert.ok(condition); checks++; };
  const bridge = http.createServer((req, res) => {
    assert.equal(req.headers['x-hg-secret'], 'synthetic-review-secret');
    if (req.url === '/api/camera/frame') {
      res.writeHead(200, { 'Content-Type': 'image/jpeg' }); res.end(Buffer.from([255,216,255,217]));
    } else {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ configured: true, source_class: 'synthetic_camera_fixture', camera: { state: 'LIVE', provenance: 'synthetic' } }));
    }
  });
  await new Promise(resolve => bridge.listen(0, '127.0.0.1', resolve));
  const env = { HG_PRIVATE_BRIDGE_URL: `http://127.0.0.1:${bridge.address().port}`, HG_PRIVATE_BRIDGE_SECRET_FILE: secretFile };
  const config = new OidcConfig({ HG_PUBLIC_ORIGIN: 'https://portal.example.test', HG_GOOGLE_CLIENT_ID: 'synthetic-client' });
  const g = createGateway({ env, config, logger: { info() {}, warn() {} },
    mcp: { async callTool(name) { nativeReads++; return name === 'get_alert_summary' ? { alert_count: 3, highest_severity: 'warning' } : { state: 'Running', capture_enabled: true }; } },
    simulationReview: {
      async catalogue() { return realSimulation ? realSimulation.catalogue() : { ok: true, default: 'normal', scenarios: [{ id: 'normal', title: 'Synthetic normal' }] }; },
      async run(id) { runs++; return realSimulation ? realSimulation.run(id) : { scenario: { ok: true, scenario_id: id, outcome: 'Synthetic policy fixture' } }; }
    }
  });
  await g.listen();
  const token = g.sessions.create({ sub: '123456789012345678901', role: 'VIEWER', issuer: 'https://accounts.google.com' }).token;
  async function req(route, { method = 'GET', auth = true, origin = 'https://portal.example.test', body } = {}) {
    return new Promise((resolve, reject) => {
      const payload = body === undefined ? null : JSON.stringify(body);
      const headers = { Host: 'portal.example.test', Origin: origin };
      if (auth) headers.Authorization = 'Bearer ' + token;
      if (payload !== null) { headers['Content-Type'] = 'application/json'; headers['Content-Length'] = Buffer.byteLength(payload); }
      const r = http.request({ hostname: '127.0.0.1', port: g.server.address().port, path: route, method, headers }, res => {
        const chunks = []; res.on('data', b => chunks.push(b)); res.on('end', () => {
          const raw = Buffer.concat(chunks).toString(); let json; try { json = JSON.parse(raw); } catch (_) {}
          check(!res.headers['set-cookie']); resolve({ status: res.statusCode, json, raw });
        });
      });
      r.on('error', reject); r.end(payload);
    });
  }
  try {
    check((await req('/api/status')).status === 403);
    check((await req('/api/scenarios')).status === 403);
    env.HG_OUTDOOR_REVIEW_ACCESS = 'google_viewers';
    const me = await req('/api/me');
    check(me.json.role === 'VIEWER' && me.json.household_authorized === false && me.json.outdoor_review_authorized === true);
    for (const route of ['/api/status','/api/alerts','/api/camera/status','/api/camera/frame']) {
      check((await req(route)).status === 200);
      check((await req(route, { auth: false })).status === 401);
    }
    const before = nativeReads;
    for (const route of ['/api/devices','/api/device','/api/consent','/api/admin','/api/feed','/api/upload']) check((await req(route)).status === 403);
    check(nativeReads === before);
    check((await req('/api/camera/status', { method: 'POST', body: {} })).status === 405);
    check((await req('/api/consent', { method: 'POST', body: { granted: true } })).status === 403);
    check((await req('/api/scenarios')).json.scenarios.length === (realSimulation ? 10 : 1));
    check((await req('/api/scenarios', { auth: false })).status === 401);
    const run = await req('/api/run-scenario?scenario=normal', { method: 'POST', body: {} });
    check(run.status === 200 && run.json.synthetic === true && run.json.isolated === true && run.json.live_camera_modified === false && runs === 1);
    check((await req('/api/run-scenario?scenario=normal', { method: 'POST', auth: false, body: {} })).status === 401);
    check((await req('/api/run-scenario?scenario=normal', { method: 'POST', origin: 'https://evil.example', body: {} })).status === 403);
    check((await req('/api/run-scenario?scenario=normal', { method: 'POST', body: { capture: true } })).status === 400);
    check((await req('/api/run-scenario?scenario=normal&url=http://evil.example', { method: 'POST', body: {} })).status === 400);
    check((await req('/api/run-scenario?scenario=normal')).status === 405);
    check(runs === 1);
    env.HG_OUTDOOR_REVIEW_ACCESS = 'true'; // Non-exact opt-in fails closed.
    check((await req('/api/status')).status === 403);
    check((await req('/api/run-scenario?scenario=normal', { method: 'POST', body: {} })).status === 403);
    env.HG_OUTDOOR_REVIEW_ACCESS = 'google_viewers';
    g.sessions.revoke(token);
    check((await req('/api/status')).status === 401);
    check((await req('/api/run-scenario?scenario=normal', { method: 'POST', body: {} })).status === 401);
    console.log(`PASS outdoor review policy: ${checks} assertions`);
  } finally {
    await g.close(); await new Promise(resolve => bridge.close(resolve)); fs.rmSync(dir, { recursive: true, force: true });
  }
})().catch(err => { console.error(err); process.exitCode = 1; });
