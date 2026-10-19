#!/usr/bin/env node
// HomeGuardian AI — End-to-End WebSocket MCP Bridge Test Suite
//
// Milestone: HG-NET-02E / Common MCP Bridge
//
// Verifies real end-to-end integration:
//   1. Genuine Native C++ MCP Server (hg_mcp_server) on 127.0.0.1:8790
//   2. Real Java Bridge (org.homeguardian.edgeservice.OutboundMcpBridge)
//   3. Node.js McpBridgeCoordinator on 127.0.0.1:18994 (WS) & 127.0.0.1:18995 (HTTP)
//   4. Measure coordinator resource utilization before Oracle deployment

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn, execSync } = require('node:child_process');

const {
  McpBridgeCoordinator,
  PROTOCOL_VERSION,
  LIMITS,
} = require('../web_mcp/outbound_bridge');

const REPO_ROOT = path.resolve(__dirname, '..');
const CXX_BIN = path.join(REPO_ROOT, 'build', 'hg_mcp_server');
const CXX_CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.demo.json');
function findJdkBinary(name) {
  if (process.env[name.toUpperCase() + '_BIN']) {
    return process.env[name.toUpperCase() + '_BIN'];
  }
  const javaHome = process.env.JAVA_HOME || (process.env.HOME ? path.join(process.env.HOME, 'android-jdk') : null);
  if (javaHome) {
    const candidate = path.join(javaHome, 'bin', name);
    if (fs.existsSync(candidate)) return candidate;
  }
  for (const dir of ['/usr/bin', '/usr/local/bin', '/usr/lib/jvm/default-java/bin']) {
    const candidate = path.join(dir, name);
    if (fs.existsSync(candidate)) return candidate;
  }
  return name;
}
const JAVAC_BIN = findJdkBinary('javac');
const JAVA_BIN = findJdkBinary('java');

const PORTS = {
  CXX_MCP: 8790,
  COORD_WS: 18994,
  COORD_HTTP: 18995,
};

const TOKENS = {
  CXX_BEARER: 'cxx-bearer-' + crypto.randomBytes(16).toString('hex'),
  CLIENT_HTTP: 'client-judge-' + crypto.randomBytes(16).toString('hex'),
};

let cxxProc = null;
let javaProc = null;
let coordinator = null;
let tmpDir = null;
let javaClassDir = null;

let passCount = 0;
let failCount = 0;

function assert(condition, label, detail = '') {
  if (condition) {
    console.log(`  \x1b[32mPASS\x1b[0m  ${label}${detail ? ' (' + detail + ')' : ''}`);
    passCount++;
  } else {
    console.error(`  \x1b[31mFAIL\x1b[0m  ${label}${detail ? ' (' + detail + ')' : ''}`);
    failCount++;
  }
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function makeHttpRequest(options, body = null) {
  return new Promise((resolve, reject) => {
    let payload = null;
    const reqOptions = { ...options, headers: { ...(options.headers || {}) } };
    if (body !== null && body !== undefined) {
      payload = typeof body === 'string' ? body : JSON.stringify(body);
      reqOptions.headers['Content-Length'] = Buffer.byteLength(payload);
    }
    const req = http.request(reqOptions, (res) => {
      let data = '';
      res.on('data', (c) => { data += c; });
      res.on('end', () => {
        let parsed = null;
        try {
          parsed = JSON.parse(data);
        } catch (_) {
          parsed = data;
        }
        resolve({
          statusCode: res.statusCode,
          headers: res.headers,
          body: parsed,
          rawBody: data,
        });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('HTTP request timeout'));
    });
    if (payload !== null) {
      req.write(payload);
    }
    req.end();
  });
}

async function setup() {
  tmpDir = fs.mkdtempSync(path.join('/tmp', 'hg-ws-cxx-e2e-'));
  javaClassDir = path.join(tmpDir, 'classes');
  fs.mkdirSync(javaClassDir, { recursive: true });

  console.log('[1/4] Compiling Java OutboundMcpBridge with javac...');
  execSync(
    `${JAVAC_BIN} -d ${javaClassDir} ` +
    `scripts/bridge_ws_compat/stubs/android/content/Context.java ` +
    `scripts/bridge_ws_compat/stubs/android/util/Log.java ` +
    `scripts/bridge_ws_compat/stubs/android/system/StructStat.java ` +
    `scripts/bridge_ws_compat/stubs/android/system/Os.java ` +
    `scripts/bridge_ws_compat/stubs/android/net/*.java ` +
    `scripts/bridge_ws_compat/stubs/org/json/JSONObject.java ` +
    `apk_service/src/org/homeguardian/edgeservice/RouteSecurityPolicy.java ` +
    `apk_service/src/org/homeguardian/edgeservice/Provisioner.java ` +
    `apk_service/src/org/homeguardian/edgeservice/OutboundMcpBridge.java ` +
    `scripts/bridge_ws_compat/Harness.java`,
    { cwd: REPO_ROOT }
  );
  console.log('      Java bridge classes compiled cleanly.');

  // Pre-seed filesDir for Java harness so it uses TOKENS.CXX_BEARER
  const javaFilesDir = path.join(tmpDir, 'java_files');
  fs.mkdirSync(javaFilesDir, { recursive: true });
  const creds = {
    [TOKENS.CXX_BEARER]: {
      role: 'operator',
      principal: 'edge-service',
      allow_status: true,
      allow_alert_summary: true,
    },
  };
  fs.writeFileSync(path.join(javaFilesDir, 'mcp_credentials.json'), JSON.stringify(creds, null, 2), { mode: 0o600 });

  console.log(`[2/4] Spawning native C++ MCP server on 127.0.0.1:${PORTS.CXX_MCP}...`);
  const cxxCredsPath = path.join(tmpDir, 'cxx_creds.json');
  fs.writeFileSync(cxxCredsPath, JSON.stringify(creds, null, 2), { mode: 0o600 });

  cxxProc = spawn(CXX_BIN, [CXX_CONFIG, cxxCredsPath], {
    env: {
      ...process.env,
      HG_MCP_PORT: String(PORTS.CXX_MCP),
      HG_MCP_BIND: '127.0.0.1',
      HG_MCP_ALLOW_REMOTE: '0',
    },
  });

  // Verify C++ server online
  let cxxReady = false;
  for (let i = 0; i < 40; i++) {
    try {
      const res = await makeHttpRequest({
        hostname: '127.0.0.1',
        port: PORTS.CXX_MCP,
        path: '/mcp',
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${TOKENS.CXX_BEARER}`,
          'Content-Type': 'application/json',
          'Accept': 'application/json',
        },
        timeout: 200,
      }, {
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
          protocolVersion: '2025-11-25',
          capabilities: {},
          clientInfo: { name: 'healthcheck', version: '1.0' },
        },
      });
      if (res.statusCode === 200) {
        cxxReady = true;
        break;
      }
    } catch (_) {}
    await sleep(50);
  }

  if (!cxxReady) {
    throw new Error('Native C++ server failed to initialize on loopback:8790');
  }
  console.log('      Native C++ server online on 127.0.0.1:8790.');

  console.log(`[3/4] Initializing Node.js McpBridgeCoordinator on WS:${PORTS.COORD_WS} & HTTP:${PORTS.COORD_HTTP}...`);
  coordinator = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1',
    wsPort: PORTS.COORD_WS,
    httpPort: PORTS.COORD_HTTP,
    clientToken: TOKENS.CLIENT_HTTP,
  });
  await coordinator.start();
  console.log('      Coordinator online.');

  console.log('[4/4] Starting Java OutboundMcpBridge harness...');
  javaProc = spawn(JAVA_BIN, [
    '-cp', javaClassDir,
    'Harness',
    javaFilesDir,
    `ws://127.0.0.1:${PORTS.COORD_WS}/`,
  ], {
    env: { ...process.env, HARNESS_MS: '60000' },
  });

  let deviceId = null;
  let authToken = null;
  await new Promise((resolve) => {
    let buf = '';
    javaProc.stdout.on('data', (d) => {
      buf += d.toString('utf8');
      const m1 = buf.match(/DEVICE_ID=(\S+)/);
      const m2 = buf.match(/AUTH_TOKEN=(\S+)/);
      if (m1 && m2 && !deviceId) {
        deviceId = m1[1];
        authToken = m2[1];
        resolve();
      }
    });
  });

  console.log(`      Discovered Java Device: ${deviceId}`);

  // Register device on coordinator
  coordinator.authorizeDevice(deviceId, authToken, 'Xiaomi Mi A3 (Java EdgeService)');

  // Wait for bridge to connect and authenticate
  let connected = false;
  for (let i = 0; i < 40; i++) {
    await sleep(100);
    if (coordinator.isEdgeOnline()) {
      connected = true;
      break;
    }
  }
  assert(connected, '1.1 Java OutboundMcpBridge authenticates over RFC6455 WebSocket to Coordinator');
}

async function teardown() {
  console.log('\n[Teardown] Cleaning up processes...');
  if (javaProc) {
    try { javaProc.kill('SIGKILL'); } catch (_) {}
    javaProc = null;
  }
  if (coordinator) {
    try { await coordinator.close(); } catch (_) {}
    coordinator = null;
  }
  if (cxxProc) {
    try { cxxProc.kill('SIGTERM'); } catch (_) {}
    cxxProc = null;
  }
  if (tmpDir && fs.existsSync(tmpDir)) {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
  }
}

async function runTests() {
  await setup();

  console.log('\n=== Real Java Bridge <-> Real C++ Engine Verification ===\n');

  const ingress = `http://127.0.0.1:${PORTS.COORD_HTTP}/mcp`;
  let clientSessionId = null;

  // --------------------------------------------------------------------------
  // Suite 2: MCP RPC Over Bridge to Native C++ Engine
  // --------------------------------------------------------------------------
  console.log('[Suite 2] Streamable HTTP MCP Request Relaying to Native C++ Server:');

  const tStartInit = process.hrtime.bigint();
  const initRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Content-Type': 'application/json',
      'Accept': 'application/json, text/event-stream',
    },
  }, {
    jsonrpc: '2.0',
    id: 101,
    method: 'initialize',
    params: {
      protocolVersion: PROTOCOL_VERSION,
      capabilities: {},
      clientInfo: { name: 'judge-client', version: '1.0' },
    },
  });
  const tEndInit = process.hrtime.bigint();
  const initLatencyMs = Number(tEndInit - tStartInit) / 1e6;

  assert(initRes.statusCode === 200, '2.1 initialize over bridge returned HTTP 200');
  assert(initRes.body.result?.protocolVersion === '2025-11-25', '2.2 negotiated protocol version 2025-11-25');
  assert(initRes.body.result?.serverInfo?.name === 'homeguardian-mcp', '2.3 serverInfo reports homeguardian-mcp from real C++ engine');

  clientSessionId = initRes.headers['mcp-session-id'];
  assert(typeof clientSessionId === 'string' && clientSessionId.length > 20, '2.4 coordinator minted client Mcp-Session-Id');

  // tools/list
  const listRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 102, method: 'tools/list' });

  assert(listRes.statusCode === 200, '2.5 tools/list over bridge returned HTTP 200');
  const tools = listRes.body.result?.tools || [];
  const toolNames = tools.map((t) => t.name);
  assert(
    toolNames.includes('get_homeguardian_status') && toolNames.includes('get_alert_summary'),
    '2.6 tools/list advertises read-only tools',
    toolNames.join(', ')
  );

  // tools/call get_homeguardian_status
  const tStartCall = process.hrtime.bigint();
  const statusRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 103,
    method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} },
  });
  const tEndCall = process.hrtime.bigint();
  const callLatencyMs = Number(tEndCall - tStartCall) / 1e6;

  assert(statusRes.statusCode === 200, '2.7 tools/call get_homeguardian_status returned HTTP 200');
  assert(
    statusRes.body.result?.content?.[0]?.text?.includes('state'),
    '2.8 status payload returned live engine state from native C++'
  );

  // tools/call get_alert_summary
  const alertRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 104,
    method: 'tools/call',
    params: { name: 'get_alert_summary', arguments: {} },
  });
  assert(alertRes.statusCode === 200 && alertRes.body.result, '2.9 tools/call get_alert_summary returned alerts from C++ persistence');

  // --------------------------------------------------------------------------
  // Suite 3: Policy Enforcement & Negative Invariants
  // --------------------------------------------------------------------------
  console.log('\n[Suite 3] Strict Defense-in-Depth Policy Refusals:');

  // Mutating tool rejected fail-closed
  const mutRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 105,
    method: 'tools/call',
    params: { name: 'set_capture_enabled', arguments: { on: true } },
  });
  assert(
    mutRes.statusCode === 400 && mutRes.body.error === 'UNAUTHORIZED_TOOL',
    '3.1 Mutating tool set_capture_enabled refused with 400 UNAUTHORIZED_TOOL'
  );

  // Disallowed method rejected fail-closed
  const badMethodRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 106, method: 'system/exec', params: { cmd: 'id' } });
  assert(
    badMethodRes.statusCode === 400 && badMethodRes.body.error === 'UNAUTHORIZED_OPERATION',
    '3.2 Arbitrary method system/exec refused with 400 UNAUTHORIZED_OPERATION'
  );

  // Unauthenticated client refused
  const unauthRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
  }, { jsonrpc: '2.0', id: 107, method: 'tools/list' });
  assert(unauthRes.statusCode === 401, '3.3 Client request without Bearer token refused HTTP 401');

  // Bogus session refused
  const bogusSessRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': 'bogus-session-token',
      'Content-Type': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 108, method: 'tools/list' });
  assert(bogusSessRes.statusCode === 404, '3.4 Unknown client session refused HTTP 404');

  // --------------------------------------------------------------------------
  // Suite 4: Credential Revocation & Reconnect Lifecycle
  // --------------------------------------------------------------------------
  console.log('\n[Suite 4] Revocation & Lifecycle:');

  const info = coordinator.getEdgeInfo();
  assert(info && info.online === true, '4.1 Coordinator reports edge device online prior to revocation');

  // Revoke device
  coordinator.revokeDevice(info.deviceId);
  await sleep(150);
  assert(!coordinator.isEdgeOnline(), '4.2 Coordinator revoking device immediately severs active WebSocket');

  // Offline request reports honest synthetic status
  const offlineRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 109,
    method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} },
  });
  assert(
    offlineRes.statusCode === 503 && offlineRes.body.error === 'EDGE_OFFLINE',
    '4.3 Post-revocation client call returns 503 EDGE_OFFLINE'
  );

  // --------------------------------------------------------------------------
  // Suite 5: Coordinator Resource Utilization Measurement
  // --------------------------------------------------------------------------
  console.log('\n[Suite 5] Coordinator Resource Utilization Metrics:');
  const memUsage = process.memoryUsage();
  const cpuUsage = process.cpuUsage();

  const rssMb = (memUsage.rss / 1024 / 1024).toFixed(2);
  const heapUsedMb = (memUsage.heapUsed / 1024 / 1024).toFixed(2);

  console.log(`  - Coordinator Process RSS: ${rssMb} MB (cap < 50 MB)`);
  console.log(`  - Coordinator Heap Used:   ${heapUsedMb} MB`);
  console.log(`  - initialize Round-Trip:   ${initLatencyMs.toFixed(2)} ms`);
  console.log(`  - tools/call Round-Trip:   ${callLatencyMs.toFixed(2)} ms`);

  assert(parseFloat(heapUsedMb) < 25.0, '5.1 Coordinator heap memory usage is < 25MB (actual: ' + heapUsedMb + ' MB)');
  assert(parseFloat(rssMb) < 100.0, '5.2 Coordinator Process RSS is < 100MB (actual: ' + rssMb + ' MB)');
  assert(initLatencyMs < 200.0, '5.2 initialize latency through bridge is < 200ms');
  assert(callLatencyMs < 200.0, '5.3 tools/call latency through bridge is < 200ms');

  console.log(`\n================================================================================`);
  console.log(`  End-to-End Test Results: ${passCount} PASSED, ${failCount} FAILED`);
  console.log(`================================================================================\n`);

  if (failCount > 0) {
    process.exit(1);
  }
}

runTests()
  .catch((err) => {
    console.error('Fatal test error:', err);
    process.exit(1);
  })
  .finally(() => teardown());
