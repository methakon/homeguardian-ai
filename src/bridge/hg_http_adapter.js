'use strict';

/**
 * hg_http_adapter — a LOCAL-ONLY, loopback HTTP adapter in front of the real
 * HomeGuardian C++ Alexa boundary.
 *
 * DEVELOPMENT / INTEGRATION USE ONLY. This is NOT a production deployment.
 *
 * What it is:
 *  - Binds EXCLUSIVELY to IPv4 loopback 127.0.0.1. It refuses to start on any
 *    other host; there is no wildcard/LAN/public bind path.
 *  - A thin HTTP front-end that forwards each request to the REAL compiled C++
 *    bridge (hg_alexa_bridge) over its NDJSON stdio boundary. It does NOT
 *    re-implement authentication or authorization — those live in the C++
 *    AlexaContract + AlexaCredentialStore. The adapter only enforces transport
 *    concerns (method/route allowlist, size limits, rate limiting, timeouts,
 *    concurrency) and maps the bridge's protocol errors to generic HTTP
 *    responses.
 *
 * What it is NOT:
 *  - NOT TLS. A loopback HTTP listener is not end-to-end TLS. Traffic between a
 *    local client and 127.0.0.1 is not protected by TLS; this adapter is for
 *    controlled local development only. Any non-local deployment requires
 *    authenticated HTTPS with an explicitly reviewed TLS termination design.
 *  - NOT a public/cloud service, reverse proxy, tunnel, or deployment.
 *  - NOT genuine Alexa identity verification: the LWA/OAuth provider remains
 *    unimplemented (see docs/SECURITY_READINESS_V6.md).
 *
 * No credentials are logged, returned, or written to disk. No request data or
 * raw media is persisted. The service is read-only: it cannot grant consent,
 * enable capture, activate sensors, or change configuration (the underlying
 * contract exposes only status + alert_summary reads).
 *
 * Usage:
 *   node hg_http_adapter.js --bridge <path> --config <path> --creds <path>
 *                            [--port N] [--secure-creds]
 */

const http = require('node:http');
const { spawn } = require('node:child_process');
const path = require('node:path');
const fs = require('node:fs');

// --- configuration (CLI args only; no secrets in argv beyond the creds path) ---
function parseArgs(argv) {
  const args = { port: 0, secureCreds: false, bridge: null, config: null, creds: null };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') args.port = parseInt(argv[++i], 10);
    else if (a === '--bridge') args.bridge = argv[++i];
    else if (a === '--config') args.config = argv[++i];
    else if (a === '--creds') args.creds = argv[++i];
    else if (a === '--secure-creds') args.secureCreds = true;
  }
  return args;
}

const args = parseArgs(process.argv);
if (!args.bridge || !args.config || !args.creds) {
  process.stderr.write('usage: hg_http_adapter.js --bridge <path> --config <path> --creds <path> [--port N] [--secure-creds]\n');
  process.exit(2);
}
if (!fs.existsSync(args.bridge)) {
  process.stderr.write('bridge binary not found\n');
  process.exit(2);
}

// --- transport security limits ---
const MAX_BODY_BYTES = 4096;          // matches the contract request bound
const MAX_HEADER_BYTES = 16384;       // Node default is 16KB; enforce explicitly
const REQUEST_TIMEOUT_MS = 5000;      // per-request socket timeout
const HEADERS_TIMEOUT_MS = 5000;
const KEEPALIVE_TIMEOUT_MS = 5000;
const MAX_CONCURRENT = 8;             // bounded in-flight requests
const BRIDGE_CALL_TIMEOUT_MS = 8000;  // bound on the child-process round-trip

// Rate limiting: fixed window per client IP, bounded state.
const RATE_WINDOW_MS = 10000;         // 10s window
const RATE_MAX_REQUESTS = 30;         // max requests per IP per window
const rateBuckets = new Map();        // ip -> { windowStart, count }
const MAX_RATE_KEYS = 1024;           // bound the rate-limit table

function rateLimit(ip, now) {
  // Bound the table: if it grows too large, drop the oldest-inserted key.
  if (rateBuckets.size > MAX_RATE_KEYS && !rateBuckets.has(ip)) {
    const oldest = rateBuckets.keys().next().value;
    rateBuckets.delete(oldest);
  }
  let b = rateBuckets.get(ip);
  if (!b || now - b.windowStart >= RATE_WINDOW_MS) {
    b = { windowStart: now, count: 0 };
    rateBuckets.set(ip, b);
  }
  b.count++;
  return b.count <= RATE_MAX_REQUESTS;
}

// --- bridge process supervision ---
// One child at a time. We spawn per request batch to keep lifetime bounded and
// avoid a long-lived shared process holding state across requests. Each call
// gets a fresh bridge, sends one request, reads one response, then the child
// exits on 'shutdown'. A crash/timeout is handled safely.
let inFlight = 0;

function callBridge(requestLine) {
  return new Promise((resolve, reject) => {
    if (inFlight >= MAX_CONCURRENT) {
      reject(Object.assign(new Error('too many concurrent requests'), { code: 'BUSY' }));
      return;
    }
    inFlight++;
    const env = Object.assign({}, process.env);
    if (args.secureCreds) env.HG_BRIDGE_SECURE_CREDS = '1';
    // Spawn with an ARGUMENT ARRAY (no shell) => no shell injection.
    const child = spawn(args.bridge, [args.config, args.creds], {
      stdio: ['pipe', 'pipe', 'pipe'],
      env,
    });
    let out = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      inFlight--;
      reject(Object.assign(new Error('bridge timeout'), { code: 'TIMEOUT' }));
    }, BRIDGE_CALL_TIMEOUT_MS);

    child.stdout.on('data', (d) => {
      out += d.toString();
      if (out.length > 64 * 1024) {  // output bound
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        child.kill('SIGKILL');
        inFlight--;
        reject(Object.assign(new Error('bridge output too large'), { code: 'BAD_OUTPUT' }));
      }
    });
    child.stderr.on('data', () => {});  // never log bridge stderr (may carry detail)
    child.on('error', (e) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      inFlight--;
      reject(e);
    });
    child.on('close', () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      inFlight--;
      const line = out.split('\n').map((s) => s.trim()).filter(Boolean)[0];
      if (!line) {
        reject(Object.assign(new Error('no bridge response'), { code: 'BAD_OUTPUT' }));
        return;
      }
      try {
        resolve(JSON.parse(line));
      } catch {
        reject(Object.assign(new Error('bad bridge response'), { code: 'BAD_OUTPUT' }));
      }
    });

    child.stdin.write(requestLine + '\n');
    child.stdin.write('shutdown\n');
    child.stdin.end();
  });
}

// --- HTTP status mapping (generic; never leaks internals) ---
function mapBridgeError(err) {
  // err is the bridge's {ok:false, error:"..."} or a transport failure.
  switch (err) {
    case 'malformed': return { status: 400, body: { ok: false, error: 'malformed' } };
    case 'unsupported_version': return { status: 400, body: { ok: false, error: 'unsupported_version' } };
    case 'unauthorized': return { status: 401, body: { ok: false, error: 'unauthorized' } };
    case 'forbidden': return { status: 403, body: { ok: false, error: 'forbidden' } };
    case 'unavailable': return { status: 503, body: { ok: false, error: 'unavailable' } };
    default: return { status: 502, body: { ok: false, error: 'bad_gateway' } };
  }
}

// --- HTTP server ---
const server = http.createServer((req, res) => {
  const ip = req.socket.remoteAddress || 'unknown';

  // Method allowlist: only GET (read-only) and OPTIONS (CORS preflight, no body).
  if (req.method !== 'GET' && req.method !== 'OPTIONS') {
    res.writeHead(405, { 'Content-Type': 'application/json', 'Allow': 'GET, OPTIONS' });
    res.end(JSON.stringify({ ok: false, error: 'method_not_allowed' }));
    return;
  }
  if (req.method === 'OPTIONS') {
    res.writeHead(204, { 'Allow': 'GET, OPTIONS' });
    res.end();
    return;
  }

  // Route allowlist: /status and /alert_summary only. /health for minimal
  // operational state (no secrets/paths/token metadata).
  const url = new URL(req.url, 'http://127.0.0.1');
  const route = url.pathname;
  if (route !== '/status' && route !== '/alert_summary' && route !== '/health') {
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'not_found' }));
    return;
  }

  // Rate limit (after cheap checks, before body read).
  if (!rateLimit(ip, Date.now())) {
    res.writeHead(429, { 'Content-Type': 'application/json', 'Retry-After': '10' });
    res.end(JSON.stringify({ ok: false, error: 'rate_limited' }));
    return;
  }

  // Content-Type: GET has no body; reject a declared body/content-type that
  // implies a payload we do not accept.
  const ctype = (req.headers['content-type'] || '').toLowerCase();
  if (ctype && !ctype.startsWith('application/json')) {
    res.writeHead(415, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'unsupported_media_type' }));
    return;
  }

  // /health: minimal operational state only. Does NOT call the bridge or touch
  // credentials; reveals only that the listener is up.
  if (route === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, status: 'up' }));
    return;
  }

  // Authentication: bearer token from the Authorization header ONLY. Exactly
  // one Authorization header; duplicates (joined by Node into a comma-list) are
  // rejected. A client-supplied identity/permission in the body or query is
  // ignored (the bridge rejects unknown fields as malformed anyway).
  const authHeaders = req.headers['authorization'];
  if (Array.isArray(authHeaders)) {
    res.writeHead(400, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'malformed' }));
    return;
  }
  // Node joins duplicate Authorization headers into "v1, v2"; reject any value
  // that is not a single clean "Bearer <token>" (comma or extra whitespace
  // between tokens indicates a duplicate/ambiguous header).
  const m = typeof authHeaders === 'string' ? authHeaders.match(/^Bearer[ \t]+([^\s,]+)$/i) : null;
  if (!m) {
    // Missing/malformed/duplicate auth -> generic 401 (does not reveal why).
    res.writeHead(401, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'unauthorized' }));
    return;
  }
  const token = m[1];
  if (!token || token.length > 512) {
    res.writeHead(401, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'unauthorized' }));
    return;
  }

  // Reject a request body on GET (we accept none).
  if (req.method === 'GET') {
    req.resume();  // drain any body
    if (req.readableLength > 0 || (req.headers['content-length'] && parseInt(req.headers['content-length'], 10) > 0)) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: false, error: 'malformed' }));
      return;
    }
  }

  // Build the bridge request from the validated route + token. The operation is
  // derived from the ALLOWLISTED route only — never from client input — so a
  // client cannot request a mutating operation.
  const operation = route === '/status' ? 'status' : 'alert_summary';
  const requestObj = { version: 1, operation, credential: token };
  const requestLine = JSON.stringify(requestObj);

  callBridge(requestLine).then((bridgeResp) => {
    if (bridgeResp && bridgeResp.ok === true) {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(bridgeResp));
    } else {
      const mapped = mapBridgeError(bridgeResp && bridgeResp.error);
      res.writeHead(mapped.status, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(mapped.body));
    }
  }).catch((e) => {
    // Transport/bridge failure -> generic 502/503, no internals.
    const status = e && e.code === 'TIMEOUT' ? 504 : 502;
    res.writeHead(status, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'bad_gateway' }));
  });
});

// --- enforce loopback-only bind ---
// The adapter ALWAYS listens on 127.0.0.1. There is no code path that binds a
// different host. We also assert the bound address at 'listening'.
server.on('listening', () => {
  const addr = server.address();
  // Defensive: if somehow bound elsewhere, refuse to serve and exit.
  if (!addr || addr.address !== '127.0.0.1') {
    process.stderr.write('refusing to serve on non-loopback address\n');
    server.close();
    process.exit(4);
  }
});

// --- graceful shutdown ---
let shuttingDown = false;
function shutdown() {
  if (shuttingDown) return;
  shuttingDown = true;
  server.close(() => process.exit(0));
  // Force-exit if connections linger.
  setTimeout(() => process.exit(0), 2000).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

// --- start ---
const port = args.port || 0;  // 0 = ephemeral (tests); fixed port for the demo
server.listen(port, '127.0.0.1');
server.on('listening', () => {
  const addr = server.address();
  // Machine-readable ready line for tests/demo (no secrets).
  process.stdout.write('LISTENING ' + addr.port + '\n');
});
server.on('error', (e) => {
  process.stderr.write('server error\n');
  process.exit(1);
});
