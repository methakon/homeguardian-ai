'use strict';
// Hermetic lifecycle/auth fixtures only; never contact R2 or real devices.
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { createGateway } = require('./gateway');
const { OidcConfig } = require('./oauth');
const PRODUCER = 'a'.repeat(64);
const ID = '00000000-0000-4000-8000-000000000001';
function scheduler() {
  const jobs = new Map(); let seq = 0;
  return { jobs, setInterval(fn, ms) { const id = { seq: ++seq, unref() {} }; jobs.set(id, { fn, ms }); return id; }, clearInterval(id) { jobs.delete(id); } };
}
async function fixture(t, enabled) {
  const timers = scheduler(), env = { HG_OUTDOOR_REVIEW_ACCESS: 'google_viewers' };
  if (enabled !== undefined) env.HG_EVENT_VIDEO_CLEANUP_ENABLED = enabled;
  let calls = 0, implementation = async () => ({ ok: true, enabled: true, examined: 0, expired: 0 });
  const eventVideos = {
    authenticateProducer(req) { return req.headers.authorization === 'Bearer ' + PRODUCER; },
    async cleanupExpired() { calls++; return implementation(); },
    async playback() { throw Object.assign(new Error('private details'), { status: 410, code: 'event_video_expired' }); },
  };
  const g = createGateway({ env, eventVideos, videoCleanupTimers: timers,
    config: new OidcConfig({ HG_PUBLIC_ORIGIN: 'https://portal.example.test', HG_GOOGLE_CLIENT_ID: 'fixture-client' }), logger: { info() {}, warn() {} },
  });
  await g.listen(); t.after(() => g.close());
  const viewer = g.sessions.create({ role: 'VIEWER', sub: '123456789012345678901', issuer: 'https://accounts.google.com' }).token;
  function request(route, token, method = 'POST', body = {}, origin) {
    return new Promise((resolve, reject) => {
      const payload = JSON.stringify(body), headers = { Host: 'portal.example.test', 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(payload) };
      if (token) headers.Authorization = 'Bearer ' + token;
      if (origin) headers.Origin = origin;
      const r = http.request({ host: '127.0.0.1', port: g.server.address().port, path: route, method, headers }, res => {
        const chunks = []; res.on('data', b => chunks.push(b)); res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks)) }));
      });
      r.on('error', reject); r.end(payload);
    });
  }
  return { g, env, timers, request, viewer, get calls() { return calls; }, setImplementation(fn) { implementation = fn; } };
}
test('retention maintenance requires producer key, empty JSON and no browser Origin', async t => {
  const f = await fixture(t);
  const endpoint = '/internal/event-videos/maintenance';
  assert.equal((await f.request(endpoint)).status, 401);
  assert.equal((await f.request(endpoint, f.viewer)).status, 401);
  assert.equal((await f.request(endpoint, PRODUCER, 'GET')).status, 405);
  assert.equal((await f.request(endpoint, PRODUCER, 'POST', {}, 'https://portal.example.test')).status, 403);
  assert.equal((await f.request(endpoint + '?id=' + ID, PRODUCER)).status, 400);
  assert.equal((await f.request(endpoint, PRODUCER, 'POST', { id: ID })).status, 400);
  assert.equal(f.calls, 0);
  const accepted = await f.request(endpoint, PRODUCER);
  assert.equal(accepted.status, 200); assert.equal(accepted.body.ok, true); assert.equal(f.calls, 1);
});
test('automatic cleanup is OFF by default and nonexact opt-ins never schedule it', async t => {
  for (const flag of [undefined, '', 'true', 'yes', '0']) {
    const f = await fixture(t, flag); assert.equal(f.timers.jobs.size, 0); assert.equal(f.calls, 0);
  }
});
test('explicit opt-in schedules one bounded tick per minute and close cancels future work', async t => {
  const f = await fixture(t, '1');
  assert.equal(f.calls, 0); assert.equal(f.timers.jobs.size, 1);
  const job = [...f.timers.jobs.values()][0]; assert.equal(job.ms, 60000);
  await job.fn(); assert.equal(f.calls, 1);
  f.env.HG_EVENT_VIDEO_CLEANUP_ENABLED = '0'; await job.fn(); assert.equal(f.calls, 1);
  await f.g.close(); assert.equal(f.timers.jobs.size, 0);
});
test('scheduled cleanup never overlaps; close waits for current transaction', async t => {
  const f = await fixture(t, '1'); let finish;
  f.setImplementation(() => new Promise(resolve => { finish = resolve; }));
  const job = [...f.timers.jobs.values()][0];
  const first = job.fn(); await new Promise(r => setImmediate(r));
  job.fn(); assert.equal(f.calls, 1);
  let closed = false; const closing = f.g.close().then(() => { closed = true; });
  await new Promise(r => setImmediate(r)); assert.equal(closed, false);
  finish({ ok: true, enabled: true, examined: 1, expired: 1 }); await first; await closing;
  assert.equal(closed, true); assert.equal(f.timers.jobs.size, 0);
});
test('scheduled failures do not become unhandled rejections or block future ticks', async t => {
  const f = await fixture(t, '1'), job = [...f.timers.jobs.values()][0];
  f.setImplementation(async () => { throw new Error('private storage details'); });
  await job.fn(); await job.fn(); assert.equal(f.calls, 2);
});
test('expired playback is explicit 410 without leaking provider detail', async t => {
  const f = await fixture(t);
  const r = await f.request('/api/event-videos/' + ID + '/playback', f.viewer, 'GET');
  assert.equal(r.status, 410); assert.equal(r.body.error, 'event_video_expired');
  assert.ok(!JSON.stringify(r.body).includes('private details'));
});
test('standalone gateway CLI exits gracefully on SIGTERM', async () => {
  const child = spawn(process.execPath, [require.resolve('./gateway')], {
    env: { PATH: process.env.PATH, TMPDIR: process.env.TMPDIR, HG_GATEWAY_BIND: '127.0.0.1', HG_GATEWAY_PORT: '0' }, stdio: ['ignore', 'pipe', 'pipe'],
  });
  const watchdog = setTimeout(() => child.kill('SIGKILL'), 5000);
  try {
    await new Promise((resolve, reject) => { child.once('error', reject); child.stdout.on('data', b => { if (b.toString().includes('listening on')) resolve(); }); child.once('exit', () => reject(new Error('gateway exited before ready'))); });
    const exited = once(child, 'exit'); child.kill('SIGTERM');
    const [code, signal] = await exited; assert.equal(code, 0); assert.equal(signal, null);
  } finally { clearTimeout(watchdog); if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL'); }
});
