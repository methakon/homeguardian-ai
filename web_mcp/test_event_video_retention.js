'use strict';
// Hermetic FAKE R2 control transport only. Never contact cloud or media sources.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { EventVideos, signR2 } = require('./event_videos');
const DAY = 86400000;
const md5 = crypto.createHash('md5').update('FAKE').digest('base64');
function fixture(t, overrides = {}) {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR, 'hg-retention-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const env = { HG_R2_ACCOUNT_ID: 'a'.repeat(32), HG_R2_BUCKET: 'fake-bucket', HG_R2_ACCESS_KEY_ID: 'FAKE', HG_R2_SECRET_ACCESS_KEY: 'FAKE', HG_EVENT_VIDEO_INDEX_FILE: path.join(dir, 'index.json'), HG_EVENT_VIDEO_CLEANUP_ENABLED: '1', ...overrides };
  let clock = Date.parse('2026-10-24T12:00:00Z');
  const calls = [];
  const options = { env, publicOrigin: 'https://portal.example.test', now: () => clock, transport: async (url, opts) => {
    calls.push({ url, opts });
    if (opts.method === 'DELETE') {
      assert.equal(read().entries.find(e => url.includes(e.id)).status, 'deleting');
      return { status: 204 };
    }
    return { status: 404 };
  } };
  const service = new EventVideos(options);
  const body = (id = 'event-1') => ({ event_id: id, detected_at: '2026-10-01T12:00:00Z', summary: 'FAKE observation', detector: 'fake-observer', severity: 'warning', provenance: 'real', detection_status: 'detected', duration_seconds: 1, size_bytes: 4, content_type: 'video/mp4', content_md5: md5, evidence: { labels: ['person'] } });
  const read = () => JSON.parse(fs.readFileSync(env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8'));
  const seed = async (id = 'event-1', status = 'ready') => {
    const p = await service.prepare(body(id));
    const data = read(), e = data.entries.find(e => e.id === p.video_id);
    e.status = status; e.ready_at = status === 'pending' ? null : clock;
    service._save(service._config(), data);
    return e;
  };
  return { env, service, options, calls, body, read, seed, advance: n => { clock += n; } };
}
test('exact ready-time boundary expires only one oldest, preserves metadata and reuses confirmed quota', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '8' });
  const a = await f.seed('a'); f.advance(1); const b = await f.seed('b');
  assert.equal((await f.service.detail(a.id)).event.clip_status, 'available');
  f.advance(7 * DAY - 2);
  assert.deepEqual(await f.service.cleanupExpired(), { ok: true, enabled: true, examined: 2, expired: 0 });
  assert.equal(f.calls.length, 0);
  f.advance(1);
  assert.deepEqual(await f.service.cleanupExpired(), { ok: true, enabled: true, examined: 2, expired: 1 });
  assert.deepEqual(f.calls.map(c => c.opts.method), ['DELETE', 'HEAD']);
  assert.equal(f.read().entries.find(e => e.id === b.id).status, 'ready');
  assert.equal(f.read().entries.find(e => e.id === a.id).status, 'expired');
  const detail = (await f.service.detail(a.id)).event;
  assert.equal(detail.clip_status, 'expired'); assert.deepEqual(detail.evidence, { labels: ['person'] });
  const list = await f.service.list(new URLSearchParams('from=2026-10-01&to=2026-10-01&page_size=1'));
  assert.equal(list.total, 2); assert.equal(list.total_pages, 2);
  const all = await f.service.list(new URLSearchParams('from=2026-10-01&to=2026-10-01'));
  assert.equal(all.events.find(e => e.id === a.id).clip_status, 'expired');
  await rejects(f.service.playback(a.id), 410, 'event_video_expired');
  await rejects(f.service.prepare(f.body('a')), 409);
  await rejects(f.service.complete(a.id), 409);
  assert.equal((await f.service.latestNotification()).event.id, b.id);
  await f.service.prepare(f.body('c'));
  await rejects(f.service.prepare(f.body('d')), 507);
  f.advance(1); await f.service.cleanupExpired();
  assert.equal((await f.service.latestNotification()).event, null);
  assert.equal(f.read().entries.length, 3);
});
for (const scenario of ['delete-404', 'delete-302', 'delete-500', 'delete-timeout', 'head-200', 'head-302', 'head-500', 'head-timeout', 'save-deleting', 'save-expired']) {
  test('failure stays charged and restart resumes: ' + scenario, async t => {
    const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '4' });
    const e = await f.seed(); f.advance(7 * DAY);
    const transport = f.options.transport, save = f.service._save.bind(f.service);
    f.service.transport = async (url, opts) => {
      if (scenario.startsWith(opts.method.toLowerCase() + '-')) {
        if (scenario.endsWith('timeout')) throw new Error('FAKE timeout');
        return { status: Number(scenario.split('-')[1]) };
      }
      return transport(url, opts);
    };
    f.service._save = (c, data) => {
      if (scenario === 'save-' + data.entries[0].status) throw new Error('FAKE disk fault');
      return save(c, data);
    };
    await rejects(f.service.cleanupExpired(), 503);
    const expected = scenario === 'save-deleting' ? 'ready' : 'deleting';
    assert.equal(f.read().entries[0].status, expected);
    await rejects(f.service.prepare(f.body('other')), 507);
    if (expected === 'deleting') {
      assert.equal((await f.service.detail(e.id)).event.clip_status, 'deleting');
      await rejects(f.service.playback(e.id), 410);
      await rejects(f.service.complete(e.id), 409);
      await rejects(f.service.prepare(f.body()), 409);
      assert.equal((await f.service.latestNotification()).event, null);
    } else assert.equal(f.calls.length, 0);
    const s = new EventVideos(f.options);
    assert.equal((await s.cleanupExpired()).expired, 1);
    assert.equal(f.read().entries[0].status, 'expired');
  });
}
test('pending reservations never expire or become published, concurrent cleanup/prepare uses one shared charge', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '12' });
  const pending = await f.seed('pending', 'pending');
  const ready = await f.seed('ready'); f.advance(7 * DAY);
  const other = new EventVideos(f.options);
  const results = await Promise.all([f.service.cleanupExpired(), other.cleanupExpired(), other.prepare(f.body('new'))]);
  assert.equal(results[0].expired + results[1].expired, 1);
  assert.equal(f.calls.length, 2);
  assert.equal(f.read().entries.find(e => e.id === pending.id).status, 'pending');
  assert.equal(f.read().entries.find(e => e.id === ready.id).status, 'expired');
  assert.equal((await f.service.list(new URLSearchParams('from=2026-10-01&to=2026-10-01'))).total, 1);
  await rejects(f.service.detail(pending.id), 404);
  f.advance(100 * DAY); await f.service.cleanupExpired();
  assert.equal(f.calls.length, 2);
  await f.service.prepare(f.body('last')); await rejects(f.service.prepare(f.body('over')), 507);
});
test('disabled cleanup never resumes prior deleting state or reads a corrupt index', async t => {
  const f = fixture(t); await f.seed('retry', 'deleting');
  const before = fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE);
  f.env.HG_EVENT_VIDEO_CLEANUP_ENABLED = '0';
  await f.service.cleanupExpired(); assert.deepEqual(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE), before);
  fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'CORRUPT');
  await f.service.cleanupExpired(); assert.equal(f.calls.length, 0);
  f.env.HG_EVENT_VIDEO_CLEANUP_ENABLED = '1';
  await rejects(f.service.cleanupExpired(), 503, 'event_video_catalog_unavailable');
  assert.equal(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8'), 'CORRUPT');
});
for (const status of ['unknown', 'deleting', 'expired']) test('strict catalog validates state and ready timestamp: ' + status, async t => {
  const f = fixture(t); const e = await f.seed();
  const data = f.read(); data.entries[0].status = status; data.entries[0].ready_at = e.created_at - 1;
  fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, JSON.stringify(data));
  await rejects(f.service.cleanupExpired(), 503, 'event_video_catalog_unavailable'); assert.equal(f.calls.length, 0);
});
test('signed DELETE uses pinned SigV4 endpoint, method-bound signature and bounded control options', async t => {
  const f = fixture(t); const e = await f.seed(); f.advance(7 * DAY);
  await f.service.cleanupExpired();
  for (const { url, opts } of f.calls) {
    const u = new URL(url);
    assert.equal(u.origin, 'https://' + 'a'.repeat(32) + '.r2.cloudflarestorage.com');
    assert.equal(u.pathname, '/fake-bucket/' + e.object_key);
    assert.equal(u.searchParams.get('X-Amz-Expires'), '120');
    assert.equal(u.searchParams.get('X-Amz-SignedHeaders'), 'host');
    assert.deepEqual(opts, { method: opts.method, timeout: 2000, redirect: 'error', maxBodyBytes: 0 });
  }
  assert.notEqual(new URL(f.calls[0].url).searchParams.get('X-Amz-Signature'), new URL(f.calls[1].url).searchParams.get('X-Amz-Signature'));
});
test('post-rename persistence failure restores charged deleting state', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '4' });
  await f.seed(); f.advance(7 * DAY);
  const save = f.service._save.bind(f.service);
  f.service._save = (c, data) => {
    save(c, data);
    if (data.entries[0].status === 'expired') throw new Error('FAKE post-rename directory fsync failure');
  };
  await rejects(f.service.cleanupExpired(), 503);
  assert.equal(f.read().entries[0].status, 'deleting');
  await rejects(f.service.prepare(f.body('new')), 507);
});
test('independent DELETE and HEAD deadlines enforce 2000ms without waiting in tests', async t => {
  const f = fixture(t); await f.seed(); f.advance(7 * DAY);
  const real = global.setTimeout;
  t.after(() => { global.setTimeout = real; });
  for (const stuck of ['DELETE', 'HEAD']) {
    const deadlines = [];
    global.setTimeout = (fn, ms, ...args) => { deadlines.push(ms); return real(fn, 1, ...args); };
    f.service.transport = async (url, opts) => opts.method === stuck ? new Promise(() => {}) : { status: 204 };
    await rejects(f.service.cleanupExpired(), 503, 'event_video_r2_unavailable');
    assert.ok(deadlines.includes(2000)); assert.equal(f.read().entries[0].status, 'deleting');
  }
});
test('unrecoverable post-rename rollback failure preserves exclusion lock fail-closed', async t => {
  const f = fixture(t); await f.seed(); f.advance(7 * DAY);
  const save = f.service._save.bind(f.service); let expiredWritten = false;
  f.service._save = (c, data) => {
    if (expiredWritten) throw new Error('FAKE rollback failure');
    save(c, data);
    if (data.entries[0].status === 'expired') { expiredWritten = true; throw new Error('FAKE fsync failure'); }
  };
  await rejects(f.service.cleanupExpired(), 503, 'event_video_catalog_unavailable');
  assert.equal(fs.existsSync(f.env.HG_EVENT_VIDEO_INDEX_FILE + '.lock'), true);
});
test('retention starts at publication time and honors configured days, not producer or creation timestamps', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_RETENTION_DAYS: '1' }); const e = await f.seed();
  const data = f.read(); data.entries[0].ready_at += 3500000; f.service._save(f.service._config(), data);
  f.advance(DAY); assert.equal((await f.service.cleanupExpired()).expired, 0); assert.equal(f.calls.length, 0);
  f.advance(3500000); assert.equal((await f.service.cleanupExpired()).expired, 1);
  assert.equal(f.read().entries[0].id, e.id);
});
test('expired historical bytes do not count toward absolute load quota', async t => {
  const f = fixture(t); const e = await f.seed(); const data = f.read();
  data.entries = Array.from({ length: 345 }, (_, i) => {
    const id = crypto.randomUUID();
    return { ...e, id, object_key: `event-videos/${id}.mp4`, status: 'expired', metadata: { ...e.metadata, event_id: 'expired-' + i, size_bytes: 25 * 1024 * 1024 } };
  });
  f.service._save(f.service._config(), data);
  assert.equal((await f.service.cleanupExpired()).expired, 0);
  await f.service.prepare(f.body('new')); assert.equal(f.read().entries.length, 346);
});
test('native control transport destroys response without body listeners and never follows redirect', async t => {
  const f = fixture(t); await f.seed(); f.advance(7 * DAY);
  const https = require('node:https'), { EventEmitter } = require('node:events');
  const request = https.request; t.after(() => { https.request = request; });
  let statuses = [204, 404], destroyed = 0; const methods = [];
  https.request = (url, opts, callback) => {
    methods.push(opts.method); assert.equal(opts.timeout, 2000); assert.equal(opts.maxHeaderSize, 16384);
    const req = new EventEmitter(); req.end = () => {
      callback({ statusCode: statuses.shift(), headers: { location: 'https://never.example.test' }, destroy() { destroyed++; }, on() { assert.fail('must not read response body'); } });
      req.emit('close');
    }; req.destroy = error => { req.emit('error', error); req.emit('close'); }; return req;
  };
  const s = new EventVideos({ ...f.options, transport: undefined });
  assert.equal((await s.cleanupExpired()).expired, 1); assert.deepEqual(methods, ['DELETE', 'HEAD']); assert.equal(destroyed, 2);
  await f.seed('redirect'); f.advance(7 * DAY); statuses = [302];
  await rejects(s.cleanupExpired(), 503, 'event_video_r2_unavailable');
  assert.deepEqual(methods, ['DELETE', 'HEAD', 'DELETE']);
});
test('retry deleting before other due clips, accepts DELETE 200, and preserves notification keys', async t => {
  const f = fixture(t); const old = await f.seed('old'); f.advance(1); const retry = await f.seed('retry', 'deleting');
  f.advance(7 * DAY);
  const transport = f.service.transport;
  f.service.transport = async (url, opts) => {
    const response = await transport(url, opts);
    assert.ok(url.includes(retry.id));
    return opts.method === 'DELETE' ? { status: 200 } : response;
  };
  assert.equal((await f.service.cleanupExpired()).expired, 1);
  assert.equal(f.read().entries.find(e => e.id === old.id).status, 'ready');
  const notification = (await f.service.latestNotification()).event;
  assert.deepEqual(Object.keys(notification).sort(), ['detected_at','event_id','id','portal_url','severity','summary']);
});
const rejects = (p, status, code) => assert.rejects(p, e => e.status === status && (!code || e.code === code));
test('cleanup is strictly opt-in and OFF performs no config/catalog/transport work', async () => {
  for (const flag of [undefined, '', '0', 'true', '01', 1]) {
    const env = new Proxy({ HG_EVENT_VIDEO_CLEANUP_ENABLED: flag }, { get(o, k) { assert.equal(k, 'HG_EVENT_VIDEO_CLEANUP_ENABLED'); return o[k]; } });
    const s = new EventVideos({ env });
    for (const name of ['_config', '_load', '_write', 'transport']) s[name] = () => assert.fail(name);
    assert.deepEqual(await s.cleanupExpired(), { ok: true, enabled: false, examined: 0, expired: 0 });
  }
});
