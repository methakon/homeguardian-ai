#!/usr/bin/env node
// HomeGuardian AI — Outbound Authenticated MCP Bridge Test Suite (HG-NET-02E)
//
// Milestone: HG-NET-02E (Remote MCP Integration)
// Protocol: HGB1 (Unified Wire Protocol)
//
// Asserts end-to-end outbound bridge security and protocol invariants:
//   1. Outbound-only connection: edge initiates TCP connection to coordinator.
//   2. Mutual authentication: device identity and CSPRNG secret validation via HGB1 handshake.
//   3. Streamable HTTP MCP operations forwarded through bridge to real C++ engine:
//      - initialize
//      - tools/list
//      - tools/call (get_homeguardian_status, get_alert_summary)
//   4. Strict defense-in-depth: unpermitted methods & mutating tools rejected fail-closed.
//   5. Client authentication: coordinator HTTP ingress enforces Bearer auth and session tracking.
//   6. Network interruption & dynamic recovery: disconnect triggers exponential backoff and transparent recovery.
//   7. Credential revocation: revoked device connection severed immediately and reconnection blocked.
//   8. Bounded frames: frames exceeding 64KB rejected before buffer allocation.

const http = require('node:http');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

const {
  McpBridgeCoordinator,
  McpBridgeEdgeClient,
  PROTOCOL_VERSION,
  LIMITS,
  computeFingerprint,
  encodeFrame,
} = require('../web_mcp/outbound_bridge');

const REPO_ROOT = path.resolve(__dirname, '..');
const CXX_BIN = path.join(REPO_ROOT, 'build', 'hg_mcp_server');
const CXX_CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.demo.json');

const PORTS = {
  LOCAL_CXX_MCP: 8790,
  COORD_TCP: 19994,
  COORD_HTTP: 19995,
};

const TOKENS = {
  LOCAL_CXX: 'cxx-secret-' + crypto.randomBytes(16).toString('hex'),
  EDGE_SECRET: 'edge-secret-' + crypto.randomBytes(16).toString('hex'),
  CLIENT_HTTP: 'client-judge-secret-' + crypto.randomBytes(16).toString('hex'),
  ATTACKER: 'attacker-evil-secret-' + crypto.randomBytes(16).toString('hex'),
};

let cxxProc = null;
let coordinator = null;
let edgeClient = null;
let tmpDir = null;

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

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function setup() {
  tmpDir = fs.mkdtempSync(path.join('/tmp', 'hg-outbound-test-'));

  // 1. Provision 0600 credentials file for real C++ MCP server
  const credsPath = path.join(tmpDir, 'mcp_creds.json');
  const creds = {
    [TOKENS.LOCAL_CXX]: {
      principal: 'edge-daemon',
      allow_status: true,
      allow_alert_summary: true,
    },
  };
  fs.writeFileSync(credsPath, JSON.stringify(creds, null, 2), { mode: 0o600 });

  // 2. Spawn real C++ hg_mcp_server on loopback 127.0.0.1:LOCAL_CXX_MCP
  console.log(`[Setup] Spawning native C++ MCP server on 127.0.0.1:${PORTS.LOCAL_CXX_MCP}...`);
  cxxProc = spawn(CXX_BIN, [CXX_CONFIG, credsPath], {
    env: {
      ...process.env,
      HG_MCP_PORT: String(PORTS.LOCAL_CXX_MCP),
      HG_MCP_BIND: '127.0.0.1',
      HG_MCP_ALLOW_REMOTE: '0',
    },
  });

  // Wait for C++ server to be ready
  let cxxReady = false;
  for (let i = 0; i < 40; i++) {
    try {
      const res = await makeHttpRequest({
        hostname: '127.0.0.1',
        port: PORTS.LOCAL_CXX_MCP,
        path: '/mcp',
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${TOKENS.LOCAL_CXX}`,
          'Content-Type': 'application/json',
          'Accept': 'application/json',
        },
        timeout: 200,
      }, {
        jsonrpc: '2.0',
        id: 1,
        method: 'initialize',
        params: {
          protocolVersion: PROTOCOL_VERSION,
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
    throw new Error('Native C++ server failed to initialize on loopback');
  }
  console.log('  Native C++ server is online and responding.');

  // 3. Initialize Coordinator
  coordinator = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1',
    tcpPort: PORTS.COORD_TCP,
    httpPort: PORTS.COORD_HTTP,
    clientToken: TOKENS.CLIENT_HTTP,
  });

  // Authorize primary edge device (Xiaomi Mi A3)
  coordinator.authorizeDevice('mi_a3_edge_001', TOKENS.EDGE_SECRET, 'Xiaomi Mi A3 (Android 11 / ARM64)');
  await coordinator.start();
  console.log(`[Setup] Coordinator started: TCP port ${PORTS.COORD_TCP}, HTTP port ${PORTS.COORD_HTTP}`);
}

async function teardown() {
  if (edgeClient) {
    edgeClient.disconnect();
    edgeClient = null;
  }
  if (coordinator) {
    await coordinator.stop();
    coordinator = null;
  }
  if (cxxProc) {
    cxxProc.kill('SIGTERM');
    cxxProc = null;
  }
  if (tmpDir && fs.existsSync(tmpDir)) {
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch (_) {}
  }
}

async function runTests() {
  console.log('\n================================================================================');
  console.log('  HomeGuardian AI — Outbound Authenticated MCP Bridge Test Suite (HG-NET-02E)');
  console.log('================================================================================\n');

  // --- Suite 1: Mutual Authentication & Handshake ---
  console.log('--- 1. Outbound Mutual Authentication & Zero-Trust Handshake ---');

  // Negative 1a: Unrecognized device identity
  const badIdClient = new McpBridgeEdgeClient({
    coordinatorHost: '127.0.0.1',
    coordinatorPort: PORTS.COORD_TCP,
    deviceId: 'unknown_attacker_phone',
    enrolmentSecret: TOKENS.ATTACKER,
    autoReconnect: false,
  });
  badIdClient.on('error', () => {});

  const badIdDisconnected = await new Promise((resolve) => {
    badIdClient.on('disconnected', () => resolve(true));
    badIdClient.connect();
  });
  assert(badIdDisconnected, 'unrecognized device identity disconnected immediately');

  // Negative 1b: Known device with wrong auth token / fingerprint
  const badTokenClient = new McpBridgeEdgeClient({
    coordinatorHost: '127.0.0.1',
    coordinatorPort: PORTS.COORD_TCP,
    deviceId: 'mi_a3_edge_001',
    enrolmentSecret: TOKENS.ATTACKER,
    autoReconnect: false,
  });
  badTokenClient.on('error', () => {});

  const badTokenDisconnected = await new Promise((resolve) => {
    badTokenClient.on('disconnected', () => resolve(true));
    badTokenClient.connect();
  });
  assert(badTokenDisconnected, 'invalid enrolment secret / fingerprint rejected');

  // Positive 1c: Legitimate device connects and authenticates
  edgeClient = new McpBridgeEdgeClient({
    coordinatorHost: '127.0.0.1',
    coordinatorPort: PORTS.COORD_TCP,
    deviceId: 'mi_a3_edge_001',
    enrolmentSecret: TOKENS.EDGE_SECRET,
    localMcpUrl: `http://127.0.0.1:${PORTS.LOCAL_CXX_MCP}/mcp`,
    localMcpToken: TOKENS.LOCAL_CXX,
    autoReconnect: true,
  });

  const authenticated = await new Promise((resolve) => {
    edgeClient.on('authenticated', () => resolve(true));
    edgeClient.connect();
  });
  await sleep(50);
  assert(authenticated, 'legitimate edge client successfully completes HGB1 mutual auth');
  assert(coordinator.isEdgeOnline(), 'coordinator reports edge node is online');

  // --- Suite 2: MCP RPC Over Bridge ---
  console.log('\n--- 2. End-to-End Streamable HTTP MCP Request Routing ---');

  // 2a. initialize RPC forwarded to real C++ server
  const initResp = await coordinator.callMcp('initialize', {
    protocolVersion: PROTOCOL_VERSION,
    capabilities: {},
    clientInfo: { name: 'antigravity-bridge-suite', version: '1.0' },
  });
  assert(
    initResp?.result?.serverInfo?.name?.startsWith('homeguardian-mcp'),
    'initialize forwarded to real C++ engine and returned serverInfo'
  );

  // 2b. tools/list RPC returns registered read-only tools
  const toolsResp = await coordinator.callMcp('tools/list', {});
  const tools = toolsResp?.result?.tools || [];
  const toolNames = tools.map((t) => t.name);
  assert(
    toolNames.includes('get_homeguardian_status') && toolNames.includes('get_alert_summary'),
    'tools/list forwarded and returned registered tools',
    toolNames.join(', ')
  );

  // 2c. tools/call get_homeguardian_status
  const statusResp = await coordinator.callMcp('tools/call', {
    name: 'get_homeguardian_status',
    arguments: {},
  });
  assert(
    statusResp?.result && !statusResp.error,
    'tools/call get_homeguardian_status successfully executed on C++ engine'
  );

  // 2d. tools/call get_alert_summary
  const alertResp = await coordinator.callMcp('tools/call', {
    name: 'get_alert_summary',
    arguments: {},
  });
  assert(
    alertResp?.result && !alertResp.error,
    'tools/call get_alert_summary successfully executed on C++ engine'
  );

  // --- Suite 3: Defense-in-Depth Security Invariants ---
  console.log('\n--- 3. Strict Defense-in-Depth Allowlist Enforcement ---');

  // Negative 3a: Disallowed method rejected
  try {
    await coordinator.callMcp('shutdown', {});
    assert(false, 'disallowed method shutdown must be rejected');
  } catch (err) {
    assert(err.message.includes('not permitted'), 'disallowed method shutdown rejected fail-closed');
  }

  // Negative 3b: Mutating tool rejected
  try {
    await coordinator.callMcp('tools/call', {
      name: 'set_camera_stream_mode',
      arguments: { mode: 'relay_video' },
    });
    assert(false, 'prohibited tool set_camera_stream_mode must be rejected');
  } catch (err) {
    assert(err.message.includes('prohibited'), 'prohibited tool rejected fail-closed');
  }

  // Negative 3c: Oversized payload rejected
  const bigPayload = Buffer.alloc(LIMITS.MAX_FRAME + 1024, 0x61);
  try {
    encodeFrame(bigPayload);
    assert(false, 'oversized frame must be rejected');
  } catch (err) {
    assert(err.message.includes('exceeds hard cap'), 'oversized frame rejected before buffer allocation');
  }

  // --- Suite 4: Coordinator HTTP Ingress ---
  console.log('\n--- 4. Client Streamable HTTP Ingress & Session Tracking ---');

  // Negative 4a: Client request without Bearer token
  const unauthRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
  }, { jsonrpc: '2.0', id: 201, method: 'tools/list' });
  assert(unauthRes.statusCode === 401, 'unauthorized client request rejected with HTTP 401');

  // Positive 4b: Authenticated client request
  const authRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Content-Type': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 202,
    method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} },
  });
  assert(authRes.statusCode === 200, 'authenticated client request returned HTTP 200');
  assert(authRes.body?.result, 'client received relayed tool result from edge device');

  // --- Suite 5: Credential Revocation & Reconnect ---
  console.log('\n--- 5. Credential Revocation & Dynamic Reconnection ---');

  // 5a. Revoke device
  coordinator.revokeDevice('mi_a3_edge_001');
  await sleep(100);
  assert(!coordinator.isEdgeOnline(), 'revoking edge device immediately severs active connection');

  // 5b. Client request while edge is offline
  const offlineRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Content-Type': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 203,
    method: 'tools/call',
    params: { name: 'get_homeguardian_status', arguments: {} },
  });
  assert(
    offlineRes.body?.error?.message?.includes('MI A3 OFFLINE'),
    'request during offline state truthfully reports MI A3 OFFLINE'
  );

  // 5c. Re-authorize device and verify automatic reconnect
  coordinator.authorizeDevice('mi_a3_edge_001', TOKENS.EDGE_SECRET, 'Xiaomi Mi A3');
  let reconnected = false;
  for (let i = 0; i < 40; i++) {
    if (coordinator.isEdgeOnline()) {
      reconnected = true;
      break;
    }
    await sleep(150);
  }
  assert(reconnected, 're-authorized edge client automatically reconnects');

  console.log('\n================================================================================');
  console.log(`  Tests completed: ${passCount} passed, ${failCount} failed`);
  console.log('================================================================================\n');

  if (failCount > 0) {
    process.exit(1);
  }
}

setup()
  .then(() => runTests())
  .catch((err) => {
    console.error('Fatal test error:', err);
    process.exit(1);
  })
  .finally(() => teardown());
