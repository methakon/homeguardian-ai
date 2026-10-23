'use strict';

const fs = require('node:fs');
const http = require('node:http');

// Open first, then validate the same descriptor; never follow a final symlink or
// block on a FIFO. Limit the read itself, not just the pre-read stat size.
function protectedRead(filename, limit = 16 * 1024) {
  let fd;
  try {
    if (!filename || !require('node:path').isAbsolute(filename)) throw new Error('invalid path');
    fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
    const s = fs.fstatSync(fd);
    if (!s.isFile() || (s.mode & 0o7777) !== 0o600 ||
        (s.uid !== 0 && s.uid !== process.getuid()) || s.size > limit) throw new Error('unsafe file');
    const buf = Buffer.alloc(limit + 1);
    const bytes = fs.readSync(fd, buf, 0, buf.length, 0);
    if (bytes > limit) throw new Error('oversized file');
    return buf.subarray(0, bytes).toString('utf8');
  } finally { if (fd !== undefined) fs.closeSync(fd); }
}

function householdAuthorized(filename, sub) {
  try {
    if (typeof sub !== 'string' || !/^[0-9]{1,255}$/.test(sub)) return false;
    const grant = JSON.parse(protectedRead(filename));
    return grant && Object.keys(grant).sort().join(',') === 'household_id,read_only,subjects' &&
      grant.household_id === 'home' && grant.read_only === true &&
      Array.isArray(grant.subjects) && grant.subjects.length > 0 &&
      grant.subjects.every(s => typeof s === 'string' && /^[0-9]{1,255}$/.test(s)) &&
      grant.subjects.includes(sub);
  } catch (_) { return false; }
}

// Only two literal paths on a loopback HTTP origin. No redirects, arbitrary
// upstream URLs, forwarded request headers, cookies, or response headers.
function bridgeRead(env, route) {
  return new Promise((resolve, reject) => {
    let origin, secret;
    try {
      origin = new URL(env.HG_PRIVATE_BRIDGE_URL || 'http://127.0.0.1:8091');
      if (origin.protocol !== 'http:' || origin.hostname !== '127.0.0.1' ||
          origin.username || origin.password || origin.pathname !== '/' || origin.search || origin.hash ||
          !['/api/camera/status', '/api/camera/frame'].includes(route)) throw new Error('invalid bridge');
      secret = protectedRead(env.HG_PRIVATE_BRIDGE_SECRET_FILE).trim();
      if (!secret || !/^[\x21-\x7e]{1,4096}$/.test(secret)) throw new Error('invalid secret');
    } catch (_) { reject(new Error('bridge unavailable')); return; }
    const limit = route.endsWith('/frame') ? 512 * 1024 : 16 * 1024;
    const req = http.get(new URL(route, origin), { headers: { 'X-HG-Secret': secret }, timeout: 5000 }, res => {
      let size = 0; const chunks = [];
      res.on('error', reject);
      res.on('data', chunk => {
        size += chunk.length;
        if (size > limit) { res.destroy(); reject(new Error('bridge oversized response')); }
        else chunks.push(chunk);
      });
      res.on('end', () => {
        if (res.statusCode !== 200) { reject(new Error('bridge unavailable')); return; }
        const body = Buffer.concat(chunks);
        if (route.endsWith('/frame')) {
          if (!/^image\/jpeg(?:;|$)/i.test(res.headers['content-type'] || '') || body.length < 4 ||
              body[0] !== 0xff || body[1] !== 0xd8 || body.at(-2) !== 0xff || body.at(-1) !== 0xd9) {
            reject(new Error('invalid JPEG')); return;
          }
          resolve(body); return;
        }
        try {
          if (!/^application\/json(?:;|$)/i.test(res.headers['content-type'] || '')) throw new Error();
          const data = JSON.parse(body.toString('utf8'));
          if (typeof data.configured !== 'boolean') throw new Error();
          // Status only: never leak URLs, locators, secrets or arbitrary bridge data.
          const out = { configured: data.configured };
          if (['synthetic_camera_fixture','live_physical_camera','unverified_camera_source'].includes(data.source_class)) {
            out.source_class = data.source_class;
          }
          if (data.configured) {
            const c = data.camera;
            if (!c || !['LIVE', 'STALE', 'DISCONNECTED', 'CONNECTING', 'UNAVAILABLE', 'UNKNOWN'].includes(c.state)) throw new Error();
            out.camera = { state: c.state };
            if (['physical_camera','synthetic','none'].includes(c.provenance)) out.camera.provenance = c.provenance;
            if (['refreshing_snapshot','snapshot'].includes(c.delivery)) out.camera.delivery = c.delivery;
            for (const key of ['frame_age_ms','width','height','last_frame_bytes','frames_served','fetch_failures']) {
              if (typeof c[key] === 'number' && Number.isFinite(c[key])) out.camera[key] = c[key];
            }
            for (const key of ['is_continuous_video','analysis_connected']) {
              if (typeof c[key] === 'boolean') out.camera[key] = c[key];
            }
          }
          resolve(out);
        } catch (_) { reject(new Error('invalid camera status')); }
      });
    });
    req.on('timeout', () => req.destroy(new Error('bridge timeout')));
    req.on('error', reject);
    const deadline = setTimeout(() => req.destroy(new Error('bridge deadline')), 5000);
    req.on('close', () => clearTimeout(deadline));
  });
}

module.exports = { householdAuthorized, bridgeRead };
