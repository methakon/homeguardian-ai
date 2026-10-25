'use strict';
// Hermetic catalog tests with FAKE credentials/control responses; no network/media.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { EventVideos } = require('./event_videos');
function fixture(t, overrides = {}) {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR, 'hg-pressure-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const env = { HG_R2_ACCOUNT_ID: 'a'.repeat(32), HG_R2_BUCKET: 'fake-bucket', HG_R2_ACCESS_KEY_ID: 'FAKE', HG_R2_SECRET_ACCESS_KEY: 'FAKE', HG_EVENT_VIDEO_INDEX_FILE: path.join(dir, 'index.json'), HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES: '10', HG_EVENT_VIDEO_PRESSURE_CLEANUP_ENABLED: '1', ...overrides };
  let clock = Date.parse('2026-10-25T12:00:00Z');
  const calls = [];
  const read = () => JSON.parse(fs.readFileSync(env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8'));
  const options = { env, publicOrigin: 'https://portal.example.test', now: () => clock, transport: async (url, opts) => {
    calls.push(opts.method);
    assert.equal(read().entries.find(e => url.includes(e.id)).status, 'deleting');
    return { status: opts.method === 'DELETE' ? 204 : 404 };
  } };
  const service = new EventVideos(options);
  const body = (id, bytes) => ({ event_id: id, detected_at: new Date(clock).toISOString(), summary: 'FAKE observation', detector: 'fake-observer', severity: 'warning', provenance: 'real', detection_status: 'detected', duration_seconds: 1, size_bytes: bytes, content_type: 'video/mp4', content_md5: crypto.createHash('md5').update('FAKE').digest('base64') });
  async function ready(id, bytes) {
    const prepared = await service.prepare(body(id, bytes));
    const data = read(), entry = data.entries.find(e => e.id === prepared.video_id);
    entry.status = 'ready'; entry.ready_at = clock++;
    service._save(service._config(), data);
    return entry;
  }
  return { env, service, options, calls, read, body, ready };
}
const full = error => error.status === 507 && error.code === 'event_video_storage_full';
test('strictly less than 80 percent counts pending reservations and survives a new service instance', async t => {
  const f = fixture(t);
  await f.service.prepare(f.body('first', 7));
  const restarted = new EventVideos(f.options);
  await assert.rejects(restarted.prepare(f.body('boundary', 1)), full);
  assert.equal(f.read().entries.length, 1);
  assert.deepEqual(f.calls, []);
});
test('pressure reclaims the oldest published clip only after durable DELETE and HEAD confirmation', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1' });
  const old = await f.ready('old', 4);
  await f.service.prepare(f.body('pending', 3));
  const prepared = await f.service.prepare(f.body('new', 3));
  assert.ok(prepared.upload);
  assert.deepEqual(f.calls, ['DELETE', 'HEAD']);
  assert.equal(f.read().entries.find(e => e.id === old.id).status, 'expired');
  assert.equal((await f.service.detail(old.id)).event.clip_status, 'expired');
  assert.equal(f.read().entries.filter(e => e.status === 'pending').length, 2);
});
test('integer ceiling is strictly below 80 percent, respects a smaller configured cap, and has a conservative default', t => {
  const f = fixture(t);
  for (let budget = 1; budget < 100; budget++) {
    f.env.HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES = String(budget);
    const cap = BigInt(f.service._config().storage);
    assert.ok(cap * 5n < BigInt(budget) * 4n);
    assert.ok((cap + 1n) * 5n >= BigInt(budget) * 4n);
  }
  delete f.env.HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES;
  assert.equal(f.service._config().storage, 7999999999);
  f.env.HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES = '12';
  assert.equal(f.service._config().storage, 12);
});
for (const budget of ['0', '-1', '1.5', '1e10', ' 10', '9007199254740992']) test('invalid budget fails closed: ' + budget, async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES: budget });
  await assert.rejects(f.service.prepare(f.body('bad', 1)), e => e.status === 503);
  assert.deepEqual(f.calls, []);
});
test('concurrent independent instances cannot oversubscribe pending quota', async t => {
  const f = fixture(t), other = new EventVideos(f.options);
  const results = await Promise.allSettled([f.service.prepare(f.body('a', 4)), other.prepare(f.body('b', 4))]);
  assert.equal(results.filter(r => r.status === 'fulfilled').length, 1);
  assert.equal(results.filter(r => r.status === 'rejected' && full(r.reason)).length, 1);
  assert.equal(f.read().entries.reduce((n, e) => n + e.metadata.size_bytes, 0), 4);
});
for (const disabled of ['HG_EVENT_VIDEO_CLEANUP_ENABLED', 'HG_EVENT_VIDEO_PRESSURE_CLEANUP_ENABLED']) test('pressure deletion requires both explicit switches: ' + disabled, async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1', [disabled]: '0' });
  await f.ready('old', 7);
  await assert.rejects(f.service.prepare(f.body('new', 1)), full);
  assert.deepEqual(f.calls, []);
  assert.equal(f.read().entries[0].status, 'ready');
});
test('pressure never destroys ready clips when pending reservations alone prevent admission', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1' });
  await f.ready('old', 2);
  await f.service.prepare(f.body('pending', 5));
  await assert.rejects(f.service.prepare(f.body('new', 3)), full);
  assert.deepEqual(f.calls, []);
});
test('one oldest deletion per request bounds work; metadata and charges survive a 507 retry', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1' });
  const a = await f.ready('a', 1), b = await f.ready('b', 1);
  await f.service.prepare(f.body('pending', 5));
  const request = f.body('new', 2);
  await assert.rejects(f.service.prepare(request), full);
  assert.deepEqual(f.calls, ['DELETE', 'HEAD']);
  assert.equal(f.read().entries.find(e => e.id === a.id).status, 'expired');
  assert.equal(f.read().entries.find(e => e.id === b.id).status, 'ready');
  const restarted = new EventVideos(f.options);
  await restarted.prepare(request);
  assert.equal(f.read().entries.find(e => e.id === b.id).status, 'expired');
  assert.equal(f.read().entries.reduce((n, e) => n + (e.status === 'expired' ? 0 : e.metadata.size_bytes), 0), 7);
});
for (const failed of ['DELETE', 'HEAD']) test('unconfirmed pressure deletion stays charged; a new instance resumes: ' + failed, async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1' });
  const old = await f.ready('old', 7), normal = f.options.transport;
  f.service.transport = (url, opts) => opts.method === failed ? Promise.resolve({ status: 500 }) : normal(url, opts);
  await assert.rejects(f.service.prepare(f.body('new', 1)), e => e.status === 503);
  assert.equal(f.read().entries[0].status, 'deleting');
  assert.equal(f.read().entries.length, 1);
  const restarted = new EventVideos(f.options);
  await restarted.prepare(f.body('new', 1));
  assert.equal(f.read().entries.find(e => e.id === old.id).status, 'expired');
});
test('a smaller budget refuses renewed pending PUT authority without erasing the existing charge', async t => {
  const f = fixture(t);
  const body = f.body('same', 7);
  await f.service.prepare(body);
  f.env.HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES = '8';
  await assert.rejects(f.service.prepare(body), full);
  assert.equal(f.read().entries[0].metadata.size_bytes, 7);
  assert.deepEqual(f.calls, []);
});
test('shared deletion transaction refuses pending state before any cloud operation', async t => {
  const f = fixture(t);
  await f.service.prepare(f.body('pending', 7));
  const config = f.service._config();
  await assert.rejects(f.service._write(config, data => f.service._expire(config, data, data.entries[0])), e => e.status === 503);
  assert.deepEqual(f.calls, []);
  assert.equal(f.read().entries[0].status, 'pending');
});
test('idempotent pending retry does not double charge or trigger cleanup', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_CLEANUP_ENABLED: '1' });
  const body = f.body('same', 7);
  const first = await f.service.prepare(body), retry = await f.service.prepare(body);
  assert.equal(first.video_id, retry.video_id);
  assert.equal(f.read().entries.length, 1);
  assert.deepEqual(f.calls, []);
});
