'use strict';
// Offline DOM fixtures only; no network, dependencies, camera or real recordings.
const assert = require('node:assert/strict');
const { test } = require('node:test');
class Node {
  constructor(tag, owner) { this.tagName = tag.toUpperCase(); this.owner = owner; this.children = []; this.attrs = {}; this.listeners = {}; this.hidden = false; this.disabled = false; this.value = ''; this._text = ''; }
  set textContent(v) { this._text = String(v); this.children = []; }
  get textContent() { return this._text + this.children.map(n => n.textContent).join(''); }
  set innerHTML(_) { throw new Error('HTML injection forbidden'); }
  appendChild(n) { this.children.push(n); n.parentNode = this; return n; }
  replaceChildren(...nodes) { this._text = ''; this.children = []; nodes.forEach(n => this.appendChild(n)); }
  setAttribute(k, v) { this.attrs[k] = String(v); if (k === 'id') this.id = String(v); if (k === 'src') this.src = String(v); if (this === this.owner.documentElement) this.owner.observers.forEach(cb => cb([{ attributeName: k }])); }
  getAttribute(k) { return Object.hasOwn(this.attrs, k) ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; if (k === 'src') this.src = ''; }
  addEventListener(k, fn) { (this.listeners[k] ||= []).push(fn); }
  dispatch(k) { for (const fn of this.listeners[k] || []) fn({ target: this, preventDefault() {} }); }
  click() { if (!this.disabled) this.dispatch('click'); }
  pause() { this.paused = true; }
  load() { this.loads = (this.loads || 0) + 1; }
  scrollIntoView() { this.scrolled = true; }
}
function fixture(search = '', readyState = 'complete') {
  const document = { observers: [], readyState, listeners: {}, createElement(tag) { return new Node(tag, this); }, addEventListener(k, fn) { (this.listeners[k] ||= []).push(fn); } };
  document.documentElement = new Node('html', document);
  document.head = document.documentElement.appendChild(new Node('head', document));
  document.body = document.documentElement.appendChild(new Node('body', document));
  document.getElementById = id => walk(document.documentElement).find(n => n.id === id) || null;
  const requests = [];
  const listeners = {};
  const window = { document, location: new URL('https://portal.example/' + search), Intl, Date, URL,
    addEventListener(k, fn) { (listeners[k] ||= []).push(fn); },
    MutationObserver: class { constructor(cb) { this.cb = cb; } observe(_, opts) { assert.equal(opts.attributes, true); assert.notEqual(opts.childList, true); document.observers.push(this.cb); } disconnect() {} },
    fetch(url, options) { return new Promise((resolve, reject) => requests.push({ url, options, resolve, reject })); }
  };
  Object.defineProperty(window, 'localStorage', { get() { throw new Error('no storage'); } });
  Object.defineProperty(window, 'sessionStorage', { get() { throw new Error('no storage'); } });
  const { buildUI } = require('./event_videos_ui');
  buildUI(window, document);
  return { window, document, requests, get: id => document.getElementById('hg-ev-' + id),
    grant() { document.documentElement.setAttribute('data-hg-auth', 'viewer'); (listeners['hg:session'] || []).forEach(fn => fn()); document.documentElement.setAttribute('data-hg-household', 'granted'); },
    revoke() { document.documentElement.setAttribute('data-hg-household', 'denied'); },
    domReady() { document.readyState = 'complete'; (document.listeners.DOMContentLoaded || []).forEach(fn => fn()); }
  };
}
function walk(node) { return [node, ...node.children.flatMap(walk)]; }
const tick = () => new Promise(resolve => setImmediate(resolve));
const ID = '12345678-1234-1234-1234-123456789abc';
function event(overrides = {}) { return { id: ID, event_id: 'original-event', alert_id: 'original-alert', detected_at: '2026-10-23T19:00:00Z', summary: 'Motion detected', severity: 'high', detector: 'person', confidence: 0.91, duration_seconds: 8, size_bytes: 512, provenance: 'real', portal_url: '/?event_video=' + ID + '#event-videos', ...overrides }; }
function reply(req, body, status = 200) { req.resolve({ ok: status >= 200 && status < 300, status, json: async () => body }); }
function listing(req, events = [], overrides = {}) { const u = new URL(req.url, 'https://portal.example'); return { ok: true, events, page: Number(u.searchParams.get('page')), page_size: 12, total: events.length, total_pages: events.length ? 1 : 0, from: u.searchParams.get('from'), to: u.searchParams.get('to'), time_zone: 'Asia/Kolkata', ...overrides }; }
test('gates on viewer AND household grant, observes late grant, initializes only once', async () => {
  const f = fixture();
  assert.equal(f.requests.length, 0);
  assert.equal(f.document.getElementById('hg-event-videos').hidden, true);
  f.document.documentElement.setAttribute('data-hg-auth', 'viewer');
  assert.equal(f.requests.length, 0);
  f.grant();
  assert.equal(f.requests.length, 1);
  assert.equal(f.document.getElementById('hg-event-videos').hidden, false);
  f.grant(); assert.equal(f.requests.length, 1);
  const u = new URL(f.requests[0].url, 'https://portal.example');
  assert.equal(u.searchParams.get('page_size'), '12');
  assert.equal(u.searchParams.get('page'), '1');
  const parts = new Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Kolkata', year: 'numeric', month: '2-digit', day: '2-digit' }).formatToParts(new Date());
  const part = kind => parts.find(p => p.type === kind).value;
  assert.equal(u.searchParams.get('from'), `${part('year')}-${part('month')}-${part('day')}`);
  assert.equal(u.searchParams.get('to'), u.searchParams.get('from'));
  assert.equal(f.get('apply').disabled, true);
  reply(f.requests[0], listing(f.requests[0])); await tick();
  assert.match(f.get('status').textContent, /No real detection/);
  assert.equal(f.get('apply').disabled, false);
});
async function loaded(events = [event()], search = '') {
  const f = fixture(search); f.grant(); reply(f.requests[0], listing(f.requests[0], events)); await tick(); return f;
}
async function selected(overrides = {}) {
  const f = await loaded();
  walk(f.get('cards')).find(n => n.tagName === 'BUTTON').click();
  reply(f.requests[1], { ok: true, event: event(overrides) }); await tick(); return f;
}
test('DOMContentLoaded and browser autoload execute actual script', async () => {
  const f = fixture('', 'loading'); assert.equal(f.document.getElementById('hg-event-videos'), null);
  f.grant(); assert.equal(f.requests.length, 1); f.domReady(); assert.equal(f.requests.length, 1);
  reply(f.requests[0], listing(f.requests[0])); await tick();
  const fs = require('node:fs'), vm = require('node:vm');
  const b = fixture();
  // A fresh document is used so the browser entry path itself must create UI.
  b.document.body.children = []; b.document.head.children = []; b.document.observers = [];
  vm.runInNewContext(fs.readFileSync(require.resolve('./event_videos_ui'), 'utf8'), { window: b.window });
  assert.ok(b.document.getElementById('hg-event-videos'));
  assert.equal(b.requests.length, 0);
});
test('date apply, page navigation, filter reset and Today use inclusive IST dates', async () => {
  const f = fixture(); f.grant();
  const dozen = Array.from({ length: 12 }, (_, i) => event({ id: `12345678-1234-1234-1234-${String(i).padStart(12, '0')}` }));
  reply(f.requests[0], listing(f.requests[0], dozen, { total: 13, total_pages: 2 })); await tick();
  assert.match(f.get('pages').textContent, /Page 1 of 2.*13 events/);
  assert.equal(f.get('previous').disabled, true); assert.equal(f.get('next').disabled, false);
  f.get('next').click(); assert.match(f.requests[1].url, /page=2&/);
  assert.equal(f.get('next').disabled, true); assert.equal(f.get('from').disabled, true);
  reply(f.requests[1], listing(f.requests[1], [event()], { total: 13, total_pages: 2 })); await tick();
  assert.equal(f.get('next').disabled, true); assert.equal(f.get('previous').disabled, false);
  f.get('previous').click(); reply(f.requests[2], listing(f.requests[2], dozen, { total: 13, total_pages: 2 })); await tick();
  f.get('from').value = '2026-01-01'; f.get('to').value = '2026-02-28'; f.get('from').dispatch('change');
  assert.equal(f.get('cards').children.length, 0); f.get('apply').click();
  assert.match(f.requests[3].url, /from=2026-01-01&to=2026-02-28&page=1&page_size=12$/);
  reply(f.requests[3], listing(f.requests[3])); await tick();
  f.get('to').value = '2026-02-30'; f.get('apply').click(); assert.equal(f.requests.length, 4); assert.match(f.get('status').textContent, /valid inclusive/);
  f.get('today').click(); assert.equal(f.requests.length, 5);
  assert.equal(f.get('from').value, f.get('to').value);
  reply(f.requests[4], listing(f.requests[4])); await tick();
  for (const req of f.requests) { assert.equal(req.options.credentials, 'omit'); assert.equal(req.options.cache, 'no-store'); assert.equal(req.options.headers, undefined); }
});
test('opaque XSS metadata is text only; detail has original IDs and safe own-origin permalink', async () => {
  const evil = '<img src=x onerror=alert(1)><script>bad()</script>';
  const f = await loaded([event({ summary: evil, detector: evil, severity: evil, portal_url: 'https://evil.example/?token=secret' })]);
  assert.ok(f.get('cards').textContent.includes(evil));
  assert.equal(walk(f.get('cards')).some(n => ['IMG', 'SCRIPT'].includes(n.tagName)), false);
  walk(f.get('cards')).find(n => n.tagName === 'BUTTON').click();
  reply(f.requests[1], { ok: true, event: event({ evidence: { original: evil }, portal_url: 'javascript:bad()' }) }); await tick();
  assert.ok(f.get('metadata').textContent.includes(evil));
  assert.match(f.get('detail').textContent, /original-event.*original-alert/);
  assert.equal(f.get('permalink').href, 'https://portal.example/?event_video=' + ID + '#event-videos');
  assert.equal(f.get('video').src, undefined); assert.equal(f.get('video').preload, 'none');
  assert.equal(f.get('video').controls, true); assert.equal(f.get('video').playsInline, true);
  assert.equal(f.get('video').referrerPolicy, 'no-referrer'); assert.notEqual(f.get('video').autoplay, true);
});
test('deep link survives denied login, loads selected outside filters even on list outage', async () => {
  const f = fixture('?event_video=' + ID + '#event-videos');
  f.document.documentElement.setAttribute('data-hg-auth', 'viewer'); assert.equal(f.requests.length, 0);
  f.grant(); f.requests[0].reject(new Error('gateway_access_denied_or_unavailable')); await tick();
  assert.match(f.requests[1].url, new RegExp('/' + ID + '$'));
  reply(f.requests[1], { ok: true, event: event({ detected_at: '2000-01-01T00:00:00Z' }) }); await tick();
  assert.match(f.get('outside').textContent, /outside/);
  assert.equal(f.document.getElementById('hg-event-videos').scrolled, true);
  assert.match(f.get('status').textContent, /unavailable/);
  for (const token of [ID.toUpperCase(), ID + '%0A', '../bad', ID + '&event_video=' + ID, 'not-a-uuid']) {
    const bad = await loaded([], '?event_video=' + token); assert.equal(bad.requests.length, 1);
  }
});
test('rejects synthetic and malformed lists rather than displaying an empty or real history', async () => {
  for (const body of [
    { events: [event({ provenance: 'synthetic' })] }, { events: [event({ id: 'bad' })] },
    { total: 99 }, { page_size: 999 }, { time_zone: 'UTC' }, { events: [event({ confidence: '0.9' })] },
    { events: [event(), event()], total: 2 }, { total_pages: -1 }
  ]) {
    const f = fixture(); f.grant(); reply(f.requests[0], listing(f.requests[0], [event()], body)); await tick();
    assert.equal(f.get('cards').children.length, 0); assert.match(f.get('status').textContent, /unavailable/);
    assert.equal(f.get('next').disabled, true); assert.equal(f.get('apply').disabled, false);
  }
});
test('distinguishes configured empty, unconfigured and unavailable, catching wrapper and JSON errors', async () => {
  for (const kind of ['unconfigured', 'non2xx', 'wrapper', 'json']) {
    const f = fixture(); f.grant();
    if (kind === 'unconfigured') reply(f.requests[0], { ok: false, error: 'event_videos_not_configured' }, 503);
    if (kind === 'non2xx') reply(f.requests[0], { ok: false }, 500);
    if (kind === 'wrapper') f.requests[0].reject(new Error('gateway_access_denied_or_unavailable'));
    if (kind === 'json') f.requests[0].resolve({ ok: true, status: 200, json: async () => { throw new Error('bad JSON'); } });
    await tick(); assert.match(f.get('status').textContent, kind === 'unconfigured' ? /not configured/ : /unavailable/);
    assert.doesNotMatch(f.get('status').textContent, /No real detection clips/); assert.equal(f.get('apply').disabled, false);
    f.get('refresh').click(); reply(f.requests[1], listing(f.requests[1])); await tick(); assert.match(f.get('status').textContent, /No real detection/);
  }
});
test('R2 playback is explicit, host validated, short lived, retryable, and unloaded on revoke', async () => {
  const f = await selected(); const v = f.get('video');
  assert.equal(f.requests.length, 2); f.get('play').click();
  assert.equal(f.get('play').disabled, true);
  const url = 'https://' + 'a'.repeat(32) + '.r2.cloudflarestorage.com/private/clip.mp4?X-Amz-Signature=test';
  reply(f.requests[2], { ok: true, url, expires_in: 120, video_id: ID }); await tick();
  assert.equal(v.src, url); assert.notEqual(v.paused, false);
  v.dispatch('error'); assert.equal(v.src, ''); assert.match(f.get('detail-status').textContent, /expired.*Click Play/);
  f.get('play').click(); reply(f.requests[3], { ok: true, url, expires_in: 120, video_id: ID }); await tick();
  assert.equal(v.src, url); f.revoke();
  assert.equal(v.src, ''); assert.equal(v.paused, true); assert.equal(f.get('detail').textContent, ''); assert.equal(f.get('cards').textContent, '');
  assert.equal(f.document.getElementById('hg-event-videos').hidden, true);
});
test('rejects arbitrary, credentialed, HTTP, deceptive, port, fragment and mismatched playback URLs', async () => {
  const host = 'a'.repeat(32) + '.r2.cloudflarestorage.com';
  for (const data of [
    { url: 'https://evil.example/clip' }, { url: 'http://' + host + '/clip' },
    { url: 'https://user:pass@' + host + '/clip' }, { url: 'https://' + host + '.evil.example/clip' },
    { url: 'https://' + host + ':444/clip' }, { url: 'https://' + host + '/clip#secret' },
    { url: 'javascript:alert(1)' }, { url: 'not-a-url' },
    { url: 'https://' + host + '/clip', video_id: '00000000-0000-0000-0000-000000000000' },
    { url: 'https://' + host + '/clip', expires_in: 9999 }
  ]) {
    const f = await selected(); f.get('play').click();
    reply(f.requests[2], { ok: true, expires_in: 120, video_id: ID, ...data }); await tick();
    assert.ok(!f.get('video').src); assert.match(f.get('detail-status').textContent, /unavailable/); assert.equal(f.get('play').disabled, false);
  }
});
test('revocation or logout invalidates fetch AND JSON continuations, including after relogin', async () => {
  for (const phase of ['fetch', 'json']) {
    const f = fixture(); f.grant(); let resolveJSON;
    if (phase === 'json') {
      f.requests[0].resolve({ ok: true, status: 200, json: () => new Promise(resolve => { resolveJSON = resolve; }) }); await tick();
    }
    f.document.documentElement.setAttribute('data-hg-auth', 'anonymous');
    assert.equal(f.get('cards').textContent, ''); f.grant(); assert.equal(f.requests.length, 2);
    if (phase === 'json') resolveJSON(listing(f.requests[0], [event()]));
    else reply(f.requests[0], listing(f.requests[0], [event()]));
    await tick(); assert.equal(f.get('cards').children.length, 0); assert.equal(f.get('apply').disabled, true);
    reply(f.requests[1], listing(f.requests[1])); await tick(); assert.equal(f.get('apply').disabled, false);
  }
  const f = await selected(); f.get('play').click(); const v = f.get('video'); f.revoke();
  reply(f.requests[2], { ok: true, url: 'https://' + 'a'.repeat(32) + '.r2.cloudflarestorage.com/x', video_id: ID, expires_in: 120 }); await tick();
  assert.ok(!v.src); assert.equal(f.get('detail').textContent, '');
});
test('filter changes and newer selections ignore obsolete detail/list/error responses', async () => {
  const f = await loaded(); const button = walk(f.get('cards')).find(n => n.tagName === 'BUTTON'); button.click();
  // Exercise handlers even when a browser control was just disabled: queued changes still invalidate.
  f.get('from').dispatch('change'); f.get('apply').click();
  reply(f.requests[2], listing(f.requests[2], [event()])); await tick();
  walk(f.get('cards')).find(n => n.tagName === 'BUTTON').click();
  reply(f.requests[3], { ok: true, event: event({ summary: 'new selection' }) }); await tick();
  reply(f.requests[1], { ok: true, event: event({ summary: 'obsolete selection' }) }); await tick();
  assert.match(f.get('detail').textContent, /new selection/); assert.doesNotMatch(f.get('detail').textContent, /obsolete/);
  f.get('refresh').click(); f.get('to').dispatch('change'); f.requests[4].reject(new Error('old failure')); await tick();
  assert.match(f.get('status').textContent, /Filters changed/); assert.equal(f.get('apply').disabled, false);
});
test('detail rejects synthetic, mismatched IDs, wrapper failures, and playback failures are retryable', async () => {
  for (const kind of ['synthetic', 'mismatch', 'wrapper', 'bad-json']) {
    const f = await loaded(); walk(f.get('cards')).find(n => n.tagName === 'BUTTON').click();
    if (kind === 'synthetic') reply(f.requests[1], { ok: true, event: event({ provenance: 'synthetic' }) });
    if (kind === 'mismatch') reply(f.requests[1], { ok: true, event: event({ id: '00000000-0000-0000-0000-000000000000' }) });
    if (kind === 'wrapper') f.requests[1].reject(new Error('gateway_access_denied_or_unavailable'));
    if (kind === 'bad-json') f.requests[1].resolve({ ok: true, status: 200, json: async () => { throw new Error('bad JSON'); } });
    await tick(); assert.match(f.get('detail-status').textContent, /unavailable/); assert.equal(f.get('video'), null);
    assert.equal(f.get('apply').disabled, false);
  }
  const f = await selected(); f.get('play').click(); f.requests[2].reject(new Error('gateway_access_denied_or_unavailable')); await tick();
  assert.equal(f.get('play').disabled, false); assert.ok(!f.get('video').src);
  f.get('play').click(); reply(f.requests[3], { ok: true, video_id: ID, expires_in: 120, url: 'https://' + 'a'.repeat(32) + '.r2.cloudflarestorage.com/clip' }); await tick();
  assert.ok(f.get('video').src);
});
test('CSS hides revoked UI before asynchronous attribute observers run', () => {
  const f = fixture(); const css = f.document.head.textContent;
  assert.ok(css.includes('html:not([data-hg-auth="viewer"]) #hg-event-videos'));
  assert.ok(css.includes('html:not([data-hg-household="granted"]) #hg-event-videos'));
});
test('auth denial clears private data without mutating global gateway state', async () => {
  const f = await selected(); f.get('play').click(); reply(f.requests[2], { ok: false }, 403); await tick();
  assert.equal(f.document.documentElement.getAttribute('data-hg-auth'), 'viewer');
  assert.equal(f.document.documentElement.getAttribute('data-hg-household'), 'granted');
  assert.equal(f.document.getElementById('hg-event-videos').hidden, true);
  assert.equal(f.get('cards').textContent, ''); assert.equal(f.get('detail').textContent, '');
  assert.equal(f.get('pages').textContent, '');
});
