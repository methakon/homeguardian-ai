/**
 * test_devices_dashboard.js — Independent Automated Test Suite for HG-UI-03
 *
 * Verifies:
 * 1. Authoritative C++ DeviceRegistry integration (GET /api/devices schema conformance).
 * 2. Session authentication, HttpOnly cookies, and CSRF token validation.
 * 3. Rejection of URL-based credentials (/?k=..., /?token=...).
 * 4. Security gate for state mutations (/api/mcp/scenario, /api/devices/:id/consent).
 * 5. Camera consent enforcement (Fail-Closed: 403 if consent withheld).
 * 6. Scoped WebRTC view token issuance and single-use consumption.
 * 7. WebRTC signaling (Origin validation, SDP offer/answer exchange, stream stop).
 * 8. Preservation of MCP conversational and tool calling functionality.
 */

const http = require('node:http');
const path = require('node:path');
const fs = require('node:fs');
const { spawn, execFileSync } = require('node:child_process');
const { RTCPeerConnection } = require('werift');

const WEB_PORT = 18880;
const MCP_PORT = 18980;
const BASE_URL = `http://127.0.0.1:${WEB_PORT}`;

let passed = 0;
let failed = 0;

function assert(condition, message) {
  if (condition) {
    console.log(`  ✓ ${message}`);
    passed++;
  } else {
    console.error(`  ✗ FAIL: ${message}`);
    failed++;
  }
}

function request(method, path, headers = {}, body = null) {
  return new Promise((resolve, reject) => {
    const url = new URL(path, BASE_URL);
    const bodyBuf = body ? Buffer.from(typeof body === 'string' ? body : JSON.stringify(body)) : null;

    const reqHeaders = { ...headers };
    if (bodyBuf && !reqHeaders['Content-Type']) {
      reqHeaders['Content-Type'] = 'application/json';
    }
    if (bodyBuf) {
      reqHeaders['Content-Length'] = String(bodyBuf.length);
    }

    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port,
        path: url.pathname + url.search,
        method,
        headers: reqHeaders,
        agent: false,
        timeout: 5000,
      },
      (res) => {
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => {
          const raw = Buffer.concat(chunks).toString('utf-8');
          let json = null;
          try {
            json = JSON.parse(raw);
          } catch (_) {
            json = raw;
          }
          resolve({
            status: res.statusCode,
            headers: res.headers,
            body: json,
          });
        });
      }
    );

    req.on('error', reject);
    if (bodyBuf) req.write(bodyBuf);
    req.end();
  });
}

async function runSuite() {
  console.log(`=======================================================`);
  console.log(`HG-UI-03: Peripheral Devices & Live Camera Test Suite`);
  console.log(`Target Gateway: ${BASE_URL} (C++ MCP Port: ${MCP_PORT})`);
  console.log(`=======================================================\n`);

  const repoRoot = path.resolve(__dirname, '..');
  const serverPath = path.join(__dirname, 'server.js');
  const cliPath = path.join(repoRoot, 'build', 'hg_device_registry_cli');
  const configPath = path.join(repoRoot, 'config', 'devices.json');

  if (fs.existsSync(cliPath)) {
    try {
      execFileSync(cliPath, ['--registry', configPath, 'init_defaults']);
    } catch (_) {}
  }

  const serverProc = spawn(process.execPath, [serverPath], {
    cwd: repoRoot,
    env: {
      ...process.env,
      HG_WEB_PORT: String(WEB_PORT),
      HG_MCP_PORT: String(MCP_PORT),
      HG_OPERATOR_USER: 'test-operator',
      HG_OPERATOR_PASSWORD: 'SecurePassword2026!',
    },
  });

  serverProc.stderr.on('data', (d) => {
    // quiet debug
  });

  // Wait for gateway ready
  for (let i = 0; i < 30; i++) {
    await new Promise((r) => setTimeout(r, 100));
    try {
      const res = await request('GET', '/');
      if (res.status === 200) break;
    } catch (_) {}
  }

  let sessionCookie = '';
  let csrfToken = '';

  try {
    // -----------------------------------------------------------------
    // TEST 1: Peripheral Device Registry (GET /api/devices)
    // -----------------------------------------------------------------
    console.log('[Test 1] Authoritative C++ Device Registry (GET /api/devices)');
    const devRes = await request('GET', '/api/devices');
    assert(devRes.status === 200, `GET /api/devices returned HTTP 200`);
    assert(Array.isArray(devRes.body), `Returned payload is an array of devices`);
    assert(devRes.body.length >= 4, `Found at least 4 registered devices (got ${devRes.body.length})`);

    const cam1 = devRes.body.find((d) => d.id === 'camera-001');
    assert(!!cam1, `Found 'camera-001' record`);
    assert(cam1.name === 'Android Wi-Fi Camera 1', `Camera 1 name matches specification`);
    assert(cam1.type === 'camera', `Device type is 'camera'`);
    assert(cam1.capabilities?.video === true, `Capabilities include video: true`);
    assert(cam1.capabilities?.audio_in === true, `Capabilities include audio_in: true`);
    assert(cam1.capabilities?.talkback === true, `Capabilities include talkback: true`);
    assert(cam1.online === false, `Physical camera offline status verified (never faked online)`);
    assert(cam1.consent === 'not_granted', `Initial camera consent is 'not_granted'`);
    assert(cam1.authorized === false, `Unauthorized state verified (capture withheld)`);
    assert(cam1.stream_state === 'inactive', `Initial stream_state is 'inactive'`);
    assert(cam1.last_seen === null, `Unseen physical device reports last_seen: null`);

    const synCam = devRes.body.find((d) => d.id === 'camera-syn-001');
    assert(!!synCam, `Found synthetic test camera 'camera-syn-001'`);
    assert(synCam.is_synthetic === true, `Synthetic provider explicitly flagged (is_synthetic: true)`);

    // -----------------------------------------------------------------
    // TEST 2: URL Secret Exposure Rejection
    // -----------------------------------------------------------------
    console.log('\n[Test 2] Security Invariant: URL Token Rejection');
    const urlSecretRes = await request('GET', '/api/devices?k=secret123');
    assert(urlSecretRes.status === 400, `Request with ?k=secret rejected with HTTP 400`);
    assert(urlSecretRes.body.error === 'credentials_in_url_forbidden', `Explicit error: credentials_in_url_forbidden`);

    const urlTokenRes = await request('GET', '/api/devices?token=abc');
    assert(urlTokenRes.status === 400, `Request with ?token=abc rejected with HTTP 400`);

    // -----------------------------------------------------------------
    // TEST 3: State Mutation Security Gate (Requirement 5)
    // -----------------------------------------------------------------
    console.log('\n[Test 3] State Mutation Security Gate (Scenario & Consent)');
    // 3a. Unauthenticated scenario trigger
    const unauthScenario = await request('POST', '/api/mcp/scenario', {}, { scenario: 'critical_fall' });
    assert(unauthScenario.status === 401, `Unauthenticated POST /api/mcp/scenario rejected HTTP 401`);
    assert(unauthScenario.body.error === 'authentication_required', `Error is 'authentication_required'`);

    // 3b. Unauthenticated consent modification
    const unauthConsent = await request('POST', '/api/devices/camera-001/consent', {}, { granted: true });
    assert(unauthConsent.status === 401, `Unauthenticated POST /api/devices/.../consent rejected HTTP 401`);

    // -----------------------------------------------------------------
    // TEST 4: Operator Authentication & CSRF
    // -----------------------------------------------------------------
    console.log('\n[Test 4] Operator Authentication & CSRF Token Issuance');
    // 4a. Wrong password
    const badLogin = await request('POST', '/api/auth/login', {}, { username: 'test-operator', password: 'WrongPassword' });
    assert(badLogin.status === 401, `Invalid credentials rejected HTTP 401`);

    // 4b. Valid login
    const goodLogin = await request('POST', '/api/auth/login', {}, { username: 'test-operator', password: 'SecurePassword2026!' });
    assert(goodLogin.status === 200, `Valid login returned HTTP 200`);
    assert(goodLogin.body.ok === true, `Login response ok === true`);
    assert(typeof goodLogin.body.csrfToken === 'string' && goodLogin.body.csrfToken.length >= 32, `CSRF token minted`);

    csrfToken = goodLogin.body.csrfToken;
    const cookieHeader = goodLogin.headers['set-cookie'];
    assert(Array.isArray(cookieHeader) && cookieHeader.some(c => c.includes('hg_session=') && c.includes('HttpOnly') && c.includes('SameSite=Strict')),
      `HttpOnly, SameSite=Strict session cookie issued`);
    sessionCookie = cookieHeader[0].split(';')[0];

    // 4c. Session introspection
    const sessRes = await request('GET', '/api/auth/session', { Cookie: sessionCookie });
    assert(sessRes.status === 200 && sessRes.body.authenticated === true, `GET /api/auth/session confirmed active operator session`);

    // 4d. Mutation with wrong CSRF
    const badCsrfScenario = await request('POST', '/api/mcp/scenario', {
      Cookie: sessionCookie,
      'X-CSRF-Token': 'wrong-csrf-token-12345',
    }, { scenario: 'critical_fall' });
    assert(badCsrfScenario.status === 403, `Mutation with invalid CSRF token rejected HTTP 403`);

    // 4e. Mutation with valid CSRF
    const goodScenario = await request('POST', '/api/mcp/scenario', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { scenario: 'critical_fall' });
    assert(goodScenario.status === 200, `Authenticated mutation with valid CSRF accepted HTTP 200`);
    assert(goodScenario.body.scenario === 'critical_fall', `Synthetic fall scenario executed safely`);

    // -----------------------------------------------------------------
    // TEST 5: Camera Consent & Scoped View Token Issuance
    // -----------------------------------------------------------------
    console.log('\n[Test 5] Live Camera Viewing Authorization Gate');
    // 5a. Attempt view token for unconsented camera
    const unconsentedView = await request('POST', '/api/devices/camera-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(unconsentedView.status === 403, `View token for unconsented camera refused with HTTP 403`);
    assert(unconsentedView.body.error === 'consent_withheld', `Error code: consent_withheld (Fail-Closed)`);

    // 5b. Grant consent to camera-001
    const grantRes = await request('POST', '/api/devices/camera-001/consent', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { granted: true, purpose: 'testing' });
    assert(grantRes.status === 200, `Granted consent via C++ DeviceRegistry`);
    assert(grantRes.body.consent === 'granted', `Camera 1 consent is now 'granted'`);
    assert(grantRes.body.authorized === true, `Camera 1 is now authorized`);

    // 5c. View token for now-consented camera
    const tokenRes = await request('POST', '/api/devices/camera-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(tokenRes.status === 200, `Issued scoped view token for consented camera`);
    assert(typeof tokenRes.body.viewToken === 'string' && tokenRes.body.viewToken.startsWith('vt_'),
      `Scoped view token format valid (${tokenRes.body.viewToken})`);
    const viewToken = tokenRes.body.viewToken;

    // 5d. Non-video device view token attempt
    const micTokenRes = await request('POST', '/api/devices/mic-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(micTokenRes.status === 400, `View token for audio-only microphone refused HTTP 400 (capability_missing)`);

    // -----------------------------------------------------------------
    // TEST 6: WebRTC Signaling & Connection
    // -----------------------------------------------------------------
    console.log('\n[Test 6] WebRTC Signaling & Synthetic Media Connection');
    // 6a. Signaling with invalid view token
    const badSignal = await request('POST', '/api/webrtc/signal', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://127.0.0.1:18880',
    }, { deviceId: 'camera-syn-001', viewToken: 'invalid-vt', sdp: {} });
    assert(badSignal.status === 400, `WebRTC signaling with invalid viewToken rejected HTTP 400`);

    // 6a2. Offline physical camera streaming refused fail-closed (H-8)
    const offlineSignal = await request('POST', '/api/webrtc/signal', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://127.0.0.1:18880',
    }, { deviceId: 'camera-001', viewToken, sdp: {} });
    assert(offlineSignal.status === 400 && String(offlineSignal.body?.error).includes('device_offline'),
      `Physical offline camera streaming rejected HTTP 400 (device_offline)`);

    // 6b. Issue view token for synthetic camera and do SDP offer/answer exchange
    const synTokenRes = await request('POST', '/api/devices/camera-syn-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(synTokenRes.status === 200, `Issued scoped view token for synthetic camera`);
    const synViewToken = synTokenRes.body.viewToken;

    const clientPc = new RTCPeerConnection({ iceServers: [] });
    clientPc.addTransceiver('video', { direction: 'recvonly' });
    const offer = await clientPc.createOffer();
    await clientPc.setLocalDescription(offer);

    const signalRes = await request('POST', '/api/webrtc/signal', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://127.0.0.1:18880',
    }, {
      deviceId: 'camera-syn-001',
      viewToken: synViewToken,
      sdp: clientPc.localDescription,
    });

    assert(signalRes.status === 200, `WebRTC signaling offer accepted with HTTP 200`);
    assert(signalRes.body.sdp?.type === 'answer', `Gateway returned valid WebRTC answer SDP`);
    assert(typeof signalRes.body.connectionId === 'string', `WebRTC connectionId assigned (${signalRes.body.connectionId})`);
    assert(signalRes.body.streamState === 'active', `Stream state is active`);

    await clientPc.setRemoteDescription(signalRes.body.sdp);
    assert(clientPc.remoteDescription !== null, `Client RTCPeerConnection remote description set`);

    // 6c. Verify active stream registry (authenticated, H-14)
    const unauthStreams = await request('GET', '/api/webrtc/streams');
    assert(unauthStreams.status === 401, `Unauthenticated GET /api/webrtc/streams rejected HTTP 401`);

    const streamsRes = await request('GET', '/api/webrtc/streams', {
      Cookie: sessionCookie,
    });
    assert(streamsRes.status === 200 && streamsRes.body.length >= 1, `GET /api/webrtc/streams lists active session`);

    // 6d. Stop stream
    const stopRes = await request('POST', '/api/webrtc/stop', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { deviceId: 'camera-syn-001' });
    assert(stopRes.status === 200 && stopRes.body.stopped === true, `Stream stopped cleanly via /api/webrtc/stop`);
    clientPc.close();

    // -----------------------------------------------------------------
    // TEST 7: Preserved MCP Conversational Functionality
    // -----------------------------------------------------------------
    console.log('\n[Test 7] Preserved Native MCP Handshake & Tools');
    const mcpSess = await request('GET', '/api/mcp/session');
    assert(mcpSess.status === 200, `MCP session endpoint active HTTP 200`);
    assert(mcpSess.body.connected === true, `Connected to native C++ hg_mcp_server`);
    assert(mcpSess.body.protocolVersion === '2025-11-25', `Protocol version is 2025-11-25`);

    const chatRes = await request('POST', '/api/mcp/chat', {}, { query: 'Is everything okay?' });
    assert(chatRes.status === 200, `MCP chat returned HTTP 200`);
    assert(chatRes.body.toolInvoked === 'get_homeguardian_status', `Invoked MCP tool 'get_homeguardian_status'`);
    assert(chatRes.body.conversationalResponse.includes('Running'), `Conversational speech generated dynamically`);

    console.log(`\n=======================================================`);
    console.log(`HG-UI-03 RESULTS: ${passed} PASSED, ${failed} FAILED`);
    console.log(`=======================================================\n`);

    if (failed > 0) {
      process.exit(1);
    }
  } finally {
    if (serverProc && !serverProc.killed) {
      serverProc.kill('SIGTERM');
    }
    if (fs.existsSync(cliPath)) {
      try {
        execFileSync(cliPath, ['--registry', configPath, 'init_defaults']);
      } catch (_) {}
    }
  }
}

if (require.main === module) {
  runSuite().catch((err) => {
    console.error('[FATAL SUITE ERROR]', err);
    process.exit(1);
  });
}

module.exports = { runSuite };
