'use strict';
// Hermetic integration: actual gateway/backend/served UI, a loopback FAKE R2,
// generated fixture bytes and synthetic principals. No real camera/cloud claim.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const crypto = require('node:crypto');
const vm = require('node:vm');
const { EventVideos } = require('./event_videos');
const { createGateway } = require('./gateway');
const { OidcConfig } = require('./oauth');

function wire(port, route, { method = 'GET', headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const r = http.request({ hostname: '127.0.0.1', port, path: route, method, headers }, res => {
      const chunks = []; res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const bytes = Buffer.concat(chunks); let json;
        try { json = JSON.parse(bytes.toString()); } catch (_) {}
        resolve({ status: res.statusCode, headers: res.headers, bytes, json });
      });
    });
    r.on('error', reject); r.end(body);
  });
}
function dom(fetch, search = '') {
  const observers = [], listeners = {};
  let document;
  class Element {
    constructor(tag) { this.tagName = tag.toUpperCase(); this.children = []; this.attrs = {}; this.listeners = {}; this.hidden = false; this.disabled = false; this._text = ''; }
    set textContent(s) { this.children = []; this._text = String(s); }
    get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
    appendChild(n) { this.children.push(n); return n; }
    setAttribute(k, v) { this.attrs[k] = String(v); if (this === document.documentElement) observers.forEach(fn => fn()); }
    getAttribute(k) { return this.attrs[k] ?? null; }
    removeAttribute(k) { delete this.attrs[k]; if (k === 'src') this.src = ''; }
    addEventListener(k, fn) { this.listeners[k] = fn; }
    click() { if (!this.disabled) this.listeners.click?.(); }
    pause() { this.paused = true; }
    load() { this.loads = (this.loads || 0) + 1; }
    scrollIntoView() {}
  }
  const walk = e => [e, ...e.children.flatMap(walk)];
  document = { readyState: 'complete', createElement: t => new Element(t), addEventListener() {} };
  document.documentElement = new Element('html');
  document.head = document.documentElement.appendChild(new Element('head'));
  document.body = document.documentElement.appendChild(new Element('body'));
  document.getElementById = id => walk(document.documentElement).find(e => e.id === id) || null;
  const window = { document, Intl, Date, URL, location: new URL('https://homeguardian.berhampore.in/' + search), fetch,
    MutationObserver: class { constructor(fn) { this.fn = fn; } observe() { observers.push(this.fn); } },
    addEventListener(k, fn) { listeners[k] = fn; },
  };
  return { document, window, walk, grant() { document.documentElement.setAttribute('data-hg-auth', 'viewer'); document.documentElement.setAttribute('data-hg-household', 'granted'); },
    get: id => document.getElementById('hg-ev-' + id) };
}
async function until(condition) {
  for (let n = 0; n < 150; n++) { if (condition()) return; await new Promise(r => setTimeout(r, 5)); }
  assert.fail('served UI did not reach expected state');
}

test('producer upload/completion → persisted catalog → Google-viewer scope → actual served list/detail/player → notification → logout', async t => {
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'hg-video-integration-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const objects = new Map(), transfers = [];
  const s3 = http.createServer((req, res) => {
    const key = new URL(req.url, 'https://' + req.headers.host).pathname;
    const chunks = [];
    req.on('data', b => chunks.push(b)); req.on('end', () => {
      transfers.push(req.method);
      if (req.method === 'PUT') {
        if (objects.has(key)) { res.writeHead(412); res.end(); return; }
        const bytes = Buffer.concat(chunks);
        assert.equal(req.headers['if-none-match'], '*');
        assert.equal(req.headers['content-type'], 'video/mp4');
        assert.equal(Number(req.headers['content-length']), bytes.length);
        assert.equal(req.headers['content-md5'], crypto.createHash('md5').update(bytes).digest('base64'));
        objects.set(key, bytes); res.writeHead(200); res.end(); return;
      }
      const bytes = objects.get(key);
      if (!bytes) { res.writeHead(404); res.end(); return; }
      res.writeHead(200, { 'Content-Type': 'video/mp4', 'Content-Length': bytes.length, ETag: '"' + crypto.createHash('md5').update(bytes).digest('hex') + '"' });
      res.end(req.method === 'HEAD' ? undefined : bytes);
    });
  });
  await new Promise(resolve => s3.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => s3.close(resolve)));
  const env = { HG_OUTDOOR_REVIEW_ACCESS: 'google_viewers', HG_R2_ACCOUNT_ID: 'a'.repeat(32), HG_R2_BUCKET: 'fake-fixture-bucket',
    HG_R2_ACCESS_KEY_ID: 'FAKE_ACCESS_KEY', HG_R2_SECRET_ACCESS_KEY: 'FAKE_SECRET_KEY',
    HG_EVENT_VIDEO_INDEX_FILE: path.join(dir, 'catalog.json'), HG_EVENT_VIDEO_INGEST_TOKEN_FILE: path.join(dir, 'producer'),
    HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE: path.join(dir, 'reader') };
  const producer = 'b'.repeat(64), reader = 'c'.repeat(64);
  fs.writeFileSync(env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, producer, { mode: 0o600 });
  fs.writeFileSync(env.HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE, reader, { mode: 0o600 });
  const publicOrigin = 'https://homeguardian.berhampore.in';
  function service() {
    return new EventVideos({ env, publicOrigin, transport: async (url, opts) => {
      // The ONLY upstream substitution is explicitly the fake S3 HEAD transport.
      const u = new URL(url);
      assert.equal(u.origin, 'https://' + 'a'.repeat(32) + '.r2.cloudflarestorage.com');
      assert.equal(opts.method, 'HEAD');
      const response = await wire(s3.address().port, u.pathname + u.search, { method: 'HEAD', headers: { Host: u.host } });
      return { status: response.status, headers: response.headers };
    } });
  }
  const g = createGateway({ env, eventVideos: service(), config: new OidcConfig({ HG_PUBLIC_ORIGIN: publicOrigin, HG_GOOGLE_CLIENT_ID: 'fixture-client' }), logger: { info() {}, warn() {} } });
  await g.listen(); t.after(() => g.close());
  let viewer = g.sessions.create({ role: 'VIEWER', sub: '123456789012345678901', issuer: 'https://accounts.google.com' }).token;
  const route = (p, token, body) => wire(g.server.address().port, p, { method: body === undefined ? 'GET' : 'POST',
    headers: { Host: 'homeguardian.berhampore.in', ...(token ? { Authorization: 'Bearer ' + token } : {}),
      ...(body === undefined ? {} : { 'Content-Type': 'application/json' }) }, body: body === undefined ? undefined : JSON.stringify(body) });
  const bytes = Buffer.from('FAKE/generated bounded clip fixture; not actual detection footage.');
  const body = { event_id: 'fixture-integration', detected_at: new Date().toISOString(), summary: 'Synthetic contract fixture: observer verdict', detector: 'fixture-only',
    severity: 'warning', provenance: 'real', detection_status: 'detected', duration_seconds: 4, size_bytes: bytes.length, content_type: 'video/mp4', content_md5: crypto.createHash('md5').update(bytes).digest('base64') };
  assert.equal((await route('/internal/event-videos/uploads', viewer, body)).status, 401);
  assert.equal((await route('/internal/event-videos/uploads', producer, { ...body, provenance: 'synthetic' })).status, 400);
  const prepared = await route('/internal/event-videos/uploads', producer, body);
  assert.equal(prepared.status, 201);
  const p = prepared.json, id = p.video_id;
  assert.equal((await route('/api/event-videos', viewer)).json.total, 0);
  assert.equal((await route(p.complete_path, producer, {})).status, 503);
  assert.equal((await route('/api/event-videos/' + id, viewer)).status, 404);
  const put = new URL(p.upload.url);
  const uploaded = await wire(s3.address().port, put.pathname + put.search, { method: 'PUT', headers: { ...p.upload.headers, Host: put.host }, body: bytes });
  assert.equal(uploaded.status, 200);
  const completed = await route(p.complete_path, producer, {});
  assert.equal(completed.status, 200);
  assert.equal(Object.hasOwn(completed.json.event, 'confidence'), false);
  assert.equal(Object.hasOwn(completed.json.event, 'alert_id'), false);
  assert.deepEqual((await service().detail(id)).event, completed.json.event); // Independent service reads actual disk.
  const notification = await route('/internal/event-videos/notification', reader);
  assert.equal(notification.status, 200);
  assert.equal(notification.json.event.portal_url, p.portal_url);
  assert.equal((await route('/internal/event-videos/notification', producer)).status, 401);
  const uiSource = await route('/assets/event-videos.js');
  assert.equal(uiSource.status, 200);
  const client = dom(async (url, opts) => {
    assert.equal(opts.credentials, 'omit');
    const r = await route(url, viewer);
    return { ok: r.status === 200, status: r.status, json: async () => r.json };
  }, '?event_video=' + id + '#event-videos');
  vm.runInNewContext(uiSource.bytes.toString(), { window: client.window });
  assert.equal(client.get('cards').children.length, 0);
  client.grant();
  await until(() => client.get('video'));
  assert.equal(client.get('cards').children.length, 1);
  assert.match(client.get('detail').textContent, /Confidence: not reported/);
  assert.match(client.get('detail').textContent, /Alert ID: not reported/);
  assert.equal(client.get('permalink').href, p.portal_url);
  client.get('play').click(); await until(() => client.get('video').src);
  const playback = new URL(client.get('video').src);
  const downloaded = await wire(s3.address().port, playback.pathname + playback.search, { headers: { Host: playback.host } });
  assert.equal(downloaded.status, 200); assert.deepEqual(downloaded.bytes, bytes);
  assert.equal(playback.searchParams.get('X-Amz-Expires'), '120');
  const v = client.get('video'); g.sessions.revoke(viewer);
  client.document.documentElement.setAttribute('data-hg-auth', 'anonymous');
  assert.equal(v.src, ''); assert.equal(v.paused, true);
  assert.equal(client.get('cards').textContent, ''); assert.equal(client.get('detail').textContent, '');
  assert.equal((await route('/api/event-videos/' + id + '/playback', viewer)).status, 401);
  assert.equal((await route('/api/event-videos/' + id + '/playback')).status, 401);
  assert.deepEqual(transfers, ['HEAD', 'PUT', 'HEAD', 'GET']);
  console.log('Hermetic producer-to-served-UI round trip verified; live R2/physical clip acceptance NOT claimed.');
});
