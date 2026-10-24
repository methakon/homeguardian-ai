'use strict';
// Hermetic HTTP authorization fixtures; no cloud upload or physical detection claim.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { createGateway } = require('./gateway');
const { OidcConfig } = require('./oauth');

(async () => {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'hg-event-routes-'));
  const id = '00000000-0000-4000-8000-000000000001';
  const producer = 'a'.repeat(64), notification = 'b'.repeat(64);
  const notificationFile = path.join(dir, 'notification.key');
  fs.writeFileSync(notificationFile, notification, { mode: 0o600 });
  const env = { HG_OUTDOOR_REVIEW_ACCESS: 'google_viewers', HG_R2_ACCOUNT_ID: 'c'.repeat(32), HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE: notificationFile };
  let checks = 0, calls = 0;
  const check = (v) => { assert.ok(v); checks++; };
  const service = {
    authenticateProducer(req) { return req.headers.authorization === 'Bearer ' + producer; },
    async list(params) { calls++; return { ok: true, events: [], page: Number(params.get('page') || 1), total: 0 }; },
    async detail(value) { calls++; assert.equal(value, id); return { ok: true, event: { id } }; },
    async playback(value) { calls++; assert.equal(value, id); return { ok: true, url: 'https://' + 'c'.repeat(32) + '.r2.cloudflarestorage.com/private/fixture?X-Amz-Signature=fixture' }; },
    async prepare(body) { calls++; assert.equal(body.event_id, 'fixture-event'); return { ok: true, video_id: id }; },
    async complete(value) { calls++; assert.equal(value, id); return { ok: true, event: { id } }; },
    async latestNotification() { calls++; return { ok: true, event: { id, summary: 'Synthetic HTTP contract fixture only' } }; },
  };
  const g = createGateway({ env, eventVideos: service,
    config: new OidcConfig({ HG_PUBLIC_ORIGIN: 'https://portal.example.test', HG_GOOGLE_CLIENT_ID: 'fixture-client' }),
    logger: { info() {}, warn() {} }, mcp: { async callTool() { throw Error('video routes must not call native MCP'); } },
  });
  await g.listen();
  const viewer = g.sessions.create({ sub: '123456789012345678901', role: 'VIEWER', issuer: 'https://accounts.google.com' }).token;
  async function req(route, { method = 'GET', auth = viewer, origin = 'https://portal.example.test', body, raw, headers: extra = {} } = {}) {
    return new Promise((resolve, reject) => {
      const payload = raw === undefined ? body === undefined ? null : JSON.stringify(body) : raw;
      const headers = { Host: 'portal.example.test', ...extra };
      if (auth) headers.Authorization = 'Bearer ' + auth;
      if (origin) headers.Origin = origin;
      if (payload !== null) { headers['Content-Type'] = 'application/json'; headers['Content-Length'] = Buffer.byteLength(payload); }
      const r = http.request({ hostname: '127.0.0.1', port: g.server.address().port, path: route, method, headers }, res => {
        const chunks = []; res.on('data', b => chunks.push(b)); res.on('end', () => {
          const text = Buffer.concat(chunks).toString(); let json; try { json = JSON.parse(text); } catch (_) {}
          check(!res.headers['set-cookie']); check(res.headers['cache-control']?.includes('no-store'));
          resolve({ status: res.statusCode, json, text, headers: res.headers });
        });
      });
      r.on('error', reject); r.end(payload);
    });
  }
  try {
    check((await req('/api/event-videos')).status === 200);
    for (const route of ['/api/event-videos', '/api/event-videos/' + id, '/api/event-videos/' + id + '/playback']) {
      check((await req(route)).status === 200);
      check((await req(route, { auth: null })).status === 401);
      check((await req(route, { auth: producer })).status === 401);
      check((await req(route, { origin: 'https://evil.example' })).status === 403);
      check((await req(route, { origin: 'http://portal.example.test' })).status === 403);
      check((await req(route, { method: 'POST', body: {} })).status === 405);
    }
    env.HG_OUTDOOR_REVIEW_ACCESS = '';
    const deniedBefore = calls;
    check((await req('/api/event-videos')).status === 403);
    check(calls === deniedBefore);
    env.HG_OUTDOOR_REVIEW_ACCESS = 'google_viewers';
    check((await req('/api/event-videos/not-a-uuid')).status === 400);
    check((await req('/api/event-videos/' + id + '?token=forbidden')).status === 400);
    check((await req('/api/event-videos/' + id + '?url=https://evil.example')).status === 400);
    check((await req('/api/event-videos/' + id + '/playback?x=1')).status === 400);
    for (const route of ['/internal/event-videos/uploads', '/internal/event-videos/uploads/' + id + '/complete']) {
      check((await req(route, { method: 'POST', body: { event_id: 'fixture-event' }, origin: null })).status === 401);
      check((await req(route, { method: 'POST', auth: notification, body: {}, origin: null })).status === 401);
      check((await req(route, { method: 'POST', auth: producer, body: {}, origin: 'https://portal.example.test' })).status === 403);
      check((await req(route, { auth: producer, origin: null })).status === 405);
    }
    check((await req('/internal/event-videos/uploads', { method: 'POST', auth: producer, origin: null, body: { event_id: 'fixture-event' } })).status === 201);
    check((await req('/internal/event-videos/uploads/' + id + '/complete', { method: 'POST', auth: producer, origin: null, body: {} })).status === 200);
    check((await req('/internal/event-videos/uploads/' + id + '/complete', { method: 'POST', auth: producer, origin: null, body: { size: 4 } })).status === 400);
    check((await req('/internal/event-videos/uploads/' + id + '/complete', { method: 'POST', auth: producer, origin: null, raw: '[]' })).status === 400);
    check((await req('/internal/event-videos/uploads', { method: 'POST', auth: producer, origin: null, raw: 'null' })).status === 400);
    check((await req('/internal/event-videos/uploads?target=evil', { method: 'POST', auth: producer, origin: null, body: {} })).status === 400);
    check((await req('/internal/event-videos/notification', { auth: viewer, origin: null })).status === 401);
    check((await req('/internal/event-videos/notification', { auth: producer, origin: null })).status === 401);
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 200);
    check((await req('/internal/event-videos/notification', { auth: notification })).status === 403);
    fs.chmodSync(notificationFile, 0o644);
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    fs.chmodSync(notificationFile, 0o600);
    fs.writeFileSync(notificationFile, 'd'.repeat(64));
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    fs.writeFileSync(notificationFile, notification);
    for (const mode of [0o400, 0o700, 0o660]) {
      fs.chmodSync(notificationFile, mode);
      check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    }
    fs.chmodSync(notificationFile, 0o600);
    const alternate = path.join(dir, 'alternate.key');
    fs.linkSync(notificationFile, alternate);
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    fs.unlinkSync(alternate);
    fs.renameSync(notificationFile, alternate); fs.symlinkSync(alternate, notificationFile);
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    fs.unlinkSync(notificationFile);
    require('node:child_process').execFileSync('mkfifo', [notificationFile]);
    check((await req('/internal/event-videos/notification', { auth: notification, origin: null })).status === 401);
    fs.unlinkSync(notificationFile); fs.renameSync(alternate, notificationFile);
    const html = await req('/');
    check(html.text.includes('/assets/event-videos.js'));
    check(html.headers['content-security-policy'].includes('media-src \'self\' blob: https://' + 'c'.repeat(32) + '.r2.cloudflarestorage.com'));
    check(!html.headers['content-security-policy'].includes('*.r2.cloudflarestorage.com'));
    // Backend error text must never reveal a signed URL or credentials.
    service.list = async () => { throw Object.assign(Error('private-secret-and-signed-url'), { status: 500, code: 'private-secret-and-signed-url' }); };
    const unavailable = await req('/api/event-videos');
    check(unavailable.status === 503 && unavailable.json.error === 'event_videos_unavailable');
    check(!unavailable.text.includes('private-secret'));
    service.prepare = async () => { throw Object.assign(Error('internal quota data'), { status: 507, code: 'event_video_storage_full' }); };
    const full = await req('/internal/event-videos/uploads', { method: 'POST', auth: producer, origin: null, body: {} });
    check(full.status === 507 && full.json.error === 'event_video_storage_full');
    check(!full.text.includes('internal quota'));
    g.sessions.revoke(viewer);
    check((await req('/api/event-videos')).status === 401);
    console.log(`PASS event-video gateway authorization: ${checks} assertions`);
  } finally { await g.close(); fs.rmSync(dir, { recursive: true, force: true }); }
})().catch(err => { console.error(err); process.exitCode = 1; });
