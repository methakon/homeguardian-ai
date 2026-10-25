'use strict';

// Private metadata only. Media goes producer → R2, never through this module.
// "real/detected" is the authenticated observer's assertion, NOT a server-verified
// classification. Retention deletion is strictly opt-in; quota is freed only
// after durable deleting state, successful DELETE and authenticated HEAD 404.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const https = require('node:https');
const { protectedRead } = (() => {
  function protectedRead(filename, limit) {
    let fd;
    try {
      if (typeof filename !== 'string' || !path.isAbsolute(filename)) throw new Error();
      safeDirectories(path.dirname(filename));
      fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
      const s = fs.fstatSync(fd);
      if (!s.isFile() || s.nlink !== 1 || (s.mode & 0o7777) !== 0o600 || s.uid !== process.getuid() || s.size > limit) throw new Error();
      const b = Buffer.alloc(limit + 1);
      const n = fs.readSync(fd, b, 0, b.length, 0);
      if (n > limit) throw new Error();
      return b.subarray(0, n).toString('utf8');
    } finally { if (fd !== undefined) fs.closeSync(fd); }
  }
  return { protectedRead };
})();
const MiB = 1024 * 1024, MAX_BYTES = 25 * MiB, MAX_STORAGE = 8 * 1024 * MiB;
const MAX_CATALOG = 16 * MiB, MAX_ENTRIES = 10000;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const IDENT = /^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$/;
const CONTROL = /[\x00-\x1f\x7f-\x9f]/;
class EventVideoError extends Error {
  constructor(status, code) { super(code); this.name = 'EventVideoError'; this.status = status; this.code = code; }
}
const fail = (status, code) => { throw new EventVideoError(status, code); };
const bad = () => fail(400, 'invalid_event_video_request');
const unavailable = () => fail(503, 'event_video_storage_unconfigured');
const corrupt = () => fail(503, 'event_video_catalog_unavailable');
function safeDirectories(dir) {
  const absolute = path.resolve(dir);
  let current = path.parse(absolute).root, privateBoundary = false;
  for (const part of absolute.slice(current.length).split(path.sep).filter(Boolean)) {
    current = path.join(current, part);
    const s = fs.lstatSync(current);
    // A sticky root-owned shared ancestor (e.g. /tmp) cannot replace a private
    // owned child. The immediate catalog/token directory must never be shared.
    const stickyAncestor = current !== absolute && s.uid === 0 && (s.mode & 0o1000);
    if (!s.isDirectory() || s.isSymbolicLink() || ((s.mode & 0o022) && !stickyAncestor && !privateBoundary) ||
        (s.uid !== 0 && s.uid !== process.getuid()) || (current === absolute && (s.mode & 0o022))) throw new Error();
    if (s.uid === process.getuid() && (s.mode & 0o077) === 0) privateBoundary = true;
  }
}
const hash = s => crypto.createHash('sha256').update(s).digest('hex');
const hmac = (key, s) => crypto.createHmac('sha256', key).update(s).digest();
const encode = s => encodeURIComponent(s).replace(/[!'()*]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
function validR2({ accountId, bucket, accessKeyId, secretAccessKey }) {
  return typeof accountId === 'string' && /^[0-9a-f]{32}$/.test(accountId) &&
    typeof bucket === 'string' && /^(?!\d+\.\d+\.\d+\.\d+$)[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket) &&
    typeof accessKeyId === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(accessKeyId) &&
    typeof secretAccessKey === 'string' && /^[\x21-\x7e]{1,256}$/.test(secretAccessKey);
}
// SigV4 query authentication: pinned account endpoint, path-style bucket, auto/s3.
// Exported for independent deterministic test vectors; no endpoint override exists.
function signR2({ accountId, bucket, accessKeyId, secretAccessKey, key, method = 'GET', headers = {}, expires = 120, now = Date.now() }) {
  if (!validR2({ accountId, bucket, accessKeyId, secretAccessKey }) || typeof key !== 'string' ||
      !key || key.length > 1024 || key.split('/').some(p => !p || p === '.' || p === '..') ||
      !['PUT', 'GET', 'HEAD', 'DELETE'].includes(method) || !Number.isInteger(expires) || expires < 1 || expires > 300 || !Number.isFinite(now)) unavailable();
  const host = `${accountId}.r2.cloudflarestorage.com`;
  const canonicalHeaders = { host };
  for (const [k, v] of Object.entries(headers)) {
    if (!['content-type', 'content-length', 'content-md5', 'if-none-match'].includes(k.toLowerCase()) || typeof v !== 'string' || CONTROL.test(v)) unavailable();
    canonicalHeaders[k.toLowerCase()] = v.trim().replace(/\s+/g, ' ');
  }
  if (method === 'PUT' && (canonicalHeaders['content-type'] !== 'video/mp4' || !/^[1-9]\d*$/.test(canonicalHeaders['content-length'] || '') || !validMd5(canonicalHeaders['content-md5']) || canonicalHeaders['if-none-match'] !== '*')) unavailable();
  const names = Object.keys(canonicalHeaders).sort();
  const stamp = new Date(now).toISOString().replace(/[:-]|\.\d{3}/g, '');
  const day = stamp.slice(0, 8), scope = `${day}/auto/s3/aws4_request`;
  const query = { 'X-Amz-Algorithm': 'AWS4-HMAC-SHA256', 'X-Amz-Credential': `${accessKeyId}/${scope}`, 'X-Amz-Date': stamp, 'X-Amz-Expires': String(expires), 'X-Amz-SignedHeaders': names.join(';') };
  const qs = Object.keys(query).sort().map(k => `${encode(k)}=${encode(query[k])}`).join('&');
  const uri = '/' + [bucket, ...key.split('/')].map(encode).join('/');
  const canonical = [method, uri, qs, names.map(k => `${k}:${canonicalHeaders[k]}\n`).join(''), names.join(';'), 'UNSIGNED-PAYLOAD'].join('\n');
  const stringToSign = ['AWS4-HMAC-SHA256', stamp, scope, hash(canonical)].join('\n');
  const signingKey = hmac(hmac(hmac(hmac('AWS4' + secretAccessKey, day), 'auto'), 's3'), 'aws4_request');
  const signature = crypto.createHmac('sha256', signingKey).update(stringToSign).digest('hex');
  return `https://${host}${uri}?${qs}&X-Amz-Signature=${signature}`;
}
function headTransport(url, { method, timeout = 8000 }) {
  return new Promise((resolve, reject) => {
    const req = https.request(url, { method, timeout, maxHeaderSize: 16384 }, res => {
      // Control responses only. Never follow redirects or accumulate a body.
      res.destroy(); resolve({ status: res.statusCode, headers: res.headers });
    });
    const deadline = setTimeout(() => req.destroy(new Error('deadline')), timeout);
    req.on('close', () => clearTimeout(deadline));
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject); req.end();
  });
}
function integerEnv(env, name, fallback, ceiling) {
  if (env[name] === undefined || env[name] === '') return fallback;
  const raw = String(env[name]);
  const n = Number(raw);
  if (!/^[1-9]\d*$/.test(raw) || !Number.isSafeInteger(n) || n > ceiling) unavailable();
  return n;
}
function dayNumber(s) {
  if (typeof s !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(s) || s.slice(0, 4) === '0000') bad();
  const n = Date.parse(s + 'T00:00:00Z');
  if (!Number.isFinite(n) || new Date(n).toISOString().slice(0, 10) !== s) bad();
  return n / 86400000;
}
function isoDate(s) {
  if (typeof s !== 'string' || !/^\d{4}-\d{2}-\d{2}T(?:[01]\d|2[0-3]):[0-5]\d:[0-5]\d(?:\.\d{1,9})?(?:Z|[+-](?:[01]\d|2[0-3]):[0-5]\d)$/.test(s)) bad();
  dayNumber(s.slice(0, 10));
  const n = Date.parse(s); if (!Number.isFinite(n)) bad();
  const canonical = new Date(n).toISOString();
  if (canonical.length !== 24 || canonical.startsWith('0000-')) bad();
  return canonical;
}
function evidenceCopy(value) {
  let nodes = 0;
  function visit(v, depth) {
    if (++nodes > 2048 || depth > 8) bad();
    if (v === null || typeof v === 'boolean' || (typeof v === 'number' && Number.isFinite(v))) return;
    if (typeof v === 'string') { if (CONTROL.test(v) || /(?:https?:\/\/|s3:\/\/|bearer\s)/i.test(v)) bad(); return; }
    if (!v || typeof v !== 'object' || (!Array.isArray(v) && Object.getPrototypeOf(v) !== Object.prototype && Object.getPrototypeOf(v) !== null)) bad();
    for (const [k, child] of Object.entries(v)) {
      if (CONTROL.test(k) || /(?:url|uri|token|secret|password|credential|authorization|cookie|access.?key|__proto__|constructor|prototype)/i.test(k)) bad();
      visit(child, depth + 1);
    }
  }
  visit(value, 0);
  let text; try { text = JSON.stringify(value); } catch (_) { bad(); }
  if (!text || Buffer.byteLength(text) > 8192) bad();
  return JSON.parse(text);
}
function validMd5(s) {
  return typeof s === 'string' && /^[A-Za-z0-9+/]{22}==$/.test(s) && Buffer.from(s, 'base64').length === 16 && Buffer.from(s, 'base64').toString('base64') === s;
}
const INPUT_FIELDS = ['event_id','alert_id','detected_at','summary','detector','severity','confidence','provenance','detection_status','duration_seconds','size_bytes','content_type','content_md5','evidence'];
function validateBody(body, limits, now, loading = false) {
  if (!body || typeof body !== 'object' || Array.isArray(body) || ![Object.prototype, null].includes(Object.getPrototypeOf(body)) || Object.keys(body).some(k => !INPUT_FIELDS.includes(k))) bad();
  if (typeof body.event_id !== 'string' || !IDENT.test(body.event_id) || (body.alert_id !== undefined && (typeof body.alert_id !== 'string' || !IDENT.test(body.alert_id)))) bad();
  const out = { event_id: body.event_id };
  if (body.alert_id !== undefined) out.alert_id = body.alert_id;
  out.detected_at = isoDate(body.detected_at);
  if (!loading && Date.parse(out.detected_at) > now + 300000) bad();
  for (const [k, max] of [['summary', 500], ['detector', 128]]) {
    if (typeof body[k] !== 'string' || !body[k].trim() || body[k].trim().length > max || CONTROL.test(body[k]) || /https?:\/\//i.test(body[k])) bad();
    out[k] = body[k].trim();
  }
  if (!['info','warning','critical'].includes(body.severity) || body.provenance !== 'real' || body.detection_status !== 'detected' || body.content_type !== 'video/mp4') bad();
  if (!validMd5(body.content_md5)) bad();
  Object.assign(out, { severity: body.severity, provenance: 'real', detection_status: 'detected', content_type: 'video/mp4', content_md5: body.content_md5 });
  if (typeof body.duration_seconds !== 'number' || !Number.isFinite(body.duration_seconds) || body.duration_seconds <= 0 || body.duration_seconds > limits.duration || !Number.isSafeInteger(body.size_bytes) || body.size_bytes < 1 || body.size_bytes > limits.bytes) bad();
  out.duration_seconds = body.duration_seconds; out.size_bytes = body.size_bytes;
  if (body.confidence !== undefined) {
    if (typeof body.confidence !== 'number' || !Number.isFinite(body.confidence) || body.confidence < 0 || body.confidence > 1) bad();
    out.confidence = body.confidence;
  }
  if (body.evidence !== undefined) out.evidence = evidenceCopy(body.evidence);
  return out;
}
function stable(value) {
  if (Array.isArray(value)) return '[' + value.map(stable).join(',') + ']';
  if (value && typeof value === 'object') return '{' + Object.keys(value).sort().map(k => JSON.stringify(k) + ':' + stable(value[k])).join(',') + '}';
  return JSON.stringify(value);
}
class EventVideos {
  constructor({ env = process.env, publicOrigin, now = Date.now, transport = headTransport } = {}) {
    this.env = env; this.publicOrigin = publicOrigin; this.now = now; this.transport = transport;
  }
  async cleanupExpired() {
    if (this.env.HG_EVENT_VIDEO_CLEANUP_ENABLED !== '1') return { ok: true, enabled: false, examined: 0, expired: 0 };
    const c = this._config();
    return this._write(c, async data => {
      const result = { ok: true, enabled: true, examined: data.entries.length, expired: 0 };
      const cutoff = this.now() - c.retention * 86400000;
      // Resume unfinished deletions first. Never reclaim a pending reservation.
      const candidates = data.entries.filter(e => e.status === 'deleting' || (e.status === 'ready' && e.ready_at <= cutoff));
      candidates.sort((a, b) => Number(b.status === 'deleting') - Number(a.status === 'deleting') || a.ready_at - b.ready_at || a.id.localeCompare(b.id));
      const e = candidates[0];
      if (!e) return result;
      await this._expire(c, data, e);
      return { ...result, expired: 1 };
    });
  }
  async _expire(c, data, e) {
    if (!['ready', 'deleting'].includes(e.status)) corrupt();
    if (e.status === 'ready') {
      e.status = 'deleting';
      try { this._save(c, data); } catch (_) { corrupt(); } // Durable denial BEFORE DELETE.
    }
    const deleted = await this._control(c, e, 'DELETE');
    if (![200, 204].includes(deleted?.status)) fail(503, 'event_video_r2_unavailable');
    const confirmed = await this._control(c, e, 'HEAD');
    if (confirmed?.status !== 404) fail(503, 'event_video_r2_unavailable');
    // Copy rather than mutate the charged state if persistence fails.
    try { this._save(c, { ...data, entries: data.entries.map(item => item === e ? { ...e, status: 'expired' } : item) }); }
    catch (_) {
      // A directory fsync can fail after rename. Restore the charged catalog
      // while still holding the lock; never report expiration on this path.
      try {
        if (this._load(c).entries.find(item => item.id === e.id)?.status === 'expired') this._save(c, data);
      } catch (_) {
        // If even rollback cannot be persisted, leave the exclusion lock for
        // operator reconciliation instead of permitting uncharged new PUTs.
        const error = new EventVideoError(503, 'event_video_catalog_unavailable');
        error.preserveCatalogLock = true; throw error;
      }
      corrupt();
    }
    return this._load(c);
  }
  async _control(c, e, method) {
    const url = signR2({ ...c.r2, key: e.object_key, method, expires: 120, now: this.now() });
    let timer;
    try {
      return await Promise.race([
        this.transport(url, { method, timeout: 2000, redirect: 'error', maxBodyBytes: 0 }),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('deadline')), 2000); })
      ]);
    } catch (_) { fail(503, 'event_video_r2_unavailable'); }
    finally { clearTimeout(timer); }
  }
  authenticateProducer(req) {
    try {
      const secret = protectedRead(this.env.HG_EVENT_VIDEO_INGEST_TOKEN_FILE, 4096).trim();
      const header = req && req.headers && req.headers.authorization;
      if (!/^[\x21-\x7e]{16,4096}$/.test(secret) || typeof header !== 'string' || header.length > 4103 || !header.startsWith('Bearer ')) return false;
      // Ignore cookies/session roles entirely. Fresh file read allows immediate rotation.
      return crypto.timingSafeEqual(Buffer.from(hash(secret), 'hex'), Buffer.from(hash(header.slice(7)), 'hex'));
    } catch (_) { return false; }
  }
  _config() {
    try {
      const e = this.env;
      const r2 = { accountId: e.HG_R2_ACCOUNT_ID, bucket: e.HG_R2_BUCKET, accessKeyId: e.HG_R2_ACCESS_KEY_ID, secretAccessKey: e.HG_R2_SECRET_ACCESS_KEY };
      if (!validR2(r2)) unavailable();
      const index = e.HG_EVENT_VIDEO_INDEX_FILE;
      if (typeof index !== 'string' || !path.isAbsolute(index) || path.resolve(index) !== index) unavailable();
      const root = path.resolve(__dirname, 'public');
      if (index === root || index.startsWith(root + path.sep)) unavailable();
      safeDirectories(path.dirname(index));
      const origin = new URL(this.publicOrigin);
      if (origin.protocol !== 'https:' || origin.username || origin.password || origin.pathname !== '/' || origin.search || origin.hash) unavailable();
      const budget = integerEnv(e, 'HG_EVENT_VIDEO_STORAGE_BUDGET_BYTES', 10000000000, Number.MAX_SAFE_INTEGER);
      // Integer arithmetic: accepted bytes must be STRICTLY below 4/5 of budget.
      const pressureLimit = Number((BigInt(budget) * 4n - 1n) / 5n);
      return { r2, index, origin: origin.origin,
        duration: integerEnv(e, 'HG_EVENT_VIDEO_MAX_DURATION_SECONDS', 30, 120),
        bytes: integerEnv(e, 'HG_EVENT_VIDEO_MAX_BYTES', MAX_BYTES, MAX_BYTES),
        storage: Math.min(integerEnv(e, 'HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES', MAX_STORAGE, MAX_STORAGE), pressureLimit),
        retention: integerEnv(e, 'HG_EVENT_VIDEO_RETENTION_DAYS', 7, 36600) };
    } catch (_) { unavailable(); }
  }
  _load(c) {
    let text;
    try { text = protectedRead(c.index, MAX_CATALOG); }
    catch (e) { if (e.code === 'ENOENT') return { version: 1, entries: [] }; corrupt(); }
    try {
      const data = JSON.parse(text);
      if (!data || Object.keys(data).sort().join(',') !== 'entries,version' || data.version !== 1 || !Array.isArray(data.entries) || data.entries.length > MAX_ENTRIES) throw new Error();
      const ids = new Set(), events = new Set(); let bytes = 0;
      for (const e of data.entries) {
        if (!e || Object.keys(e).sort().join(',') !== 'created_at,expires_at,id,metadata,object_key,ready_at,status' || !UUID.test(e.id) || ids.has(e.id) || !['pending','ready','deleting','expired'].includes(e.status) || e.object_key !== `event-videos/${e.id}.mp4` || !Number.isSafeInteger(e.created_at) || e.created_at < 0 || e.expires_at !== e.created_at + 3600000 || (e.status === 'pending' ? e.ready_at !== null : !Number.isSafeInteger(e.ready_at) || e.ready_at < e.created_at)) throw new Error();
        const m = validateBody(e.metadata, { duration: 120, bytes: MAX_BYTES }, this.now(), true);
        if (stable(m) !== stable(e.metadata) || events.has(m.event_id)) throw new Error();
        ids.add(e.id); events.add(m.event_id); if (e.status !== 'expired') bytes += m.size_bytes;
      }
      if (bytes > MAX_STORAGE) throw new Error();
      return data;
    } catch (_) { corrupt(); }
  }
  _save(c, data) {
    let fd, temp;
    try {
      const text = JSON.stringify(data);
      if (Buffer.byteLength(text) > MAX_CATALOG || data.entries.length > MAX_ENTRIES) fail(507, 'event_video_storage_full');
      // Validate existing target before replacement; never silently repair unsafe state.
      try { protectedRead(c.index, MAX_CATALOG); } catch (e) { if (e.code !== 'ENOENT') throw e; }
      temp = c.index + '.' + crypto.randomUUID() + '.tmp';
      fd = fs.openSync(temp, fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_NOFOLLOW, 0o600);
      fs.writeFileSync(fd, text); fs.fsyncSync(fd); fs.closeSync(fd); fd = undefined;
      fs.renameSync(temp, c.index); temp = undefined;
      const dir = fs.openSync(path.dirname(c.index), fs.constants.O_RDONLY | fs.constants.O_DIRECTORY | fs.constants.O_NOFOLLOW);
      try { fs.fsyncSync(dir); } finally { fs.closeSync(dir); }
    } catch (e) { if (e instanceof EventVideoError) throw e; corrupt(); }
    finally { if (fd !== undefined) fs.closeSync(fd); if (temp) { try { fs.unlinkSync(temp); } catch (_) {} } }
  }
  async _write(c, action) {
    // Cross-instance AND cross-process exclusion. Never reclaim a stale lock on
    // elapsed time: an operator must reconcile a process crash before removing it.
    const lock = c.index + '.lock'; let fd;
    const deadline = Date.now() + 8000;
    while (fd === undefined) {
      try { fd = fs.openSync(lock, fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_EXCL | fs.constants.O_NOFOLLOW, 0o600); }
      catch (e) {
        if (e.code !== 'EEXIST' || Date.now() >= deadline) fail(503, 'event_video_catalog_busy');
        await new Promise(resolve => setTimeout(resolve, 10));
      }
    }
    let preserveLock = false;
    try { return await action(this._load(c)); }
    catch (e) { preserveLock = e.preserveCatalogLock === true; throw e; }
    finally { fs.closeSync(fd); if (!preserveLock) fs.unlinkSync(lock); }
  }
  _portal(c, id) { return `${c.origin}/?event_video=${id}#event-videos`; }
  _public(c, e, detail = false) {
    const m = e.metadata;
    const result = { id: e.id, event_id: m.event_id, detected_at: m.detected_at, summary: m.summary, severity: m.severity, detector: m.detector, duration_seconds: m.duration_seconds, size_bytes: m.size_bytes, provenance: 'real', portal_url: this._portal(c, e.id), clip_status: e.status === 'expired' ? 'expired' : e.status === 'deleting' ? 'deleting' : 'available' };
    for (const k of ['alert_id', 'confidence']) if (m[k] !== undefined) result[k] = m[k];
    if (detail) { result.content_type = 'video/mp4'; result.detection_status = 'detected'; if (m.evidence !== undefined) result.evidence = JSON.parse(JSON.stringify(m.evidence)); }
    return result;
  }
  _retained(c, e) { return e.status === 'ready'; }
  _published(e) { return ['ready', 'deleting', 'expired'].includes(e.status); }
  _find(c, data, id) {
    if (typeof id !== 'string' || !UUID.test(id)) fail(404, 'event_video_not_found');
    const e = data.entries.find(e => e.id === id && this._published(e));
    if (!e) fail(404, 'event_video_not_found');
    return e;
  }
  _prepared(c, e) {
    if (e.status === 'deleting' || e.status === 'expired') fail(409, 'event_video_expired');
    const out = { ok: true, video_id: e.id, complete_path: `/internal/event-videos/uploads/${e.id}/complete`, portal_url: this._portal(c, e.id) };
    if (e.status === 'ready') return { ...out, upload: null, already_ready: true, event: this._public(c, e, true) };
    const expires = Math.min(300, Math.floor((e.expires_at - this.now()) / 1000));
    if (expires < 1) fail(409, 'event_video_reservation_expired');
    const headers = { 'Content-Type': 'video/mp4', 'Content-Length': String(e.metadata.size_bytes), 'Content-MD5': e.metadata.content_md5, 'If-None-Match': '*' };
    return { ...out, upload: { url: signR2({ ...c.r2, key: e.object_key, method: 'PUT', headers, expires, now: this.now() }), method: 'PUT', headers, expires_in: expires } };
  }
  async prepare(body) {
    const c = this._config(), metadata = validateBody(body, c, this.now());
    return this._write(c, async data => {
      const existing = data.entries.find(e => e.metadata.event_id === metadata.event_id);
      if (existing) {
        if (stable(existing.metadata) !== stable(metadata)) fail(409, 'event_video_event_conflict');
        if (existing.status === 'pending' && data.entries.reduce((n, e) => n + (e.status === 'expired' ? 0 : e.metadata.size_bytes), 0) > c.storage) fail(507, 'event_video_storage_full');
        return this._prepared(c, existing);
      }
      const charged = () => data.entries.reduce((n, e) => n + (e.status === 'expired' ? 0 : e.metadata.size_bytes), 0);
      if (data.entries.length >= MAX_ENTRIES || metadata.size_bytes > c.storage) fail(507, 'event_video_storage_full');
      if (charged() + metadata.size_bytes > c.storage && this.env.HG_EVENT_VIDEO_CLEANUP_ENABLED === '1' && this.env.HG_EVENT_VIDEO_PRESSURE_CLEANUP_ENABLED === '1') {
        // Never discard pending charges or delete clips when pending bytes alone
        // already make this request impossible. One confirmed deletion per request
        // bounds control-plane work; remaining pressure returns 507 for later retry.
        const pending = data.entries.reduce((n, e) => n + (e.status === 'pending' ? e.metadata.size_bytes : 0), 0);
        if (pending + metadata.size_bytes <= c.storage) {
          const candidates = data.entries.filter(e => e.status === 'ready' || e.status === 'deleting');
          candidates.sort((a, b) => Number(b.status === 'deleting') - Number(a.status === 'deleting') || a.ready_at - b.ready_at || a.id.localeCompare(b.id));
          if (candidates[0]) data = await this._expire(c, data, candidates[0]);
        }
      }
      if (charged() + metadata.size_bytes > c.storage) fail(507, 'event_video_storage_full');
      const id = crypto.randomUUID(), created = this.now();
      const entry = { id, object_key: `event-videos/${id}.mp4`, created_at: created, expires_at: created + 3600000, status: 'pending', ready_at: null, metadata };
      data.entries.push(entry); this._save(c, data); // Durable charge BEFORE releasing a signed PUT.
      return this._prepared(c, entry);
    });
  }
  async complete(id) {
    const c = this._config();
    if (typeof id !== 'string' || !UUID.test(id)) fail(404, 'event_video_not_found');
    return this._write(c, async data => {
      const e = data.entries.find(e => e.id === id);
      if (!e) fail(404, 'event_video_not_found');
      if (e.status === 'deleting' || e.status === 'expired') fail(409, 'event_video_expired');
      if (e.status !== 'ready') {
        if (this.now() > e.expires_at) fail(409, 'event_video_reservation_expired');
        const url = signR2({ ...c.r2, key: e.object_key, method: 'HEAD', expires: 120, now: this.now() });
        let response;
        try {
          let timer;
          try { response = await Promise.race([this.transport(url, { method: 'HEAD', timeout: 8000, redirect: 'error', maxBodyBytes: 0 }), new Promise((_, reject) => { timer = setTimeout(() => reject(new Error()), 8000); })]); }
          finally { clearTimeout(timer); }
        } catch (_) { fail(503, 'event_video_r2_unavailable'); }
        if (!response || response.status !== 200) fail(503, 'event_video_r2_unavailable');
        const get = k => typeof response.headers?.get === 'function' ? response.headers.get(k) : response.headers?.[k];
        const length = get('content-length'), type = get('content-type'), etag = get('etag');
        const expectedEtag = '"' + Buffer.from(e.metadata.content_md5, 'base64').toString('hex') + '"';
        if (typeof length !== 'string' || !/^[1-9]\d*$/.test(length) || Number(length) !== e.metadata.size_bytes || Number(length) > c.bytes || type !== 'video/mp4' || etag !== expectedEtag) fail(409, 'event_video_object_mismatch');
        if (this.now() > e.expires_at) fail(409, 'event_video_reservation_expired');
        e.status = 'ready'; e.ready_at = this.now(); this._save(c, data);
      }
      return { ok: true, event: this._public(c, e, true), portal_url: this._portal(c, e.id), notification: { summary: e.metadata.summary, portal_url: this._portal(c, e.id) } };
    });
  }
  async list(params = new URLSearchParams()) {
    const c = this._config();
    if (!(params instanceof URLSearchParams)) bad();
    const allowed = ['from','to','page','page_size'];
    for (const k of params.keys()) if (!allowed.includes(k) || params.getAll(k).length !== 1) bad();
    const today = new Date(this.now() + 19800000).toISOString().slice(0, 10);
    const from = params.get('from') ?? today, to = params.get('to') ?? today;
    const start = dayNumber(from), end = dayNumber(to);
    if (end < start || end - start + 1 > 366) bad();
    function pagination(k, fallback, cap) {
      const raw = params.get(k); if (raw === null) return fallback;
      if (!/^[1-9]\d*$/.test(raw) || !Number.isSafeInteger(Number(raw)) || Number(raw) > cap) bad();
      return Number(raw);
    }
    const page = pagination('page', 1, Number.MAX_SAFE_INTEGER), page_size = pagination('page_size', 12, 50);
    const entries = this._load(c).entries.filter(e => this._published(e) && Date.parse(e.metadata.detected_at) >= start * 86400000 - 19800000 && Date.parse(e.metadata.detected_at) < (end + 1) * 86400000 - 19800000).sort((a, b) => b.metadata.detected_at.localeCompare(a.metadata.detected_at) || a.id.localeCompare(b.id));
    const total = entries.length;
    return { ok: true, events: entries.slice((page - 1) * page_size, page * page_size).map(e => this._public(c, e)), page, page_size, total, total_pages: Math.ceil(total / page_size), from, to, time_zone: 'Asia/Kolkata' };
  }
  async detail(id) {
    const c = this._config(); return { ok: true, event: this._public(c, this._find(c, this._load(c), id), true) };
  }
  async playback(id) {
    const c = this._config(), e = this._find(c, this._load(c), id);
    if (e.status !== 'ready') fail(410, 'event_video_expired');
    return { ok: true, url: signR2({ ...c.r2, key: e.object_key, method: 'GET', expires: 120, now: this.now() }), expires_in: 120, video_id: id };
  }
  async latestNotification() {
    const c = this._config();
    const e = this._load(c).entries.filter(e => this._retained(c, e)).sort((a, b) => b.metadata.detected_at.localeCompare(a.metadata.detected_at) || a.id.localeCompare(b.id))[0];
    if (!e) return { ok: true, event: null };
    const m = e.metadata;
    return { ok: true, event: { id: e.id, event_id: m.event_id, summary: m.summary, severity: m.severity, detected_at: m.detected_at, portal_url: this._portal(c, e.id) } };
  }
}
module.exports = { EventVideos, EventVideoError, signR2 };
