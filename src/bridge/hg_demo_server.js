'use strict';

/**
 * hg_demo_server — serves the simulated Alexa+ web experience and proxies it to
 * the REAL HomeGuardian C++ bridge.
 *
 * What this is:
 *  - A tiny local-only static file server + JSON proxy, using the Node stdlib
 *    (http, fs, path) with NO new dependency and NO framework.
 *  - Serves the web UI (public/) and forwards UI actions to the real compiled
 *    hg_alexa_bridge over its NDJSON stdio boundary, so every value shown in
 *    the UI comes from the real backend (consent gate, event pipeline,
 *    correlation, alerts, dedup, SQLite persistence) — never fabricated.
 *  - Binds 127.0.0.1 only.
 *
 * What this is NOT:
 *  - NOT an MCP server. This is an ordinary local HTTP API for the demo.
 *  - NOT a real Alexa+ integration. This is a project-built SIMULATION of an
 *    Alexa+ conversational experience, explicitly labelled as such in the UI.
 *  - NOT live Amazon identity validation (the LWA/OAuth provider is not
 *    implemented). Authentication uses the same local test credential store as
 *    the bridge.
 *  - Does NOT activate any physical sensor; all observations are synthetic.
 *
 * The frontend must not bypass this proxy or touch SQLite directly; all state
 * flows through the real contract.
 *
 * Usage:
 *   node src/bridge/hg_demo_server.js --bridge <path> --config <path> \
 *        --creds <path> [--port N] [--token <token>]
 */

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');

// --- CLI args ---
function parseArgs(argv) {
  const args = { port: 0, bridge: null, config: null, creds: null, token: 'demo-token' };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') args.port = parseInt(argv[++i], 10);
    else if (a === '--bridge') args.bridge = argv[++i];
    else if (a === '--config') args.config = argv[++i];
    else if (a === '--creds') args.creds = argv[++i];
    else if (a === '--token') args.token = argv[++i];
  }
  return args;
}
const args = parseArgs(process.argv);
if (!args.bridge || !args.config || !args.creds) {
  process.stderr.write('usage: hg_demo_server.js --bridge <path> --config <path> --creds <path> [--port N] [--token T]\n');
  process.exit(2);
}
if (!fs.existsSync(args.bridge)) {
  process.stderr.write('bridge binary not found; build it first (see README)\n');
  process.exit(2);
}

const PUBLIC_DIR = path.join(__dirname, '..', '..', 'src', 'web');
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

// --- bridge proxy: one NDJSON round-trip per call ---
const BRIDGE_TIMEOUT_MS = 15000;
function callBridge(lines) {
  return new Promise((resolve, reject) => {
    const child = spawn(args.bridge, [args.config, args.creds], {
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    let out = '';
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill('SIGKILL');
      reject(new Error('bridge timeout'));
    }, BRIDGE_TIMEOUT_MS);
    child.stdout.on('data', (d) => { out += d.toString(); });
    child.stderr.on('data', () => {});  // never log bridge stderr
    child.on('error', (e) => { if (!settled) { settled = true; clearTimeout(timer); reject(e); } });
    child.on('close', () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      // Return ALL response lines (scenario + status + summary) in order.
      // Defensively skip any non-JSON line (e.g. a stray log line) so the
      // protocol stays machine-readable.
      const responses = out.split('\n').map((s) => s.trim()).filter((l) => l.startsWith('{')).map((l) => {
        try { return JSON.parse(l); } catch { return { ok: false, error: 'bad_output' }; }
      });
      resolve(responses);
    });
    for (const l of lines) child.stdin.write(l + '\n');
    child.stdin.write('shutdown\n');
    child.stdin.end();
  });
}

function contractRequest(op) {
  return JSON.stringify({ version: 1, operation: op, credential: args.token });
}

// --- API: run the full demo scenario and return real results ---
async function runScenario(cycles) {
  const lines = [
    'run_scenario ' + cycles,
    contractRequest('status'),
    contractRequest('alert_summary'),
  ];
  const [scenario, status, summary] = await callBridge(lines);
  return { scenario, status, summary };
}

async function queryStatus() {
  const [status, summary] = await callBridge([contractRequest('status'), contractRequest('alert_summary')]);
  return { status, summary };
}

// --- HTTP server (loopback only) ---
function sendJson(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  const route = url.pathname;

  // CORS-free local API
  if (route === '/api/status') {
    queryStatus().then((r) => sendJson(res, 200, r)).catch(() => sendJson(res, 502, { ok: false, error: 'backend_unavailable' }));
    return;
  }
  if (route === '/api/run-scenario') {
    const cyclesParam = parseInt(url.searchParams.get('cycles') || '5', 10);
    const cycles = Number.isFinite(cyclesParam) ? Math.max(0, Math.min(100, cyclesParam)) : 5;
    runScenario(cycles).then((r) => sendJson(res, 200, r)).catch(() => sendJson(res, 502, { ok: false, error: 'backend_unavailable' }));
    return;
  }
  if (route === '/api/health') {
    sendJson(res, 200, { ok: true, status: 'up', simulated: true });
    return;
  }

  // Static files (the web UI)
  if (req.method !== 'GET') { sendJson(res, 405, { ok: false, error: 'method_not_allowed' }); return; }
  let filePath = route === '/' ? '/index.html' : route;
  // Prevent path traversal.
  const resolved = path.normalize(path.join(PUBLIC_DIR, filePath));
  if (!resolved.startsWith(PUBLIC_DIR)) { sendJson(res, 403, { ok: false, error: 'forbidden' }); return; }
  fs.readFile(resolved, (err, data) => {
    if (err) { sendJson(res, 404, { ok: false, error: 'not_found' }); return; }
    const ext = path.extname(resolved).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
    res.end(data);
  });
});

server.on('listening', () => {
  const addr = server.address();
  if (!addr || addr.address !== '127.0.0.1') {
    process.stderr.write('refusing to serve on non-loopback address\n');
    server.close();
    process.exit(4);
  }
  process.stdout.write('DEMO_READY ' + addr.port + '\n');
});
server.on('error', () => { process.stderr.write('server error (port in use?)\n'); process.exit(1); });

process.on('SIGINT', () => server.close(() => process.exit(0)));
process.on('SIGTERM', () => server.close(() => process.exit(0)));

server.listen(args.port || 0, '127.0.0.1');
