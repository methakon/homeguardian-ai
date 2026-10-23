#!/usr/bin/env node
/**
 * HG-NET-02E protocol-compatibility test: MY real Java edge client  <->  Antigravity's REAL
 * coordinator (their web_mcp/outbound_bridge/coordinator.js, unmodified).
 *
 * The stub MCP server stands in for the C++ engine ONLY on the loopback hop, because the compiled
 * hg_mcp_server binary is not present here. Everything on the wire between the Java client and the
 * coordinator is real: RFC 6455 upgrade, bridge_auth, mcp_request/mcp_response, allowlists, bounds.
 *
 * Harness bug found and fixed after the first run: the coordinator mints an Mcp-Session-Id on
 * initialize and requires it on every subsequent call, so tools/list and tools/call returned
 * 404 session_not_found. The refusal cases were also scored "PASS" on that 404 - a VACUOUS pass,
 * because a missing session is not a policy refusal. Fixed: the session id is carried forward, and
 * the refusal cases now require status 400 AND an UNAUTHORIZED_* code, so they cannot pass on a
 * session error.
 *
 * Usage: node run_compat_test.js <coordinatorDir> <harnessClassPath> <harnessWorkDir>
 */
const http = require('node:http');
const { spawn } = require('node:child_process');
const path = require('node:path');
const crypto = require('node:crypto');

const COORD_DIR = process.argv[2];
const HARNESS_CP = process.argv[3];
const WORK = process.argv[4];

const WS_PORT = 18994;
const HTTP_PORT = 18995;
const MCP_PORT = 8790;                 // OutboundMcpBridge relays to Provisioner.DEFAULT_PORT
const CLIENT_TOKEN = crypto.randomBytes(32).toString('hex');

const results = [];
function check(name, ok, detail) {
  results.push({ name, ok });
  console.log(`  ${name.padEnd(52)} ${ok ? 'PASS' : 'FAIL'}  ${detail || ''}`);
}

// ---------------------------------------------------------------- stub local MCP (loopback only)
const mcpSeen = [];
function startStubMcp() {
  return new Promise((resolve) => {
    const srv = http.createServer((req, res) => {
      let body = '';
      req.on('data', (c) => (body += c));
      req.on('end', () => {
        mcpSeen.push({ auth: req.headers['authorization'] ? 'present' : 'absent' });
        let out;
        try {
          const j = JSON.parse(body);
          if (j.method === 'initialize') {
            out = { jsonrpc: '2.0', id: j.id, result: { protocolVersion: '2025-11-25',
              serverInfo: { name: 'hg_mcp_server', version: '1.0.0' }, capabilities: { tools: {} } } };
          } else if (j.method === 'tools/list') {
            out = { jsonrpc: '2.0', id: j.id, result: { tools: [
              { name: 'get_homeguardian_status', description: 'status' },
              { name: 'get_alert_summary', description: 'alerts' } ] } };
          } else if (j.method === 'tools/call') {
            out = { jsonrpc: '2.0', id: j.id, result: { content: [ { type: 'text',
              text: JSON.stringify({ capture_enabled: false, read_only: true, state: 'Running' }) } ] } };
          } else {
            out = { jsonrpc: '2.0', id: j.id, error: { code: -32601, message: 'unknown' } };
          }
        } catch (e) {
          out = { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'parse' } };
        }
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(out));
      });
    });
    srv.listen(MCP_PORT, '127.0.0.1', () => resolve(srv));
  });
}

function post(url, body, token, sessionId) {
  return new Promise((resolve) => {
    const u = new URL(url);
    const data = JSON.stringify(body);
    const headers = { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data),
                      Authorization: 'Bearer ' + token };
    if (sessionId) headers['Mcp-Session-Id'] = sessionId;
    const req = http.request({ hostname: u.hostname, port: u.port, path: u.pathname, method: 'POST',
      headers }, (res) => {
      let b = '';
      res.on('data', (c) => (b += c));
      res.on('end', () => resolve({ status: res.statusCode, body: b, headers: res.headers }));
    });
    req.on('error', (e) => resolve({ status: 0, body: String(e), headers: {} }));
    req.write(data); req.end();
  });
}

(async () => {
  const mcp = await startStubMcp();
  console.log(`stub loopback MCP listening on 127.0.0.1:${MCP_PORT}`);

  const java = spawn('java', ['-cp', HARNESS_CP, 'Harness', WORK, `ws://127.0.0.1:${WS_PORT}/`],
    { env: Object.assign({}, process.env, { HARNESS_MS: '60000' }) });

  let deviceId = null, authToken = null;
  await new Promise((resolve) => {
    let buf = '';
    java.stdout.on('data', (d) => {
      buf += d.toString();
      process.stdout.write(d.toString().split('\n').filter(Boolean).map((l) => '  [java] ' + l).join('\n') + '\n');
      const m1 = buf.match(/DEVICE_ID=(\S+)/), m2 = buf.match(/AUTH_TOKEN=(\S+)/);
      if (m1 && m2) { deviceId = m1[1]; authToken = m2[1]; resolve(); }
    });
  });
  console.log(`java device registered as ${deviceId}`);

  const { McpBridgeCoordinator } = require(path.join(COORD_DIR, 'web_mcp/outbound_bridge'));
  const coord = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1', wsPort: WS_PORT, httpPort: HTTP_PORT, clientToken: CLIENT_TOKEN,
    authorizedDevices: new Map([[deviceId, { authToken, name: 'mi-a3-compat-test', revoked: false }]])
  });
  await coord.start();
  console.log(`coordinator up: ws ${WS_PORT}, http ${HTTP_PORT}`);

  let connected = false;
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 500));
    const d = coord.authorizedDevices.get(deviceId);
    if (d && d.connectedAt) { connected = true; break; }
  }
  check('1 Java edge authenticates to the real coordinator', connected,
        connected ? 'bridge_auth accepted' : 'never connected');

  const ingress = `http://127.0.0.1:${HTTP_PORT}/mcp`;

  // --- initialize mints the client session; carry it forward -------------------
  const r1 = await post(ingress, { jsonrpc: '2.0', id: 1, method: 'initialize',
    params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'compat', version: '1' } } }, CLIENT_TOKEN);
  const sessionId = r1.headers['mcp-session-id'];
  check('2 initialize relayed through bridge', r1.status === 200 && /protocolVersion/.test(r1.body),
        `http=${r1.status} session=${sessionId ? 'minted' : 'MISSING'}`);

  const r2 = await post(ingress, { jsonrpc: '2.0', id: 2, method: 'tools/list' }, CLIENT_TOKEN, sessionId);
  check('3 tools/list relayed through bridge', r2.status === 200 && /get_homeguardian_status/.test(r2.body),
        `http=${r2.status}`);

  const r3 = await post(ingress, { jsonrpc: '2.0', id: 3, method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} } }, CLIENT_TOKEN, sessionId);
  check('4 tools/call (allowed tool) reached the engine', r3.status === 200 && /capture_enabled/.test(r3.body),
        `http=${r3.status}`);

  // Refusals must be POLICY refusals (400 + UNAUTHORIZED_*), not session 404s.
  const r4 = await post(ingress, { jsonrpc: '2.0', id: 4, method: 'tools/call',
    params: { name: 'set_capture_enabled', arguments: { on: true } } }, CLIENT_TOKEN, sessionId);
  check('5 MUTATING tool refused (policy, not session)',
        r4.status === 400 && /UNAUTHORIZED_TOOL/.test(r4.body), `http=${r4.status} ${r4.body.slice(0,60)}`);

  const r5 = await post(ingress, { jsonrpc: '2.0', id: 5, method: 'system/exec',
    params: { cmd: 'id' } }, CLIENT_TOKEN, sessionId);
  check('6 arbitrary method refused (policy, not session)',
        r5.status === 400 && /UNAUTHORIZED_OPERATION/.test(r5.body), `http=${r5.status} ${r5.body.slice(0,60)}`);

  check('7 loopback MCP reached with a bearer token',
        mcpSeen.length > 0 && mcpSeen.every((s) => s.auth === 'present'),
        `${mcpSeen.length} local calls, all authenticated`);

  const r6 = await post(ingress, { jsonrpc: '2.0', id: 6, method: 'tools/list' }, 'wrong-client-token', sessionId);
  check('8 unauthenticated CLIENT refused by coordinator', r6.status === 401, `http=${r6.status}`);

  const r7 = await post(ingress, { jsonrpc: '2.0', id: 7, method: 'tools/list' }, CLIENT_TOKEN, 'bogus-session');
  check('9 bogus client session refused', r7.status === 404, `http=${r7.status}`);

  java.kill('SIGTERM');
  await coord.close();
  mcp.close();

  const passed = results.filter((r) => r.ok).length;
  console.log(`\n=== protocol compatibility: ${passed}/${results.length} passed ===`);
  process.exit(passed === results.length ? 0 : 1);
})().catch((e) => { console.error('runner error:', e); process.exit(2); });
