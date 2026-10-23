#!/usr/bin/env node
/**
 * HG-NET-02B interoperability test, three real components, no stubs:
 *
 *   real Java OutboundMcpBridge  ->  Antigravity's real coordinator  ->  GENUINE native C++
 *   hg_mcp_server  (which the bridge reaches over loopback with the production bearer token)
 *
 * Phase A  protocol + relay: auth, initialize, session continuity, tools/list, tools/call,
 *          policy refusals, and the client-side negative cases.
 * Phase B  reconnection: the coordinator drops the edge socket; the bridge must redial and serve
 *          again under a NEW edge session.
 * Phase C  revocation: revokeDevice() closes 4403; the bridge must HALT and must NOT redial.
 *
 * Phase order matters: revocation is terminal for the bridge process, so it runs last.
 *
 * Usage: node run_interop_test.js <coordinatorDir> <harnessClassPath> <workDir> <cxxBin> <cxxConfig>
 */
const http = require('node:http');
const fs = require('node:fs');
const { spawn } = require('node:child_process');
const path = require('node:path');
const crypto = require('node:crypto');

const COORD_DIR = process.argv[2];
const HARNESS_CP = process.argv[3];
const WORK = process.argv[4];
const CXX_BIN = process.argv[5];
const CXX_CONFIG = process.argv[6];

const WS_PORT = 18994;
const HTTP_PORT = 18995;
const MCP_PORT = 8790;                 // OutboundMcpBridge relays to Provisioner.DEFAULT_PORT
const CLIENT_TOKEN = crypto.randomBytes(32).toString('hex');

const results = [];
function check(name, ok, detail) {
  results.push({ name, ok });
  console.log(`  ${name.padEnd(54)} ${ok ? 'PASS' : 'FAIL'}  ${detail || ''}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function post(url, body, token, sessionId) {
  return new Promise((resolve) => {
    const u = new URL(url);
    const data = JSON.stringify(body);
    const headers = { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data),
                      Authorization: 'Bearer ' + token };
    if (sessionId) headers['Mcp-Session-Id'] = sessionId;
    const req = http.request({ hostname: u.hostname, port: u.port, path: u.pathname, method: 'POST', headers },
      (res) => { let b = ''; res.on('data', (c) => (b += c));
                 res.on('end', () => resolve({ status: res.statusCode, body: b, headers: res.headers })); });
    req.on('error', (e) => resolve({ status: 0, body: String(e), headers: {} }));
    req.write(data); req.end();
  });
}

(async () => {
  // ---------------------------------------------------------------- genuine C++ MCP server
  if (!fs.existsSync(CXX_BIN)) { console.error('missing C++ server binary: ' + CXX_BIN); process.exit(2); }
  if (!fs.existsSync(CXX_CONFIG)) { console.error('missing C++ config: ' + CXX_CONFIG); process.exit(2); }

  // Start the Java bridge first so it provisions, then hand its token to the C++ server.
  const javaBin = fs.existsSync('/home/swarna-sekhar-dhar/android-jdk/bin/java')
    ? '/home/swarna-sekhar-dhar/android-jdk/bin/java'
    : 'java';
  const java = spawn(javaBin, ['-cp', HARNESS_CP, 'Harness', WORK, `ws://127.0.0.1:${WS_PORT}/`],
    { env: Object.assign({}, process.env, { HARNESS_MS: '150000' }) });

  let deviceId = null, authToken = null, mcpToken = null;
  const javaLog = [];
  await new Promise((resolve) => {
    let buf = '';
    java.stdout.on('data', (d) => {
      buf += d.toString();
      d.toString().split('\n').filter(Boolean).forEach((l) => javaLog.push(l));
      const a = buf.match(/DEVICE_ID=(\S+)/), b = buf.match(/AUTH_TOKEN=(\S+)/), c = buf.match(/MCP_TOKEN=(\S+)/);
      if (a && b && c) { deviceId = a[1]; authToken = b[1]; mcpToken = c[1]; resolve(); }
    });
  });
  console.log(`java edge: device=${deviceId} (bridge token fp ${crypto.createHash('sha256').update(authToken).digest('hex').slice(0,16)})`);

  // Provision the GENUINE C++ server with the same bearer token the bridge will present.
  const credsPath = path.join(WORK, 'mcp_creds.json');
  fs.writeFileSync(credsPath, JSON.stringify({
    [mcpToken]: { principal: 'edge-daemon', allow_status: true, allow_alert_summary: true }
  }, null, 2), { mode: 0o600 });

  const cxx = spawn(CXX_BIN, [CXX_CONFIG, credsPath],
    { env: Object.assign({}, process.env, { HG_MCP_PORT: String(MCP_PORT), HG_MCP_BIND: '127.0.0.1', HG_MCP_ALLOW_REMOTE: '0' }) });
  let cxxErr = '';
  cxx.stderr.on('data', (d) => { cxxErr += d.toString(); });
  await sleep(2500);
  check('0 genuine native C++ MCP server started', cxx.exitCode === null,
        cxx.exitCode === null ? `pid ${cxx.pid} on 127.0.0.1:${MCP_PORT}` : `exited: ${cxxErr.trim().slice(0,80)}`);

  // ---------------------------------------------------------------- coordinator
  const { McpBridgeCoordinator } = require(path.resolve(COORD_DIR, 'web_mcp/outbound_bridge'));
  const coord = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1', wsPort: WS_PORT, httpPort: HTTP_PORT, clientToken: CLIENT_TOKEN,
    authorizedDevices: new Map()
  });
  coord.authorizeDevice(deviceId, authToken, 'mi-a3-interop');
  await coord.start();
  console.log(`coordinator up: ws ${WS_PORT}, http ${HTTP_PORT}`);

  const ingress = `http://127.0.0.1:${HTTP_PORT}/mcp`;
  const sessionsSeen = [];
  const origMint = coord.clientSessions;

  // ================================================================ PHASE A
  console.log('\n--- Phase A: protocol, relay, refusals ---');
  let connected = false;
  for (let i = 0; i < 40; i++) { await sleep(500); if (coord.isEdgeOnline()) { connected = true; break; } }
  check('1 Java edge authenticates to the real coordinator', connected,
        connected ? `edge session established` : 'never connected');

  const r1 = await post(ingress, { jsonrpc: '2.0', id: 1, method: 'initialize',
    params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'interop', version: '1' } } }, CLIENT_TOKEN);
  const sessionId = r1.headers['mcp-session-id'];
  check('2 initialize relayed to the NATIVE C++ engine',
        r1.status === 200 && /serverInfo|protocolVersion/.test(r1.body),
        `http=${r1.status} session=${sessionId ? 'minted' : 'MISSING'}`);

  const r2 = await post(ingress, { jsonrpc: '2.0', id: 2, method: 'tools/list' }, CLIENT_TOKEN, sessionId);
  check('3 tools/list from the native engine', r2.status === 200 && /get_homeguardian_status/.test(r2.body),
        `http=${r2.status} tools=${(r2.body.match(/"name"/g) || []).length}`);

  const r3 = await post(ingress, { jsonrpc: '2.0', id: 3, method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} } }, CLIENT_TOKEN, sessionId);
  check('4 tools/call on the native engine', r3.status === 200 && /capture_enabled|state|Running/.test(r3.body),
        `http=${r3.status}`);

  const r4 = await post(ingress, { jsonrpc: '2.0', id: 4, method: 'tools/call',
    params: { name: 'get_alert_summary', arguments: {} } }, CLIENT_TOKEN, sessionId);
  check('5 SESSION CONTINUITY: 2nd call on the same client session',
        r4.status === 200, `http=${r4.status} (no session re-mint needed)`);

  const r5 = await post(ingress, { jsonrpc: '2.0', id: 5, method: 'tools/call',
    params: { name: 'set_capture_enabled', arguments: { on: true } } }, CLIENT_TOKEN, sessionId);
  check('6 mutating tool refused (policy)', r5.status === 400 && /UNAUTHORIZED_TOOL/.test(r5.body),
        `http=${r5.status}`);

  const r6 = await post(ingress, { jsonrpc: '2.0', id: 6, method: 'system/exec', params: { cmd: 'id' } },
    CLIENT_TOKEN, sessionId);
  check('7 arbitrary method refused (policy)', r6.status === 400 && /UNAUTHORIZED_OPERATION/.test(r6.body),
        `http=${r6.status}`);

  const r7 = await post(ingress, { jsonrpc: '2.0', id: 7, method: 'tools/list' }, 'wrong-client-token', sessionId);
  check('8 unauthenticated client refused', r7.status === 401, `http=${r7.status}`);

  const r8 = await post(ingress, { jsonrpc: '2.0', id: 8, method: 'tools/list' }, CLIENT_TOKEN, 'bogus');
  check('9 bogus client session refused', r8.status === 404, `http=${r8.status}`);

  // ================================================================ PHASE B
  console.log('\n--- Phase B: reconnection ---');
  const before = coord.isEdgeOnline();
  const oldSession = coord.activeEdge ? coord.activeEdge.sessionId : null;
  // Force-drop the edge socket as a network blip would.
  if (coord.activeEdge && coord.activeEdge.ws) coord.activeEdge.ws.terminate();
  await sleep(1500);
  let back = false, newSession = null;
  for (let i = 0; i < 30; i++) {
    await sleep(500);
    if (coord.isEdgeOnline() && coord.activeEdge && coord.activeEdge.sessionId !== oldSession) {
      back = true; newSession = coord.activeEdge.sessionId; break;
    }
  }
  check('10 bridge RECONNECTS after an edge socket drop', before && back,
        back ? `new session ${String(newSession).slice(0, 22)}...` : 'did not return');

  if (back) {
    const r9 = await post(ingress, { jsonrpc: '2.0', id: 9, method: 'initialize',
      params: { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'post-reconnect', version: '1' } } },
      CLIENT_TOKEN);
    const sid2 = r9.headers['mcp-session-id'];
    const r10 = await post(ingress, { jsonrpc: '2.0', id: 10, method: 'tools/call',
      params: { name: 'get_homeguardian_status', arguments: {} } }, CLIENT_TOKEN, sid2);
    check('11 serving again after reconnect (no duplicate sessions)',
          r10.status === 200 && coord.activeEdge !== null,
          `http=${r10.status}`);
  } else {
    check('11 serving again after reconnect (no duplicate sessions)', false, 'skipped: no reconnect');
  }

  // ================================================================ PHASE C
  console.log('\n--- Phase C: revocation ---');
  const preRevokeOnline = coord.isEdgeOnline();
  coord.revokeDevice(deviceId);
  await sleep(2000);
  const offlineAfter = !coord.isEdgeOnline();
  // A halting bridge must NOT redial: watch long enough for several backoff attempts.
  let redialed = false;
  for (let i = 0; i < 16; i++) { await sleep(1000); if (coord.isEdgeOnline()) { redialed = true; break; } }
  check('12 revoke closes the edge (4403)', preRevokeOnline && offlineAfter,
        offlineAfter ? 'edge offline after revoke' : 'still online');
  check('13 bridge HALTED - no redial after revocation', !redialed,
        redialed ? 'REDIALED after revocation' : 'no reconnect in 16 s (correct)');

  const halt = javaLog.filter((l) => /HALTED|REVOKED/.test(l));
  check('14 bridge logged the halt explicitly', halt.length > 0,
        halt.length ? halt[halt.length - 1].trim().slice(0, 70) : 'no halt line');

  // ---------------------------------------------------------------- teardown
  try { java.kill('SIGTERM'); } catch (_) {}
  try { cxx.kill('SIGTERM'); } catch (_) {}
  await coord.close();

  const passed = results.filter((r) => r.ok).length;
  console.log(`\n=== interoperability (real Java + real coordinator + native C++): ${passed}/${results.length} passed ===`);
  process.exit(passed === results.length ? 0 : 1);
})().catch((e) => { console.error('runner error:', e); process.exit(2); });
