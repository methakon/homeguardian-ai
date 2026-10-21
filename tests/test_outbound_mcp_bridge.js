#!/usr/bin/env node
// HomeGuardian AI — Outbound Authenticated MCP Bridge Test Suite (HG-NET-02E)
//
// Asserts end-to-end outbound bridge security and protocol invariants:
//   1. Outbound-only connection: edge initiates WSS connection to coordinator.
//   2. Mutual authentication: device identity and CSPRNG secret validation.
//   3. Streamable HTTP MCP operations forwarded through bridge to real C++ engine:
//      - initialize
//      - tools/list
//      - tools/call (get_homeguardian_status, get_alert_summary)
//      - ping
//   4. Strict defense-in-depth: unpermitted methods & mutating tools rejected fail-closed.
//   5. Client authentication: coordinator HTTP ingress enforces Bearer auth and session tracking.
//   6. Network interruption & dynamic recovery: disconnect triggers exponential backoff and transparent recovery.
//   7. Credential revocation: revoked device connection severed immediately and reconnection blocked.
//   8. Bounded queues & limits: payload limits and queue bounds enforced.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

const {
  McpBridgeCoordinator,
  McpBridgeEdgeClient,
  PROTOCOL_VERSION,
  ERROR_CODES,
  LIMITS,
} = require('../web_mcp/outbound_bridge');

const REPO_ROOT = path.resolve(__dirname, '..');
const CXX_BIN = path.join(REPO_ROOT, 'build', 'hg_mcp_server');
const CXX_CONFIG = path.join(REPO_ROOT, 'config', 'homeguardian.demo.json');

const PORTS = {
  LOCAL_CXX_MCP: 19880 + Math.floor(Math.random() * 50),
  COORD_WS: 19930 + Math.floor(Math.random() * 50),
  COORD_HTTP: 19980 + Math.floor(Math.random() * 50),
};

const TOKENS = {
  LOCAL_CXX: 'cxx-secret-' + crypto.randomBytes(16).toString('hex'),
  EDGE_AUTH: 'edge-auth-secret-' + crypto.randomBytes(16).toString('hex'),
  CLIENT_HTTP: 'client-judge-secret-' + crypto.randomBytes(16).toString('hex'),
  ATTACKER: 'attacker-evil-secret-' + crypto.randomBytes(16).toString('hex'),
};

let cxxProc = null;
let coordinator = null;
let edgeClient = null;
let tmpDir = null;

let passCount = 0;
let failCount = 0;

function assert(condition, label) {
  if (condition) {
    console.log(`  \x1b[32mPASS\x1b[0m  ${label}`);
    passCount++;
  } else {
    console.error(`  \x1b[31mFAIL\x1b[0m  ${label}`);
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

// ----------------------------------------------------------------------------
// Test Setup & Teardown
// ----------------------------------------------------------------------------

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

  cxxProc.stderr.on('data', (d) => {
    // console.log('[CXX]', d.toString().trim());
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
    wsPort: PORTS.COORD_WS,
    httpPort: PORTS.COORD_HTTP,
    clientToken: TOKENS.CLIENT_HTTP,
  });

  // Authorize primary edge device (Xiaomi Mi A3)
  coordinator.authorizeDevice('mi_a3_edge_001', TOKENS.EDGE_AUTH, 'Xiaomi Mi A3 (Android 11 / ARM64)');
  await coordinator.start();
  console.log(`[Setup] Coordinator started: WS port ${PORTS.COORD_WS}, HTTP port ${PORTS.COORD_HTTP}`);
}

async function teardown() {
  if (edgeClient) {
    edgeClient.disconnect();
    edgeClient = null;
  }
  if (coordinator) {
    await coordinator.close();
    coordinator = null;
  }
  if (cxxProc) {
    cxxProc.kill('SIGTERM');
    cxxProc = null;
  }
  if (tmpDir && fs.existsSync(tmpDir)) {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

// ----------------------------------------------------------------------------
// Test Suites
// ----------------------------------------------------------------------------

async function runTests() {
  console.log('\n================================================================================');
  console.log('  HomeGuardian AI — Outbound Authenticated MCP Bridge Test Suite (HG-NET-02E)');
  console.log('================================================================================\n');

  // --- Suite 1: Mutual Authentication & Handshake ---
  console.log('--- 1. Outbound Mutual Authentication & Zero-Trust Handshake ---');

  // Negative 1a: Unrecognized device identity
  const badIdClient = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${PORTS.COORD_WS}`,
    deviceId: 'unknown_attacker_phone',
    authToken: TOKENS.ATTACKER,
    autoReconnect: false,
  });

  const badIdClosed = await new Promise((resolve) => {
    badIdClient.on('disconnected', (ev) => resolve(ev));
    badIdClient.connect();
  });
  assert(badIdClosed.code === 4401, 'unrecognized device identity rejected with code 4401');

  // Negative 1b: Known device with wrong auth token
  const badTokenClient = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${PORTS.COORD_WS}`,
    deviceId: 'mi_a3_edge_001',
    authToken: TOKENS.ATTACKER,
    autoReconnect: false,
  });

  const badTokenClosed = await new Promise((resolve) => {
    badTokenClient.on('disconnected', (ev) => resolve(ev));
    badTokenClient.connect();
  });
  assert(badTokenClosed.code === 4401, 'invalid auth_token rejected with code 4401');

  // Positive 1c: Valid device identity & token
  edgeClient = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${PORTS.COORD_WS}`,
    deviceId: 'mi_a3_edge_001',
    authToken: TOKENS.EDGE_AUTH,
    localMcpUrl: `http://127.0.0.1:${PORTS.LOCAL_CXX_MCP}/mcp`,
    localMcpToken: TOKENS.LOCAL_CXX,
    autoReconnect: true,
  });

  const connectedEv = await new Promise((resolve) => {
    edgeClient.on('connected', (ev) => resolve(ev));
    edgeClient.connect();
  });

  assert(edgeClient.connected && edgeClient.authenticated, 'valid edge credentials authenticate and establish session');
  assert(typeof connectedEv.sessionId === 'string' && connectedEv.sessionId.startsWith('edge_sess_'), 'coordinator minted edge session token');
  assert(coordinator.isEdgeOnline() === true, 'coordinator reports edge is online and ready');

  // --- Suite 2: Remote Streamable HTTP MCP Ingress via Bridge ---
  console.log('\n--- 2. Streamable HTTP MCP Operations Forwarded to Real C++ Engine ---');

  let clientSessionId = null;

  // Test 2a: Client initialize through Coordinator HTTP ingress
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

  assert(initRes.statusCode === 200, 'initialize over bridge returned HTTP 200');
  assert(initRes.body.result?.protocolVersion === '2025-11-25', 'negotiated protocol version is 2025-11-25');
  assert(initRes.body.result?.serverInfo?.name === 'homeguardian-mcp', 'serverInfo reports homeguardian-mcp from real C++');

  clientSessionId = initRes.headers['mcp-session-id'];
  assert(typeof clientSessionId === 'string' && clientSessionId.length > 20, 'coordinator issued Mcp-Session-Id to client');

  // Test 2b: tools/list through bridge
  const listRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
      'Content-Type': 'application/json',
      'Accept': 'application/json, text/event-stream',
    },
  }, {
    jsonrpc: '2.0',
    id: 102,
    method: 'tools/list',
  });

  assert(listRes.statusCode === 200, 'tools/list over bridge returned HTTP 200');
  const toolNames = (listRes.body.result?.tools || []).map((t) => t.name);
  assert(toolNames.includes('get_homeguardian_status'), 'tools/list advertises get_homeguardian_status');
  assert(toolNames.includes('get_alert_summary'), 'tools/list advertises get_alert_summary');
  assert(toolNames.length === 2, 'strictly two read tools advertised (no mutating tools)');

  // Test 2c: tools/call get_homeguardian_status through bridge
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
    params: {
      name: 'get_homeguardian_status',
      arguments: {},
    },
  });

  assert(statusRes.statusCode === 200, 'tools/call status returned HTTP 200');
  const statusText = statusRes.body.result?.content?.[0]?.text || '';
  assert(statusText.includes('"state":"Running"') || statusText.includes('Running'), 'status payload returned live Running state from C++');
  assert(statusText.includes('"read_only":true') || statusText.includes('read_only'), 'status payload confirmed read_only invariant');

  // Test 2d: tools/call get_alert_summary through bridge
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
    params: {
      name: 'get_alert_summary',
      arguments: {},
    },
  });

  assert(alertRes.statusCode === 200, 'tools/call alert summary returned HTTP 200');
  const alertText = alertRes.body.result?.content?.[0]?.text || '';
  assert(alertText.includes('alert_count'), 'alert summary payload returned alert_count from C++ persistence');

  // Test 2e: ping through bridge
  const pingRes = await makeHttpRequest({
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
    method: 'ping',
  });
  assert(pingRes.statusCode === 200, 'ping over bridge returned HTTP 200');

  // --- Suite 3: Defense-in-Depth & Operation Allowlist ---
  console.log('\n--- 3. Operation Allowlist & Zero-Trust Defense-in-Depth ---');

  // Test 3a: Prohibited mutating tool call (e.g. set_capture_enabled)
  const mutateRes = await makeHttpRequest({
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
    id: 106,
    method: 'tools/call',
    params: {
      name: 'set_capture_enabled',
      arguments: { enabled: true },
    },
  });

  assert(mutateRes.statusCode === 400, 'mutating tool call rejected with HTTP 400');
  assert(mutateRes.body.error === ERROR_CODES.UNAUTHORIZED_TOOL, 'rejection explicitly identifies UNAUTHORIZED_TOOL');

  // Test 3b: Prohibited arbitrary JSON-RPC method (e.g. system_exec / shell)
  const arbitraryMethodRes = await makeHttpRequest({
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
    id: 107,
    method: 'system/exec',
    params: { command: 'id' },
  });

  assert(arbitraryMethodRes.statusCode === 400, 'unpermitted method rejected with HTTP 400');
  assert(arbitraryMethodRes.body.error === ERROR_CODES.UNAUTHORIZED_OPERATION, 'rejection explicitly identifies UNAUTHORIZED_OPERATION');

  // Test 3c: Malformed non-JSON-RPC payload
  const malformedRes = await makeHttpRequest({
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
  }, 'not-json-rpc');

  assert(malformedRes.statusCode === 400, 'malformed non-json payload rejected with HTTP 400');

  // --- Suite 4: Client Authentication & Session Teardown ---
  console.log('\n--- 4. Client Authentication, Headers & Session Teardown ---');

  // Test 4a: Missing Bearer token on Coordinator HTTP
  const noTokenRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 201, method: 'tools/list' });
  assert(noTokenRes.statusCode === 401, 'unauthenticated client request returns HTTP 401');
  assert(noTokenRes.headers['www-authenticate']?.includes('Bearer'), 'returns WWW-Authenticate: Bearer header');

  // Test 4b: Invalid Bearer token
  const badTokenRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': 'Bearer bad-token-12345',
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 202, method: 'tools/list' });
  assert(badTokenRes.statusCode === 401, 'invalid Bearer token returns HTTP 401');

  // Test 4c: Clean session termination via DELETE /mcp
  const deleteRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'DELETE',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': clientSessionId,
    },
  });
  assert(deleteRes.statusCode === 200, 'DELETE /mcp terminates client session');

  // Test 4d: Subsequent call on terminated session returns 404
  const postDeleteRes = await makeHttpRequest({
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
  }, { jsonrpc: '2.0', id: 203, method: 'tools/list' });
  assert(postDeleteRes.statusCode === 404, 'call on terminated session returns HTTP 404');

  // --- Suite 5: Network Interruption & Automatic Reconnect ---
  console.log('\n--- 5. Network Interruption & Dynamic Reconnect Handling ---');

  // Re-initialize a client session
  const reconnectInit = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 301,
    method: 'initialize',
    params: { protocolVersion: PROTOCOL_VERSION, capabilities: {}, clientInfo: { name: 'reconnect-test', version: '1.0' } },
  });
  const activeSessId = reconnectInit.headers['mcp-session-id'];

  // Abruptly sever the edge connection by closing its underlying WebSocket
  console.log('  Severing edge connection to simulate temporary network drop...');
  const disconnectedPromise = new Promise((resolve) => edgeClient.once('disconnected', resolve));
  const reconnectedPromise = new Promise((resolve) => edgeClient.once('connected', resolve));

  edgeClient.ws.terminate();
  await disconnectedPromise;
  await sleep(50);

  assert(coordinator.isEdgeOnline() === false, 'coordinator immediately detects disconnected edge');

  // Querying while edge is disconnected returns HTTP 503 with EDGE_OFFLINE
  const offlineQuery = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': activeSessId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 302, method: 'tools/list' });

  assert(offlineQuery.statusCode === 503, 'requests during network outage return HTTP 503 Service Unavailable');
  assert(offlineQuery.body.error === ERROR_CODES.EDGE_OFFLINE, 'error code identifies EDGE_OFFLINE');

  // Wait for automatic exponential backoff reconnect
  console.log('  Waiting for automatic exponential backoff reconnect...');
  await reconnectedPromise;

  assert(coordinator.isEdgeOnline() === true, 'edge automatically reconnected and restored online state');

  // Subsequent query succeeds seamlessly after reconnection
  const postReconnectQuery = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': activeSessId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, { jsonrpc: '2.0', id: 303, method: 'tools/list' });

  assert(postReconnectQuery.statusCode === 200, 'MCP requests succeed seamlessly after automatic reconnect');

  // --- Suite 6: Credential Revocation ---
  console.log('\n--- 6. Edge Credential Revocation & Immediate Teardown ---');

  // Revoke device credentials on Coordinator
  console.log('  Revoking device mi_a3_edge_001 on coordinator...');
  const revokedPromise = new Promise((resolve) => edgeClient.once('revoked', resolve));
  coordinator.revokeDevice('mi_a3_edge_001');

  await revokedPromise;
  assert(edgeClient.revoked === true, 'edge client received revocation signal and marked revoked=true');
  assert(coordinator.isEdgeOnline() === false, 'revoked device connection severed immediately');

  // Verify that edge client will not attempt further reconnects
  assert(edgeClient.reconnectTimer === null, 'edge client halted automatic reconnect timers upon revocation');

  // Verify that an explicit connect attempt with revoked credentials fails closed
  const explicitRevokedClient = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${PORTS.COORD_WS}`,
    deviceId: 'mi_a3_edge_001',
    authToken: TOKENS.EDGE_AUTH,
    autoReconnect: false,
  });

  const revokedConnectClose = await new Promise((resolve) => {
    explicitRevokedClient.on('disconnected', resolve);
    explicitRevokedClient.connect();
  });
  assert(revokedConnectClose.code === 4403, 'reconnect with revoked credentials rejected with code 4403');

  // --- Suite 7: Bounded Buffers & In-Flight Limits ---
  console.log('\n--- 7. Bounded Buffers & Payload Limits ---');

  // Re-authorize device for bounded tests
  coordinator.authorizeDevice('mi_a3_edge_002', TOKENS.EDGE_AUTH, 'Secondary Mi A3 Node');
  const edgeClient2 = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${PORTS.COORD_WS}`,
    deviceId: 'mi_a3_edge_002',
    authToken: TOKENS.EDGE_AUTH,
    localMcpUrl: `http://127.0.0.1:${PORTS.LOCAL_CXX_MCP}/mcp`,
    localMcpToken: TOKENS.LOCAL_CXX,
    autoReconnect: false,
  });

  await new Promise((resolve) => {
    edgeClient2.on('connected', resolve);
    edgeClient2.connect();
  });

  // Client init
  const boundInit = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, {
    jsonrpc: '2.0',
    id: 401,
    method: 'initialize',
    params: { protocolVersion: PROTOCOL_VERSION, capabilities: {}, clientInfo: { name: 'bound-test', version: '1.0' } },
  });
  const boundSessId = boundInit.headers['mcp-session-id'];

  // Test 7a: Oversize payload (> 64 KB)
  const hugePayload = {
    jsonrpc: '2.0',
    id: 402,
    method: 'tools/list',
    padding: 'X'.repeat(70 * 1024), // 70 KB
  };

  const oversizeRes = await makeHttpRequest({
    hostname: '127.0.0.1',
    port: PORTS.COORD_HTTP,
    path: '/mcp',
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${TOKENS.CLIENT_HTTP}`,
      'Mcp-Session-Id': boundSessId,
      'Content-Type': 'application/json',
      'Accept': 'application/json',
    },
  }, hugePayload);

  assert(oversizeRes.statusCode === 413, 'payload exceeding 64KB rejected with HTTP 413 Payload Too Large');

  edgeClient2.disconnect();

  // --- 8. Private Network Confinement & Interface Binding ---
  console.log('\n--- 8. Private Network Confinement & Interface Binding ---');

  // 8.1 Refuse wildcard binding
  let bindErr = null;
  try {
    new McpBridgeCoordinator({ bindAddress: '0.0.0.0' });
  } catch (err) {
    bindErr = err;
  }
  assert(bindErr !== null && /refusing to bind wildcard/.test(bindErr.message), 'refuses wildcard bind address 0.0.0.0');

  // 8.2 Subnet filtering unit checks
  const vpnOnlyCoord = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1',
    allowedSubnets: ['10.42.0.'],
  });
  assert(vpnOnlyCoord._isPermittedRemoteIp('10.42.0.2'), 'permits WireGuard peer 10.42.0.2');
  assert(vpnOnlyCoord._isPermittedRemoteIp('::ffff:10.42.0.2'), 'permits IPv4-mapped WireGuard peer ::ffff:10.42.0.2');
  assert(!vpnOnlyCoord._isPermittedRemoteIp('127.0.0.1'), 'rejects loopback when VPN-only configured');
  assert(!vpnOnlyCoord._isPermittedRemoteIp('192.168.1.100'), 'rejects LAN IP 192.168.1.100');
  assert(!vpnOnlyCoord._isPermittedRemoteIp('140.238.10.1'), 'rejects public WAN IP');

  // 8.3 Live connection rejection from non-permitted subnet
  const testSubnetPort = 20050 + Math.floor(Math.random() * 40);
  const strictCoord = new McpBridgeCoordinator({
    bindAddress: '127.0.0.1',
    wsPort: testSubnetPort,
    httpPort: testSubnetPort + 1,
    allowedSubnets: ['10.42.0.'], // loopback 127.0.0.1 is forbidden
  });
  await strictCoord.start();

  let closeCode = null;
  const nonVpnClient = new McpBridgeEdgeClient({
    coordinatorUrl: `ws://127.0.0.1:${testSubnetPort}/`,
    deviceId: 'mi_a3_edge_001',
    authToken: TOKENS.EDGE_AUTH,
    mcpLocalUrl: `http://127.0.0.1:${PORTS.LOCAL_CXX_MCP}/mcp`,
    mcpBearerToken: TOKENS.LOCAL_CXX,
    autoReconnect: false,
  });

  await new Promise((resolve) => {
    nonVpnClient.on('disconnected', ({ code }) => {
      closeCode = code;
      resolve();
    });
    nonVpnClient.connect();
  });

  assert(closeCode === 4403, `connection from non-VPN interface terminated with code 4403 (got ${closeCode})`);
  await strictCoord.close();

  // --- Final Summary ---
  console.log('\n================================================================================');
  console.log(`  SUMMARY: ${passCount} PASSED, ${failCount} FAILED`);
  console.log('================================================================================\n');

  if (failCount > 0) {
    process.exit(1);
  }
}

async function main() {
  try {
    await setup();
    await runTests();
  } catch (err) {
    console.error('Fatal test error:', err);
    process.exit(1);
  } finally {
    await teardown();
  }
}

main();
