'use strict';
// FAKE/local R2 transport only: these tests never upload media or access R2.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const FAKE_BYTES = Buffer.alloc(1234, 0x48); // Arbitrary local bytes, NOT an actual detected clip.
const FAKE_MD5 = crypto.createHash('md5').update(FAKE_BYTES).digest('base64');
const FAKE_ETAG = '"' + crypto.createHash('md5').update(FAKE_BYTES).digest('hex') + '"';
const { EventVideos, EventVideoError, signR2 } = require('./event_videos');
function fixture(t, overrides = {}) {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'hg-event-video-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const env = { HG_R2_ACCOUNT_ID: 'a'.repeat(32), HG_R2_BUCKET: 'fake-test-bucket', HG_R2_ACCESS_KEY_ID: 'FAKEACCESS', HG_R2_SECRET_ACCESS_KEY: 'FAKESECRET', HG_EVENT_VIDEO_INDEX_FILE: path.join(dir, 'index.json'), HG_EVENT_VIDEO_INGEST_TOKEN_FILE: path.join(dir, 'ingest'), ...overrides };
  fs.writeFileSync(env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 'FAKE-producer-token\n', { mode: 0o600 });
  let clock = Date.parse('2026-10-24T12:00:00Z');
  const calls = [];
  const options = { env, publicOrigin: 'https://portal.example.test', now: () => clock, transport: async (url, opts) => { calls.push({ url, opts }); return { status: 200, headers: { 'content-type': 'video/mp4', 'content-length': '1234', etag: FAKE_ETAG } }; } };
  return { dir, env, calls, options, service: new EventVideos(options), advance: n => { clock += n; }, body: (extra = {}) => ({ event_id: 'event-1', detected_at: '2026-10-24T12:00:00Z', summary: 'Observer detected a person', detector: 'trusted-observer', severity: 'warning', provenance: 'real', detection_status: 'detected', duration_seconds: 4, size_bytes: FAKE_BYTES.length, content_type: 'video/mp4', content_md5: FAKE_MD5, ...extra }) };
}
const rejects = (promise, status, code) => assert.rejects(promise, e => e instanceof EventVideoError && e.status === status && (!code || e.code === code));
test('reservation → HEAD → ready, private durable restart/read/playback/notification', async t => {
  const f = fixture(t);
  const p = await f.service.prepare(f.body({ evidence: { objects: 1 } }));
  assert.equal(p.ok, true); assert.equal(p.upload.method, 'PUT');
  assert.equal(p.complete_path, `/internal/event-videos/uploads/${p.video_id}/complete`);
  assert.equal(p.portal_url, `https://portal.example.test/?event_video=${p.video_id}#event-videos`);
  assert.deepEqual(p.upload.headers, { 'Content-Type': 'video/mp4', 'Content-Length': '1234', 'Content-MD5': FAKE_MD5, 'If-None-Match': '*' });
  assert.equal((await f.service.list(new URLSearchParams())).total, 0);
  const done = await f.service.complete(p.video_id);
  assert.equal(done.event.id, p.video_id); assert.equal(done.event.provenance, 'real');
  assert.equal(f.calls.length, 1); assert.equal(f.calls[0].opts.method, 'HEAD');
  const restarted = new EventVideos(f.options);
  assert.deepEqual((await restarted.detail(p.video_id)).event, done.event);
  assert.equal((await restarted.list(new URLSearchParams())).total, 1);
  assert.deepEqual((await restarted.latestNotification()).event, { id: p.video_id, event_id: 'event-1', summary: 'Observer detected a person', severity: 'warning', detected_at: '2026-10-24T12:00:00.000Z', portal_url: p.portal_url });
  const play = await restarted.playback(p.video_id); assert.equal(play.expires_in, 120);
  assert.equal(new URL(play.url).searchParams.get('X-Amz-Expires'), '120');
  assert.deepEqual(done.event.evidence, { objects: 1 });
  const exposed = JSON.stringify([done, await restarted.list(new URLSearchParams()), await restarted.detail(p.video_id)]);
  for (const secret of ['FAKEACCESS', 'FAKESECRET', 'FAKE-producer-token', 'object_key', 'X-Amz']) assert.ok(!exposed.includes(secret));
  const disk = fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8');
  for (const secret of ['FAKEACCESS', 'FAKESECRET', 'FAKE-producer-token', 'X-Amz', 'https:']) assert.ok(!disk.includes(secret));
  assert.equal(fs.statSync(f.env.HG_EVENT_VIDEO_INDEX_FILE).mode & 0o777, 0o600);
});

test('fresh-file producer authentication never authorizes VIEWER cookies/sessions', t => {
  const f = fixture(t), auth = token => f.service.authenticateProducer({ headers: { authorization: 'Bearer ' + token } });
  assert.equal(typeof auth('FAKE-producer-token'), 'boolean');
  assert.equal(auth('FAKE-producer-token'), true);
  for (const token of ['', 'viewer-session-token', 'FAKE-producer-token ', 'x'.repeat(5000)]) assert.equal(auth(token), false);
  for (const req of [null, {}, { headers: {} }, { headers: { cookie: 'session=FAKE-producer-token' }, session: { role: 'VIEWER' } }, { headers: { authorization: ['Bearer FAKE-producer-token'] } }, { headers: { authorization: 'bearer FAKE-producer-token' } }]) assert.equal(f.service.authenticateProducer(req), false);
  fs.writeFileSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 'FAKE-rotated-token');
  assert.equal(auth('FAKE-producer-token'), false); assert.equal(auth('FAKE-rotated-token'), true);
  fs.chmodSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 0o644); assert.equal(auth('FAKE-rotated-token'), false);
  fs.chmodSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 0o600);
  fs.renameSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, path.join(f.dir, 'actual'));
  fs.symlinkSync(path.join(f.dir, 'actual'), f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE); assert.equal(auth('FAKE-rotated-token'), false);
  delete f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE; assert.equal(auth('FAKE-rotated-token'), false);
});

const invalidBodies = [
  ['missing event', { event_id: undefined }], ['invalid event', { event_id: '../object' }], ['long event', { event_id: 'a'.repeat(129) }], ['invalid alert', { alert_id: 'alert id' }],
  ['synthetic', { provenance: 'synthetic' }], ['absent provenance', { provenance: undefined }], ['non-detection', { detection_status: 'not_detected' }], ['absent detection', { detection_status: undefined }],
  ['wrong mime', { content_type: 'video/webm' }], ['mime parameters', { content_type: 'video/mp4; charset=utf-8' }],
  ['no md5', { content_md5: undefined }], ['hex md5', { content_md5: 'a'.repeat(32) }], ['short md5', { content_md5: 'AAAA' }], ['noncanonical md5', { content_md5: 'AAAAAAAAAAAAAAAAAAAAAB==' }], ['url-safe md5', { content_md5: '_'.repeat(22) + '==' }],
  ['zero duration', { duration_seconds: 0 }], ['negative duration', { duration_seconds: -1 }], ['over default duration', { duration_seconds: 31 }], ['infinite duration', { duration_seconds: Infinity }], ['string duration', { duration_seconds: '4' }],
  ['zero bytes', { size_bytes: 0 }], ['negative bytes', { size_bytes: -1 }], ['fraction bytes', { size_bytes: 1.5 }], ['too many bytes', { size_bytes: 25 * 1024 * 1024 + 1 }], ['string bytes', { size_bytes: '1234' }], ['nan bytes', { size_bytes: NaN }],
  ['empty summary', { summary: '  ' }], ['long summary', { summary: 'x'.repeat(501) }], ['control summary', { summary: 'a\nb' }], ['empty detector', { detector: '' }], ['long detector', { detector: 'x'.repeat(129) }], ['control detector', { detector: '\u0085' }],
  ['invalid severity', { severity: 'urgent' }], ['confidence low', { confidence: -0.1 }], ['confidence high', { confidence: 1.1 }], ['nan confidence', { confidence: NaN }], ['string confidence', { confidence: '1' }],
  ['future', { detected_at: '2026-10-24T12:05:00.001Z' }], ['offset underflows supported year', { detected_at: '0001-01-01T00:00:00+01:00' }], ['no offset', { detected_at: '2026-10-24T12:00:00' }], ['impossible date', { detected_at: '2026-02-30T12:00:00Z' }], ['bad hour', { detected_at: '2026-10-24T24:00:00Z' }], ['bad offset', { detected_at: '2026-10-24T12:00:00+24:00' }], ['zero year', { detected_at: '0000-10-24T12:00:00Z' }],
  ['url field', { url: 'https://evil.test' }], ['key field', { object_key: 'evil' }], ['token field', { token: 'secret' }], ['unknown field', { made_up: true }],
  ['evidence URL key', { evidence: { source_url: 'a' } }], ['evidence secret key', { evidence: { access_token: 'a' } }], ['evidence URL value', { evidence: { value: 'https://evil.test' } }], ['evidence controls', { evidence: { value: '\n' } }], ['evidence oversized', { evidence: { value: 'a'.repeat(8192) } }], ['evidence nonfinite', { evidence: { n: Infinity } }], ['evidence unsupported', { evidence: new Date() }],
];
for (const [name, change] of invalidBodies) test('reject input: ' + name, async t => {
  const f = fixture(t); await rejects(f.service.prepare(f.body(change)), 400);
  assert.equal(fs.existsSync(f.env.HG_EVENT_VIDEO_INDEX_FILE), false); assert.equal(f.calls.length, 0);
});
test('reject nonobjects, cyclic/deep evidence and non-JSON producer values', async t => {
  const f = fixture(t);
  for (const input of [null, [], '', 1, undefined]) await rejects(f.service.prepare(input), 400);
  const cycle = {}; cycle.self = cycle; await rejects(f.service.prepare(f.body({ evidence: cycle })), 400);
  let deep = {}; for (let n = 0; n < 10; n++) deep = { nested: deep };
  await rejects(f.service.prepare(f.body({ evidence: deep })), 400);
  for (const evidence of [BigInt(1), () => 1, { x: undefined }, { x: Symbol('x') }, JSON.parse('{"__proto__":1}')]) await rejects(f.service.prepare(f.body({ evidence })), 400);
});
test('allowed limits, trimmed metadata, explicit IST offset, optional fields', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_MAX_DURATION_SECONDS: '120' });
  const p = await f.service.prepare(f.body({ duration_seconds: 120, confidence: 0, alert_id: 'alert_1', summary: '  Person  ', detector: ' observer ', detected_at: '2026-10-24T17:30:00+05:30', evidence: { labels: ['person'], scores: [1, null, true] } }));
  const d = (await f.service.complete(p.video_id)).event;
  assert.equal(d.summary, 'Person'); assert.equal(d.detector, 'observer'); assert.equal(d.detected_at, '2026-10-24T12:00:00.000Z'); assert.equal(d.confidence, 0); assert.equal(d.alert_id, 'alert_1');
  assert.equal((await f.service.list()).events[0].evidence, undefined);
});

test('idempotent prepare and complete, metadata conflicts, no ready overwrite URL', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  const again = await f.service.prepare(f.body()); assert.equal(again.video_id, p.video_id);
  await rejects(f.service.prepare(f.body({ summary: 'Changed observation' })), 409, 'event_video_event_conflict');
  const done = await f.service.complete(p.video_id);
  assert.deepEqual(await f.service.complete(p.video_id), done); assert.equal(f.calls.length, 1);
  const ready = await f.service.prepare(f.body()); assert.equal(ready.video_id, p.video_id); assert.equal(ready.upload, null); assert.equal(ready.already_ready, true);
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries.length, 1);
});
test('cross-instance concurrent quota reservation and duplicate races have no lost updates', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: String(1234 * 3) });
  const services = Array.from({ length: 12 }, () => new EventVideos(f.options));
  const results = await Promise.allSettled(services.map((s, i) => s.prepare(f.body({ event_id: 'race-' + i }))));
  assert.equal(results.filter(r => r.status === 'fulfilled').length, 3);
  for (const r of results.filter(r => r.status === 'rejected')) assert.equal(r.reason.code, 'event_video_storage_full');
  const catalog = JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)); assert.equal(catalog.entries.length, 3);
  const first = results.find(r => r.status === 'fulfilled').value;
  const eventId = catalog.entries.find(e => e.id === first.video_id).metadata.event_id;
  const duplicates = await Promise.all(services.map(s => s.prepare(f.body({ event_id: eventId }))));
  assert.equal(new Set(duplicates.map(x => x.video_id)).size, 1);
  const completed = await Promise.all(services.map(s => s.complete(first.video_id)));
  assert.equal(completed.length, 12); assert.equal(f.calls.length, 1);
});
test('expired reservations never free quota, re-sign, or silently become ready', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '1234' });
  const p = await f.service.prepare(f.body()); f.advance(3600001);
  await rejects(f.service.prepare(f.body()), 409, 'event_video_reservation_expired');
  await rejects(f.service.complete(p.video_id), 409, 'event_video_reservation_expired');
  await rejects(f.service.prepare(f.body({ event_id: 'other' })), 507, 'event_video_storage_full');
  assert.equal(f.calls.length, 0); assert.equal((await f.service.list()).total, 0);
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries[0].metadata.size_bytes, 1234);
});
test('pending PUT re-signing bounded by one-hour reservation and short TTL', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  assert.equal(p.upload.expires_in, 300);
  f.advance(3590000); const again = await f.service.prepare(f.body()); assert.equal(again.upload.expires_in, 10);
  assert.equal(new URL(again.upload.url).searchParams.get('X-Amz-Expires'), '10');
  await f.service.complete(p.video_id); assert.equal(f.calls.length, 1);
});

const badHeads = [
  ['not found', { status: 404 }, 503], ['redirect', { status: 302 }, 503], ['R2 outage', { status: 503 }, 503],
  ['wrong size', { headers: { 'content-length': '1233' } }, 409], ['zero size', { headers: { 'content-length': '0' } }, 409], ['bad size', { headers: { 'content-length': '1234abc' } }, 409], ['leading zeros', { headers: { 'content-length': '01234' } }, 409], ['absent size', { headers: { 'content-length': undefined } }, 409],
  ['wrong mime', { headers: { 'content-type': 'application/octet-stream' } }, 409], ['absent mime', { headers: { 'content-type': undefined } }, 409], ['mime params', { headers: { 'content-type': 'video/mp4; x=y' } }, 409],
  ['absent etag', { headers: { etag: undefined } }, 409], ['unquoted etag', { headers: { etag: FAKE_ETAG.slice(1, -1) } }, 409], ['wrong bytes etag', { headers: { etag: '"' + '0'.repeat(32) + '"' } }, 409], ['uppercase etag', { headers: { etag: FAKE_ETAG.toUpperCase() } }, 409], ['multipart etag', { headers: { etag: FAKE_ETAG.slice(0, -1) + '-2"' } }, 409],
];
for (const [name, change, status] of badHeads) test('FAKE R2 HEAD reject: ' + name, async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  f.service.transport = async () => ({ status: change.status ?? 200, headers: { 'content-length': '1234', 'content-type': 'video/mp4', etag: FAKE_ETAG, ...change.headers } });
  await rejects(f.service.complete(p.video_id), status);
  assert.equal((await f.service.latestNotification()).event, null); await rejects(f.service.detail(p.video_id), 404);
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries[0].status, 'pending');
});
test('transport failure is sanitized, retries HEAD and only then publishes ready', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  f.service.transport = async () => { throw new Error('FAKESECRET FAKEACCESS https://sensitive.test'); };
  await assert.rejects(f.service.complete(p.video_id), e => e.code === 'event_video_r2_unavailable' && !e.message.includes('FAKE'));
  assert.equal((await f.service.list()).total, 0);
  f.service.transport = async (_, opts) => { assert.equal(opts.redirect, 'error'); assert.equal(opts.timeout, 8000); assert.equal(opts.maxBodyBytes, 0); return { status: 200, headers: new Headers({ 'content-type': 'video/mp4', 'content-length': '1234', etag: FAKE_ETAG }) }; };
  assert.equal((await f.service.complete(p.video_id)).ok, true);
});

test('IST date boundaries, defaults, inclusive ranges, sorting and pagination', async t => {
  const f = fixture(t); f.advance(86400000);
  const dates = ['2026-10-23T18:29:59.999Z', '2026-10-23T18:30:00Z', '2026-10-24T18:29:59.999Z', '2026-10-24T18:30:00Z'];
  for (let n = 0; n < dates.length; n++) { const p = await f.service.prepare(f.body({ event_id: 'boundary-' + n, detected_at: dates[n] })); await f.service.complete(p.video_id); }
  const today = await f.service.list(); assert.equal(today.from, '2026-10-25'); assert.equal(today.total, 1); assert.equal(today.events[0].event_id, 'boundary-3');
  const params = new URLSearchParams({ from: '2026-10-24', to: '2026-10-24', page_size: '1' });
  const first = await f.service.list(params); assert.equal(first.total, 2); assert.equal(first.total_pages, 2); assert.equal(first.events[0].event_id, 'boundary-2');
  params.set('page', '2'); assert.equal((await f.service.list(params)).events[0].event_id, 'boundary-1');
  params.set('page', '3'); assert.equal((await f.service.list(params)).events.length, 0);
  const all = await f.service.list(new URLSearchParams('from=2026-10-23&to=2026-10-25')); assert.equal(all.total, 4); assert.equal(all.page_size, 12); assert.equal(all.time_zone, 'Asia/Kolkata');
  assert.equal((await f.service.latestNotification()).event.event_id, 'boundary-3');
});
for (const query of ['x=1','token=secret','from=2026-10-24&from=2026-10-24','page=1&page=1','page=0','page=-1','page=1.2','page=01','page=9007199254740992','page_size=0','page_size=51','page_size=1e1','from=2026-02-30','from=2026-13-01','from=2026-1-01','from=0000-01-01','from=2026-10-25&to=2026-10-24','from=2025-01-01&to=2026-01-02','from=2026-10-24T00%3A00%3A00Z','to=']) test('reject list query: ' + query, async t => {
  const f = fixture(t); await rejects(f.service.list(new URLSearchParams(query)), 400);
});
test('strict valid leap date and maximum inclusive 366-day range', async t => {
  const f = fixture(t);
  assert.equal((await f.service.list(new URLSearchParams('from=2024-01-01&to=2024-12-31'))).total, 0);
  assert.equal((await f.service.list(new URLSearchParams('from=2024-02-29&to=2024-02-29'))).ok, true);
  await rejects(f.service.list(new URLSearchParams('from=2025-02-29')), 400);
  await rejects(f.service.list({}), 400);
});
test('ready clips remain visible after nominal retention; old pending still charged', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '2468', HG_EVENT_VIDEO_RETENTION_DAYS: '7' });
  const p = await f.service.prepare(f.body()); await f.service.complete(p.video_id);
  await f.service.prepare(f.body({ event_id: 'orphan' })); f.advance(30 * 86400000);
  assert.equal((await f.service.detail(p.video_id)).event.id, p.video_id);
  assert.equal((await f.service.list(new URLSearchParams('from=2026-10-24&to=2026-10-24'))).total, 1);
  assert.equal((await f.service.latestNotification()).event.id, p.video_id);
  assert.equal((await f.service.playback(p.video_id)).expires_in, 120);
  await rejects(f.service.prepare(f.body({ event_id: 'new' })), 507);
});
test('unknown and pending IDs never readable/playable; notifications empty', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  assert.deepEqual(await f.service.latestNotification(), { ok: true, event: null });
  for (const id of ['', '../x', 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa', p.video_id]) {
    await rejects(f.service.detail(id), 404); await rejects(f.service.playback(id), 404);
  }
  await rejects(f.service.complete('../x'), 404);
});

for (const [key, value] of [
  ['HG_R2_ACCOUNT_ID', undefined], ['HG_R2_ACCOUNT_ID', 'A'.repeat(32)], ['HG_R2_ACCOUNT_ID', 'evil.test'], ['HG_R2_BUCKET', 'bad/bucket'], ['HG_R2_BUCKET', '1.2.3.4'], ['HG_R2_ACCESS_KEY_ID', undefined], ['HG_R2_SECRET_ACCESS_KEY', undefined], ['HG_R2_SECRET_ACCESS_KEY', 'x\ny'],
  ['HG_EVENT_VIDEO_INDEX_FILE', undefined], ['HG_EVENT_VIDEO_INDEX_FILE', 'relative.json'], ['HG_EVENT_VIDEO_INDEX_FILE', path.join(__dirname, 'public', 'private.json')],
  ['HG_EVENT_VIDEO_MAX_DURATION_SECONDS', '121'], ['HG_EVENT_VIDEO_MAX_BYTES', String(25 * 1024 * 1024 + 1)], ['HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES', String(8 * 1024 ** 3 + 1)], ['HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES', '0'], ['HG_EVENT_VIDEO_MAX_DURATION_SECONDS', '1.5'], ['HG_EVENT_VIDEO_MAX_BYTES', 'Infinity']
]) test('unconfigured/invalid config fails closed: ' + key + '=' + value, async t => {
  const f = fixture(t); f.env[key] = value;
  for (const invoke of [() => f.service.prepare(f.body()), () => f.service.list(), () => f.service.latestNotification(), () => f.service.detail('id'), () => f.service.playback('id'), () => f.service.complete('id')]) await rejects(invoke(), 503, 'event_video_storage_unconfigured');
});
test('bad origin, unsafe/missing directory and symlinked directory fail closed', async t => {
  const f = fixture(t);
  for (const publicOrigin of [undefined, 'http://example.test', 'https://user:pass@example.test', 'https://example.test/path', 'https://example.test/?token=secret']) await rejects(new EventVideos({ ...f.options, publicOrigin }).list(), 503);
  f.env.HG_EVENT_VIDEO_INDEX_FILE = path.join(f.dir, 'absent', 'index'); await rejects(f.service.list(), 503);
  fs.symlinkSync(f.dir, path.join(f.dir, 'link')); f.env.HG_EVENT_VIDEO_INDEX_FILE = path.join(f.dir, 'link', 'index'); await rejects(f.service.list(), 503);
  f.env.HG_EVENT_VIDEO_INDEX_FILE = path.join(f.dir, 'index'); fs.chmodSync(f.dir, 0o777); await rejects(f.service.list(), 503); fs.chmodSync(f.dir, 0o700);
});
test('corrupt catalog is never reset and all methods fail closed with sanitized errors', async t => {
  const f = fixture(t); fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, '{FAKESECRET', { mode: 0o600 });
  for (const invoke of [() => f.service.list(), () => f.service.latestNotification(), () => f.service.prepare(f.body()), () => f.service.complete('aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa'), () => f.service.detail('id'), () => f.service.playback('id')]) await rejects(invoke(), 503, 'event_video_catalog_unavailable');
  assert.equal(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8'), '{FAKESECRET');
});
test('unsafe catalog modes, symlink, hardlink and oversized file rejected', async t => {
  const f = fixture(t); await f.service.prepare(f.body());
  fs.chmodSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 0o644); await rejects(f.service.list(), 503);
  fs.chmodSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 0o600);
  const original = path.join(f.dir, 'original'); fs.renameSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, original);
  fs.symlinkSync(original, f.env.HG_EVENT_VIDEO_INDEX_FILE); await rejects(f.service.list(), 503); await rejects(f.service.prepare(f.body()), 503); fs.unlinkSync(f.env.HG_EVENT_VIDEO_INDEX_FILE);
  fs.linkSync(original, f.env.HG_EVENT_VIDEO_INDEX_FILE); await rejects(f.service.list(), 503); fs.unlinkSync(f.env.HG_EVENT_VIDEO_INDEX_FILE);
  fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, Buffer.alloc(16 * 1024 * 1024 + 1), { mode: 0o600 }); await rejects(f.service.list(), 503);
});
test('structurally corrupt catalog, synthetic records, duplicate events/IDs rejected', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  const original = JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE));
  for (const mutate of [
    d => { d.version = 2; }, d => { d.extra = true; }, d => { d.entries = null; }, d => { d.entries[0].object_key = '../evil'; }, d => { d.entries[0].metadata.provenance = 'synthetic'; }, d => { d.entries[0].metadata.content_md5 = 'bad'; }, d => { d.entries[0].status = 'made-up'; }, d => { d.entries[0].expires_at++; }, d => { d.entries[0].ready_at = 123; }, d => { d.entries.push(structuredClone(d.entries[0])); }, d => { const duplicate = structuredClone(d.entries[0]); duplicate.id = crypto.randomUUID(); duplicate.object_key = `event-videos/${duplicate.id}.mp4`; d.entries.push(duplicate); },
  ]) {
    const data = structuredClone(original); mutate(data); fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, JSON.stringify(data));
    await rejects(f.service.list(), 503); await rejects(f.service.complete(p.video_id), 503);
  }
});
test('atomic rename failure preserves charged pending state and leaves no temp/lock', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  const original = fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8');
  const rename = fs.renameSync;
  try {
    fs.renameSync = () => { throw new Error('FAKESECRET failure'); };
    await rejects(f.service.complete(p.video_id), 503, 'event_video_catalog_unavailable');
  } finally { fs.renameSync = rename; }
  assert.equal(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, 'utf8'), original);
  assert.deepEqual(fs.readdirSync(f.dir).sort(), ['index.json', 'ingest']);
  await f.service.complete(p.video_id); assert.equal((await f.service.list()).total, 1);
});

test('independent Python hashlib/hmac golden SigV4 PUT vector and canonical header binding', () => {
  // Independently calculated using Python stdlib (not this implementation).
  const args = { accountId: 'a'.repeat(32), bucket: 'fake-test-bucket', accessKeyId: 'FAKEACCESS', secretAccessKey: 'FAKESECRET', key: "a b/!'()*+.mp4", method: 'PUT', expires: 300, now: Date.parse('2026-10-24T12:00:00Z'), headers: { 'Content-Type': 'video/mp4', 'Content-Length': '1234', 'Content-MD5': FAKE_MD5, 'If-None-Match': '*' } };
  const url = new URL(signR2(args));
  assert.equal(url.searchParams.get('X-Amz-Signature'), '3e6e1e0becd51ed48686f5aeb31cbdb048669a6da9cecc7f80502f022b16a858');
  assert.equal(url.pathname, '/fake-test-bucket/a%20b/%21%27%28%29%2A%2B.mp4');
  assert.equal(url.searchParams.get('X-Amz-SignedHeaders'), 'content-length;content-md5;content-type;host;if-none-match');
  assert.equal(signR2(args), signR2({ ...args, headers: { 'If-None-Match': '*', 'Content-MD5': FAKE_MD5, 'Content-Length': '1234', 'Content-Type': 'video/mp4' } }));
  assert.notEqual(signR2(args), signR2({ ...args, headers: { ...args.headers, 'Content-Length': '1235' } }));
  assert.notEqual(signR2(args), signR2({ ...args, headers: { ...args.headers, 'Content-MD5': crypto.createHash('md5').update('different bytes').digest('base64') } }));
  for (const change of [{ accountId: 'evil.test' }, { expires: 301 }, { expires: 0 }, { method: 'POST' }, { key: '../escape' }, { headers: { ...args.headers, 'If-None-Match': undefined } }]) assert.throws(() => signR2({ ...args, ...change }), EventVideoError);
});
test('separate Node processes serialize reservations; disk catalog survives actual process restart', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES: '3702' });
  const { execFile } = require('node:child_process');
  const execute = (source, args) => new Promise((resolve, reject) => execFile(process.execPath, ['-e', source, JSON.stringify(args)], { env: { ...process.env, NODE_OPTIONS: '' }, maxBuffer: 1024 * 1024, timeout: 20000 }, (err, stdout) => err ? reject(err) : resolve(JSON.parse(stdout))));
  const source = `const {EventVideos}=require(${JSON.stringify(path.join(__dirname, 'event_videos.js'))}); const a=JSON.parse(process.argv[1]); const s=new EventVideos({env:a.env,publicOrigin:'https://portal.example.test',now:()=>Date.parse('2026-10-24T12:00:00Z')}); s.prepare(a.body).then(p=>console.log(JSON.stringify({id:p.video_id})),e=>console.log(JSON.stringify({code:e.code})));`;
  const results = await Promise.all(Array.from({ length: 5 }, (_, n) => execute(source, { env: f.env, body: f.body({ event_id: 'process-' + n }) })));
  assert.equal(results.filter(r => r.id).length, 3); assert.equal(results.filter(r => r.code === 'event_video_storage_full').length, 2);
  const id = results.find(r => r.id).id; await f.service.complete(id);
  const reader = `const {EventVideos}=require(${JSON.stringify(path.join(__dirname, 'event_videos.js'))});const a=JSON.parse(process.argv[1]); const s=new EventVideos({env:a.env,publicOrigin:'https://portal.example.test',now:()=>Date.parse('2026-10-24T12:00:00Z')}); Promise.all([s.detail(a.id),s.list(),s.latestNotification()]).then(r=>console.log(JSON.stringify(r)));`;
  const readback = await execute(reader, { env: f.env, id });
  assert.equal(readback[0].event.id, id); assert.equal(readback[1].total, 1); assert.equal(readback[2].event.id, id);
});
test('fsync ordering makes reservation durable before PUT is returned', async t => {
  const f = fixture(t), original = fs.fsyncSync;
  const syncs = [];
  try {
    fs.fsyncSync = fd => { syncs.push(fs.fstatSync(fd).isDirectory() ? 'directory' : 'file'); return original(fd); };
    const p = await f.service.prepare(f.body());
    assert.deepEqual(syncs, ['file', 'directory']);
    assert.ok(p.upload.url); assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries[0].id, p.video_id);
  } finally { fs.fsyncSync = original; }
});
test('catalog entry bound prevents new upload grants without discarding existing accounting', async t => {
  const f = fixture(t); await f.service.prepare(f.body({ size_bytes: 1 }));
  const data = JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)), seed = data.entries[0];
  data.entries = Array.from({ length: 10000 }, (_, n) => { const id = crypto.randomUUID(); return { ...seed, id, object_key: `event-videos/${id}.mp4`, metadata: { ...seed.metadata, event_id: 'entry-' + n } }; });
  fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, JSON.stringify(data));
  await rejects(f.service.prepare(f.body({ event_id: 'overflow', size_bytes: 1 })), 507, 'event_video_storage_full');
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries.length, 10000);
  data.entries.push(seed); fs.writeFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE, JSON.stringify(data)); await rejects(f.service.list(), 503);
});
test('native HTTPS request path uses HEAD only, bounds headers and rejects redirects/errors (FAKE network)', async t => {
  const f = fixture(t), https = require('node:https'), { EventEmitter } = require('node:events');
  const original = https.request, observed = [];
  let responseStatus = 302, emitError = false;
  https.request = (url, options, callback) => {
    observed.push({ url, options });
    const req = new EventEmitter(); req.destroy = err => { if (err) req.emit('error', err); req.emit('close'); };
    req.end = () => process.nextTick(() => {
      if (emitError) { req.emit('error', new Error('FAKESECRET')); req.emit('close'); return; }
      callback({ statusCode: responseStatus, headers: { 'content-type': 'video/mp4', 'content-length': '1234', etag: FAKE_ETAG }, destroy() {} }); req.emit('close');
    });
    return req;
  };
  try {
    const s = new EventVideos({ env: f.env, publicOrigin: f.options.publicOrigin, now: f.options.now });
    const p = await s.prepare(f.body());
    await rejects(s.complete(p.video_id), 503, 'event_video_r2_unavailable'); assert.equal(observed.length, 1);
    emitError = true; await rejects(s.complete(p.video_id), 503, 'event_video_r2_unavailable');
    emitError = false; responseStatus = 200; await s.complete(p.video_id);
    for (const o of observed) { assert.equal(o.options.method, 'HEAD'); assert.equal(o.options.timeout, 8000); assert.equal(o.options.maxHeaderSize, 16384); }
    assert.equal((await s.list()).total, 1);
  } finally { https.request = original; }
});
test('catalog charge remains pending while FAKE HEAD is inflight', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  let release, started;
  const gate = new Promise(resolve => { release = resolve; }), announced = new Promise(resolve => { started = resolve; });
  f.service.transport = async () => { started(); await gate; return { status: 200, headers: { 'content-type': 'video/mp4', 'content-length': '1234', etag: FAKE_ETAG } }; };
  const complete = f.service.complete(p.video_id); await announced;
  assert.equal((await new EventVideos(f.options).list()).total, 0);
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries[0].status, 'pending');
  release(); assert.equal((await complete).ok, true);
});

test('stalled FAKE transport has an enforced eight-second deadline and preserves reservation', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  t.mock.timers.enable({ apis: ['setTimeout'] });
  f.service.transport = async () => new Promise(() => {});
  const completion = f.service.complete(p.video_id);
  const rejection = rejects(completion, 503, 'event_video_r2_unavailable');
  t.mock.timers.tick(8000); await rejection;
  assert.equal(JSON.parse(fs.readFileSync(f.env.HG_EVENT_VIDEO_INDEX_FILE)).entries[0].status, 'pending');
  assert.equal(fs.existsSync(f.env.HG_EVENT_VIDEO_INDEX_FILE + '.lock'), false);
});
test('stale/symlink lock is never automatically reclaimed or followed', async t => {
  const f = fixture(t), lock = f.env.HG_EVENT_VIDEO_INDEX_FILE + '.lock';
  fs.symlinkSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, lock);
  const original = Date.now; let calls = 0;
  try {
    Date.now = () => calls++ === 0 ? 0 : 8001;
    await rejects(f.service.prepare(f.body()), 503, 'event_video_catalog_busy');
  } finally { Date.now = original; }
  assert.equal(fs.lstatSync(lock).isSymbolicLink(), true);
  assert.equal(fs.readFileSync(f.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 'utf8'), 'FAKE-producer-token\n');
});
test('HEAD finishing after reservation expiry cannot publish a clip', async t => {
  const f = fixture(t), p = await f.service.prepare(f.body());
  f.service.transport = async () => { f.advance(3600001); return { status: 200, headers: { 'content-type': 'video/mp4', 'content-length': '1234', etag: FAKE_ETAG } }; };
  await rejects(f.service.complete(p.video_id), 409, 'event_video_reservation_expired');
  assert.equal((await f.service.latestNotification()).event, null);
});
test('lower per-clip limits are enforced at prepare and actual HEAD completion', async t => {
  const f = fixture(t, { HG_EVENT_VIDEO_MAX_BYTES: '1233', HG_EVENT_VIDEO_MAX_DURATION_SECONDS: '3' });
  await rejects(f.service.prepare(f.body()), 400);
  await rejects(f.service.prepare(f.body({ size_bytes: 1233 })), 400);
  f.env.HG_EVENT_VIDEO_MAX_BYTES = '1234'; f.env.HG_EVENT_VIDEO_MAX_DURATION_SECONDS = '4';
  const p = await f.service.prepare(f.body()); f.env.HG_EVENT_VIDEO_MAX_BYTES = '1233';
  await rejects(f.service.complete(p.video_id), 409, 'event_video_object_mismatch');
});

test('all signed URLs pin HTTPS R2; endpoint overrides never affect transport', async t => {
  const f = fixture(t, { HG_R2_ENDPOINT: 'http://127.0.0.1:1', AWS_ENDPOINT_URL: 'http://169.254.169.254/' });
  const p = await f.service.prepare(f.body()); await f.service.complete(p.video_id);
  const play = await f.service.playback(p.video_id);
  for (const value of [p.upload.url, f.calls[0].url, play.url]) {
    const url = new URL(value); assert.equal(url.protocol, 'https:'); assert.equal(url.host, 'a'.repeat(32) + '.r2.cloudflarestorage.com'); assert.ok(!value.includes('FAKESECRET')); assert.equal(url.searchParams.get('X-Amz-Credential'), 'FAKEACCESS/20261024/auto/s3/aws4_request');
  }
});
