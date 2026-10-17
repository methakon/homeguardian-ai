/**
 * test_devices_dashboard.js — Independent Automated Test Suite for HG-UI-03 & RC2 Security Remediation
 *
 * Verifies:
 * 1. Public unauthenticated health endpoints (/health, /api/health).
 * 2. Strict authentication enforcement across all protected endpoints (GET /api/devices, /api/mcp/*, /api/webrtc/*).
 * 3. Rejection of URL-based credentials (/?k=..., /?token=...).
 * 4. Operator authentication, HttpOnly cookies, and CSRF token issuance.
 * 5. Authoritative C++ DeviceRegistry integration (GET /api/devices schema conformance).
 * 6. Security gate for state mutations (/api/mcp/scenario, /api/devices/:id/consent).
 * 7. Camera consent enforcement (Fail-Closed: 403 if consent withheld).
 * 8. Scoped WebRTC view token issuance and single-use consumption.
 * 9. WebRTC signaling (Origin validation, SDP offer/answer exchange).
 * 10. WebRTC stream ownership: cross-operator stop refusal (HTTP 403).
 * 11. Preserved MCP conversational and tool calling functionality.
 * 12. Operator logout and session/stream teardown.
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
  console.log(`HG-UI-03 & RC2: Devices, Live Camera & Security Suite`);
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
      HG_MCP_TOKEN: process.env.HG_MCP_TOKEN || 'demo-token',
      HG_OPERATOR_USER: 'test-operator',
      HG_OPERATOR_PASSWORD: 'SecurePassword2026!',
    },
  });

  serverProc.stderr.on('data', (d) => {
    // quiet debug
  });

  // Wait for gateway ready
  let ready = false;
  for (let i = 0; i < 60; i++) {
    await new Promise((r) => setTimeout(r, 100));
    try {
      const res = await request('GET', '/');
      if (res.status === 200) { ready = true; break; }
    } catch (_) {}
  }
  if (!ready) {
    throw new Error('Gateway server failed to start on port ' + WEB_PORT);
  }

  let sessionCookie = '';
  let csrfToken = '';

  try {
    // -----------------------------------------------------------------
    // TEST 1: Designated Nonsensitive Public Health Endpoints
    // -----------------------------------------------------------------
    console.log('[Test 1] Public Nonsensitive Health Endpoints (Positive Control)');
    const health1 = await request('GET', '/health');
    assert(health1.status === 200, `GET /health returns HTTP 200 without authentication`);
    assert(health1.body.status === 'ok', `GET /health body reports status 'ok'`);

    const health2 = await request('GET', '/api/health');
    assert(health2.status === 200, `GET /api/health returns HTTP 200 without authentication`);
    assert(health2.body.service === 'homeguardian-web-gateway', `GET /api/health confirms service identity`);

    // -----------------------------------------------------------------
    // TEST 2: Unauthenticated Protected Routes Refusal (Fail-Closed)
    // -----------------------------------------------------------------
    console.log('\n[Test 2] Fail-Closed Rejection of Unauthenticated Operations');
    const unauthDevices = await request('GET', '/api/devices');
    assert(unauthDevices.status === 401, `Unauthenticated GET /api/devices rejected HTTP 401`);
    assert(unauthDevices.body.error === 'authentication_required', `Device list requires authentication`);

    const unauthDevice = await request('GET', '/api/devices/camera-001');
    assert(unauthDevice.status === 401, `Unauthenticated GET /api/devices/:id rejected HTTP 401`);

    const unauthMcpSess = await request('GET', '/api/mcp/session');
    assert(unauthMcpSess.status === 401, `Unauthenticated GET /api/mcp/session rejected HTTP 401`);

    const unauthMcpStatus = await request('GET', '/api/mcp/status');
    assert(unauthMcpStatus.status === 401, `Unauthenticated GET /api/mcp/status rejected HTTP 401`);

    const unauthMcpAlerts = await request('GET', '/api/mcp/alerts');
    assert(unauthMcpAlerts.status === 401, `Unauthenticated GET /api/mcp/alerts rejected HTTP 401`);

    const unauthMcpChat = await request('POST', '/api/mcp/chat', {}, { query: 'status' });
    assert(unauthMcpChat.status === 401, `Unauthenticated POST /api/mcp/chat rejected HTTP 401`);

    const unauthStreams = await request('GET', '/api/webrtc/streams');
    assert(unauthStreams.status === 401, `Unauthenticated GET /api/webrtc/streams rejected HTTP 401`);

    const unauthScenario = await request('POST', '/api/mcp/scenario', {}, { scenario: 'critical_fall' });
    assert(unauthScenario.status === 401, `Unauthenticated POST /api/mcp/scenario rejected HTTP 401`);

    const unauthConsent = await request('POST', '/api/devices/camera-001/consent', {}, { granted: true });
    assert(unauthConsent.status === 401, `Unauthenticated POST /api/devices/.../consent rejected HTTP 401`);

    // -----------------------------------------------------------------
    // TEST 3: URL Secret Exposure Rejection
    // -----------------------------------------------------------------
    console.log('\n[Test 3] Security Invariant: URL Token Rejection');
    const urlSecretRes = await request('GET', '/api/devices?k=secret123');
    assert(urlSecretRes.status === 400, `Request with ?k=secret rejected with HTTP 400`);
    assert(urlSecretRes.body.error === 'credentials_in_url_forbidden', `Explicit error: credentials_in_url_forbidden`);

    const urlTokenRes = await request('GET', '/api/devices?token=abc');
    assert(urlTokenRes.status === 400, `Request with ?token=abc rejected with HTTP 400`);

    // -----------------------------------------------------------------
    // TEST 4: Operator Authentication & CSRF
    // -----------------------------------------------------------------
    console.log('\n[Test 4] Operator Authentication & CSRF Token Issuance');
    const badLogin = await request('POST', '/api/auth/login', {}, { username: 'test-operator', password: 'WrongPassword' });
    assert(badLogin.status === 401, `Invalid credentials rejected HTTP 401`);

    const goodLogin = await request('POST', '/api/auth/login', {}, { username: 'test-operator', password: 'SecurePassword2026!' });
    assert(goodLogin.status === 200, `Valid login returned HTTP 200`);
    assert(goodLogin.body.ok === true, `Login response ok === true`);
    assert(typeof goodLogin.body.csrfToken === 'string' && goodLogin.body.csrfToken.length >= 32, `CSRF token minted`);

    csrfToken = goodLogin.body.csrfToken;
    const cookieHeader = goodLogin.headers['set-cookie'];
    assert(Array.isArray(cookieHeader) && cookieHeader.some(c => c.includes('hg_session=') && c.includes('HttpOnly') && c.includes('SameSite=Strict')),
      `HttpOnly, SameSite=Strict session cookie issued`);
    sessionCookie = cookieHeader[0].split(';')[0];

    const sessRes = await request('GET', '/api/auth/session', { Cookie: sessionCookie });
    assert(sessRes.status === 200 && sessRes.body.authenticated === true, `GET /api/auth/session confirmed active operator session`);

    // -----------------------------------------------------------------
    // TEST 4b: P0-1 Browser Origin Authorization (CORS Exact Port Enforcement)
    // -----------------------------------------------------------------
    console.log('\n[Test 4b] P0-1 Browser Origin Authorization & Strict Port Matching');
    // Mismatched loopback port on OPTIONS preflight
    const hostileOptions = await request('OPTIONS', '/api/auth/session', {
      Origin: 'http://127.0.0.1:9999',
    });
    assert(hostileOptions.status === 403, `OPTIONS from different-port origin (http://127.0.0.1:9999) rejected HTTP 403`);
    assert(!hostileOptions.headers['access-control-allow-origin'], `Zero Access-Control-Allow-Origin header returned on rejected origin`);

    // Mismatched loopback port on authenticated GET
    const hostileGet = await request('GET', '/api/auth/session', {
      Cookie: sessionCookie,
      Origin: 'http://127.0.0.1:9999',
    });
    assert(hostileGet.status === 403, `GET /api/auth/session from different-port origin rejected HTTP 403`);
    assert(hostileGet.body?.error === 'forbidden_origin', `Body reports error: forbidden_origin`);
    assert(!hostileGet.headers['access-control-allow-origin'], `Zero Access-Control-Allow-Origin header returned on rejected GET`);

    // Mismatched localhost port on mutation POST
    const hostilePost = await request('POST', '/api/devices/camera-syn-001/consent', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://localhost:3000',
    }, { granted: true });
    assert(hostilePost.status === 403, `POST /api/devices/... from http://localhost:3000 rejected HTTP 403`);
    assert(!hostilePost.headers['access-control-allow-origin'], `Zero Access-Control-Allow-Origin header on rejected mutation`);

    // Exact matching origin: accepted with ACAO matching exact origin and credentials enabled
    const genuineOrigin = `http://127.0.0.1:${WEB_PORT}`;
    const genuineOptions = await request('OPTIONS', '/api/auth/session', {
      Origin: genuineOrigin,
    });
    assert(genuineOptions.status === 204, `OPTIONS from exact listener origin returned HTTP 204`);
    assert(genuineOptions.headers['access-control-allow-origin'] === genuineOrigin, `ACAO header matches exact origin`);
    assert(genuineOptions.headers['access-control-allow-credentials'] === 'true', `Access-Control-Allow-Credentials is true`);

    // -----------------------------------------------------------------
    // TEST 5: Peripheral Device Registry (Authenticated GET /api/devices)
    // -----------------------------------------------------------------
    console.log('\n[Test 5] Authoritative C++ Device Registry (GET /api/devices)');
    const devRes = await request('GET', '/api/devices', { Cookie: sessionCookie });
    assert(devRes.status === 200, `GET /api/devices returned HTTP 200 with session`);
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
    assert(cam1.has_credential === false, `cam1 has_credential is false (unprovisioned physical candidate)`);
    assert(cam1.paired === true, `cam1 paired is true (registered device candidate)`);
    assert(cam1.credential_id === undefined, `cam1 credential_id is NOT disclosed in public listing`);
    assert(cam1.auth_secret === undefined, `cam1 auth_secret is NOT disclosed in public listing`);
    for (const dev of devRes.body) {
      assert(dev.credential_id === undefined, `Device ${dev.id} credential_id not disclosed in registry payload`);
      assert(dev.auth_secret === undefined, `Device ${dev.id} auth_secret not disclosed in registry payload`);
    }

    const synCam = devRes.body.find((d) => d.id === 'camera-syn-001');
    assert(!!synCam, `Found synthetic test camera 'camera-syn-001'`);
    assert(synCam.is_synthetic === true, `Synthetic provider explicitly flagged (is_synthetic: true)`);

    // -----------------------------------------------------------------
    // TEST 6: State Mutation Security Gate (Requirement 5)
    // -----------------------------------------------------------------
    console.log('\n[Test 6] State Mutation Security Gate (Scenario & Consent)');
    const badCsrfScenario = await request('POST', '/api/mcp/scenario', {
      Cookie: sessionCookie,
      'X-CSRF-Token': 'wrong-csrf-token-12345',
    }, { scenario: 'critical_fall' });
    assert(badCsrfScenario.status === 403, `Mutation with invalid CSRF token rejected HTTP 403`);

    const goodScenario = await request('POST', '/api/mcp/scenario', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { scenario: 'critical_fall' });
    assert(goodScenario.status === 200, `Authenticated mutation with valid CSRF accepted HTTP 200`);
    assert(goodScenario.body.scenario === 'critical_fall', `Synthetic fall scenario executed safely`);

    // -----------------------------------------------------------------
    // TEST 7: Camera Consent & Scoped View Token Issuance
    // -----------------------------------------------------------------
    console.log('\n[Test 7] Live Camera Viewing Authorization Gate');
    const unconsentedView = await request('POST', '/api/devices/camera-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(unconsentedView.status === 403, `View token for unconsented camera refused with HTTP 403`);
    assert(unconsentedView.body.error === 'consent_withheld', `Error code: consent_withheld (Fail-Closed)`);

    const grantRes = await request('POST', '/api/devices/camera-001/consent', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { granted: true, purpose: 'testing' });
    assert(grantRes.status === 200, `Granted consent via C++ DeviceRegistry`);
    assert(grantRes.body.consent === 'granted', `Camera 1 consent is now 'granted'`);
    assert(grantRes.body.authorized === true, `Camera 1 is now authorized`);

    const tokenRes = await request('POST', '/api/devices/camera-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(tokenRes.status === 400, `View token for unverified physical camera refused with HTTP 400`);
    assert(String(tokenRes.body?.error).includes('physical_camera_unverified'),
      `Error code: physical_camera_unverified (judge build hardware protection)`);

    const micTokenRes = await request('POST', '/api/devices/mic-001/view-token', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    });
    assert(micTokenRes.status === 400, `View token for audio-only microphone refused HTTP 400 (capability_missing)`);

    // -----------------------------------------------------------------
    // HG-SEC-06: Device Secrets, CSPRNG Generation, Rotation & Revocation
    // -----------------------------------------------------------------
    console.log('\n[HG-SEC-06] Device Secret Lifecycle & CSPRNG Credential Hardening');

    // 1. Simulator restriction invariant
    let pairForgedSimFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-auth',
        'pair',
        JSON.stringify({
          device_id: 'cam-forged-001',
          display_name: 'Forged Sim',
          device_type: 'simulator',
          credential_id: 'forged-cert',
          has_camera: true,
          consent_granted: true,
          capture_enabled: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      pairForgedSimFailed = true;
    }
    assert(pairForgedSimFailed, `Pairing device_type simulator for non-authoritative id rejected`);

    // 2. Non-allowlisted device type rejected
    let pairUnknownTypeFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-auth',
        'pair',
        JSON.stringify({
          device_id: 'cam-bad-001',
          display_name: 'Bad Type',
          device_type: 'Simulator',
          credential_id: 'bad-cert',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      pairUnknownTypeFailed = true;
    }
    assert(pairUnknownTypeFailed, `Pairing non-allowlisted device_type rejected`);

    // 3. Reconcile all 5 historical credential literals: verify every one is rejected (HG-SEC-06B Action 2)
    const kFivePublishedLiterals = [
      'sec_cam1_7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c',
      'sec_syn_1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d',
      'sec_mic1_9e8d7c6b5a4f3e2d1c0b9a8f7e6d5c4b',
      'sec_spk1_5a6b7c8d9e0f1a2b3c4d5e6f7a8b9c0d',
      'sec_env1_3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f',
    ];
    for (const lit of kFivePublishedLiterals) {
      let litPairFailed = false;
      try {
        execFileSync(cliPath, [
          '--registry', configPath,
          '--operator-auth',
          'pair',
          JSON.stringify({
            device_id: 'cam-lit-test',
            display_name: 'Historical Literal Test Camera',
            device_type: 'camera',
            auth_secret: lit,
            has_camera: true,
            capabilities: ['camera'],
          }),
        ], {
          env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
        });
      } catch (e) {
        litPairFailed = true;
      }
      assert(litPairFailed, `Pairing with historical credential literal '${lit.slice(0, 12)}...' rejected`);

      let litHbFailed = false;
      try {
        execFileSync(cliPath, [
          '--registry', configPath, 'heartbeat', 'cam-live-001', '1', String(Date.now()),
        ], {
          env: { ...process.env, HG_DEVICE_SECRET: lit },
        });
      } catch (e) {
        litHbFailed = true;
      }
      assert(litHbFailed, `Heartbeat presenting historical credential literal '${lit.slice(0, 12)}...' rejected`);
    }

    // 3b. HG-SEC-06C Task A: Initial Enrollment Authorization Enforcement
    // Negative test: Unauthenticated local caller cannot claim unprovisioned seeded camera (camera-001)
    let unauthClaimSeededFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'pair',
        JSON.stringify({
          device_id: 'camera-001',
          display_name: 'Hijacked camera-001',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ]);
    } catch (e) {
      unauthClaimSeededFailed = true;
    }
    assert(unauthClaimSeededFailed, `Claiming unprovisioned seeded camera without operator auth rejected (HG-SEC-06C Task A)`);

    // Negative test: Unauthenticated local caller cannot enroll a new device
    let unauthEnrollNewFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'pair',
        JSON.stringify({
          device_id: 'cam-live-001',
          display_name: 'Android Live Physical Camera',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ]);
    } catch (e) {
      unauthEnrollNewFailed = true;
    }
    assert(unauthEnrollNewFailed, `Initial device enrollment without operator auth rejected (HG-SEC-06C Task A)`);

    // Negative test: Operator secret file with insecure permissions (0644) rejected (HG-SEC-06C Task C)
    const insecureSecretFile = path.join(path.dirname(configPath), 'test_operator_insecure.secret');
    fs.writeFileSync(insecureSecretFile, 'trusted-operator-secret-2026\n', { mode: 0o644 });
    let insecureSecretFileFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-auth',
        '--operator-secret-file', insecureSecretFile,
        'pair',
        JSON.stringify({
          device_id: 'cam-live-001',
          display_name: 'Android Live Physical Camera',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      insecureSecretFileFailed = true;
    }
    assert(insecureSecretFileFailed, `Operator secret file with insecure permissions (0644) rejected (HG-SEC-06C Task C)`);
    try { fs.unlinkSync(insecureSecretFile); } catch (_) {}

    // Positive test: Operator secret file with strict 0600 permissions accepted
    const secureSecretFile = path.join(path.dirname(configPath), 'test_operator_secure.secret');
    fs.writeFileSync(secureSecretFile, 'trusted-operator-secret-2026\n', { mode: 0o600 });

    // 4. Secure physical device pairing with CSPRNG secret generation via trusted operator secret file
    const pairRaw = execFileSync(cliPath, [
      '--registry', configPath,
      '--operator-auth',
      '--operator-secret-file', secureSecretFile,
      'pair',
      JSON.stringify({
        device_id: 'cam-live-001',
        display_name: 'Android Live Physical Camera',
        device_type: 'camera',
        has_camera: true,
        consent_granted: true,
        capture_enabled: true,
        capabilities: ['camera', 'microphone'],
      }),
    ], {
      env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
    }).toString('utf-8');
    try { fs.unlinkSync(secureSecretFile); } catch (_) {}
    const pairResult = JSON.parse(pairRaw);
    assert(pairResult.id === 'cam-live-001', `Explicit pairing created cam-live-001`);
    assert(typeof pairResult.provisioned_auth_secret === 'string', `Pairing returned provisioned_auth_secret`);
    assert(pairResult.provisioned_auth_secret.startsWith('sec_live_'), `Secret has sec_live_ prefix`);
    assert(pairResult.provisioned_auth_secret.length === 73, `Secret is 256-bit CSPRNG hex (73 chars with prefix)`);
    const liveSecret = pairResult.provisioned_auth_secret;

    // 5. Query registry over public API: secret must NEVER be disclosed in get or list
    const devListAfterPair = await request('GET', '/api/devices', { Cookie: sessionCookie });
    const liveDev = devListAfterPair.body.find(d => d.id === 'cam-live-001');
    assert(!!liveDev, `Found newly paired device cam-live-001 in registry`);
    assert(liveDev.has_credential === true, `cam-live-001 has_credential is true`);
    assert(liveDev.credential_id === undefined, `cam-live-001 credential_id is NOT disclosed in public API`);
    assert(liveDev.auth_secret === undefined, `cam-live-001 auth_secret is NOT disclosed in public API`);
    assert(liveDev.provisioned_auth_secret === undefined, `cam-live-001 provisioned_auth_secret is NOT disclosed in public API`);

    // 6. Hard gate: Reject heartbeat presenting public credential_id
    let replayHbFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-live-001', '1', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_CREDENTIAL: liveDev.id },
      });
    } catch (e) {
      replayHbFailed = true;
    }
    assert(replayHbFailed, `Heartbeat presenting public credential_id rejected`);

    // 7. Hard gate: Reject heartbeat presenting blacklisted secret
    let blacklistedHbFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-live-001', '1', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: 'sec_cam1_7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c' },
      });
    } catch (e) {
      blacklistedHbFailed = true;
    }
    assert(blacklistedHbFailed, `Heartbeat presenting known published/blacklisted secret rejected`);

    // 8. Authenticated heartbeat with valid CSPRNG secret
    const hbSuccessRaw = execFileSync(cliPath, [
      '--registry', configPath, 'heartbeat', 'cam-live-001', '1', String(Date.now()),
    ], {
      env: { ...process.env, HG_DEVICE_SECRET: liveSecret },
    }).toString('utf-8');
    const hbSuccess = JSON.parse(hbSuccessRaw);
    assert(hbSuccess.online === true, `Heartbeat with valid CSPRNG secret sets online: true`);
    assert(hbSuccess.last_seen !== null, `Heartbeat records ISO timestamp in last_seen`);

    // 9. Secret rotation (CLI command: rotate_secret <id>)
    const rotateRaw = execFileSync(cliPath, [
      '--registry', configPath, 'rotate_secret', 'cam-live-001',
    ]).toString('utf-8');
    const rotateResult = JSON.parse(rotateRaw);
    assert(typeof rotateResult.rotated_auth_secret === 'string', `Secret rotation issued rotated_auth_secret`);
    assert(rotateResult.rotated_auth_secret !== liveSecret, `Rotated secret differs from old secret`);
    assert(rotateResult.online === false, `Device transitioned to offline pending heartbeat with new secret`);
    const rotatedSecret = rotateResult.rotated_auth_secret;

    // 10. Old secret is immediately revoked upon rotation
    let oldSecretHbFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-live-001', '2', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: liveSecret },
      });
    } catch (e) {
      oldSecretHbFailed = true;
    }
    assert(oldSecretHbFailed, `Heartbeat presenting old pre-rotation secret rejected`);

    // 11. Heartbeat with rotated secret succeeds
    const hbRotatedRaw = execFileSync(cliPath, [
      '--registry', configPath, 'heartbeat', 'cam-live-001', '2', String(Date.now()),
    ], {
      env: { ...process.env, HG_DEVICE_SECRET: rotatedSecret },
    }).toString('utf-8');
    const hbRotated = JSON.parse(hbRotatedRaw);
    assert(hbRotated.online === true, `Heartbeat with rotated secret successfully restores online: true`);

    // 12. Revocation of paired device
    const revokeRaw = execFileSync(cliPath, [
      '--registry', configPath, 'revoke', 'cam-live-001', 'security audit revocation',
    ]).toString('utf-8');
    const revokeResult = JSON.parse(revokeRaw);
    assert(revokeResult.revoked === true, `cam-live-001 revoked: true`);

    // 12b. HG-SEC-06C Task B: Revoked device reclaim lifecycle protection
    // Negative test: Revoked device CANNOT simply be reclaimed through ordinary unauthenticated pairing
    let revokedReclaimUnauthFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'pair',
        JSON.stringify({
          device_id: 'cam-live-001',
          display_name: 'Reclaimed Revoked Camera',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ]);
    } catch (e) {
      revokedReclaimUnauthFailed = true;
    }
    assert(revokedReclaimUnauthFailed, `Reclaiming revoked device via ordinary unauthenticated pairing rejected (HG-SEC-06C Task B)`);

    // Negative test: Revoked device CANNOT be reclaimed with ordinary operator auth without recovery flag
    let revokedReclaimWithoutRecoveryFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-auth',
        'pair',
        JSON.stringify({
          device_id: 'cam-live-001',
          display_name: 'Reclaimed Revoked Camera',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      revokedReclaimWithoutRecoveryFailed = true;
    }
    assert(revokedReclaimWithoutRecoveryFailed, `Reclaiming revoked device requires operator-recovery, not simple operator-auth (HG-SEC-06C Task B)`);

    // 13. Revoked device heartbeat is unconditionally rejected
    let revokedHbFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-live-001', '3', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: rotatedSecret },
      });
    } catch (e) {
      revokedHbFailed = true;
    }
    assert(revokedHbFailed, `Heartbeat for revoked device unconditionally rejected`);

    // 14. Strict registry file permissions (0600) and directory permissions (0700)
    const fileStat = fs.statSync(configPath);
    const modeOctal = (fileStat.mode & 0o777).toString(8);
    assert(modeOctal === '600', `Registry file permissions enforced at 0600 (owner-only read/write, got 0${modeOctal})`);

    const dirStat = fs.statSync(path.dirname(configPath));
    const dirModeOctal = (dirStat.mode & 0o777).toString(8);
    assert(dirModeOctal === '700', `Registry parent directory permissions enforced at 0700 (got 0${dirModeOctal})`);

    // 15. Fix 3: Reject arbitrary client-supplied auth_secret during pairing
    let clientSuppliedSecretFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-auth',
        'pair',
        JSON.stringify({
          device_id: 'cam-fake-secret-001',
          display_name: 'Fake Secret Cam',
          device_type: 'camera',
          auth_secret: 'attacker_supplied_credential_value_123',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      clientSuppliedSecretFailed = true;
    }
    assert(clientSuppliedSecretFailed, `Pairing with arbitrary client-supplied auth_secret rejected (Fix 3)`);

    // 16. Fix 3: Initial enrollment vs unauthorized re-pairing
    const enrollRaw = execFileSync(cliPath, [
      '--registry', configPath,
      '--operator-auth',
      'pair',
      JSON.stringify({
        device_id: 'cam-re-001',
        display_name: 'Re-pairable Camera',
        device_type: 'camera',
        has_camera: true,
        capabilities: ['camera'],
      }),
    ], {
      env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
    }).toString('utf-8');
    const enrollResult = JSON.parse(enrollRaw);
    const secretA = enrollResult.provisioned_auth_secret;
    assert(typeof secretA === 'string' && secretA.startsWith('sec_live_'), `Initial enrollment provisioned secretA`);

    // Prior to re-pairing, grant consent to cam-re-001 to verify it gets invalidated on re-pairing
    execFileSync(cliPath, ['--registry', configPath, 'set_consent', 'cam-re-001', 'true', 'operator', 'test_consent']);
    const preDevRaw = execFileSync(cliPath, ['--registry', configPath, 'get', 'cam-re-001']).toString('utf-8');
    assert(JSON.parse(preDevRaw).consent === 'granted', `Consent was granted to cam-re-001 prior to re-pairing`);

    // Unauthorized re-pairing attempt (without existing credential proof or operator recovery)
    let unauthRePairFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'pair',
        JSON.stringify({
          device_id: 'cam-re-001',
          display_name: 'Hijacked Camera',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ]);
    } catch (e) {
      unauthRePairFailed = true;
    }
    assert(unauthRePairFailed, `Unauthorized re-pairing of enrolled device rejected (Fix 3)`);

    // Authorized re-pairing with proof of current secret (existing_auth_secret)
    const authRePairRaw = execFileSync(cliPath, [
      '--registry', configPath, 'pair',
      JSON.stringify({
        device_id: 'cam-re-001',
        display_name: 'Re-paired Camera',
        device_type: 'camera',
        existing_auth_secret: secretA,
        has_camera: true,
        capabilities: ['camera'],
      }),
    ]).toString('utf-8');
    const authRePairResult = JSON.parse(authRePairRaw);
    const secretB = authRePairResult.provisioned_auth_secret;
    assert(typeof secretB === 'string' && secretB !== secretA, `Authorized re-pairing with proof minted secretB`);
    assert(authRePairResult.consent === 'not_granted', `Re-pairing invalidated prior consent (HG-SEC-06C Task B)`);
    assert(authRePairResult.authorized === false, `Re-pairing invalidated prior viewing grant (HG-SEC-06C Task B)`);
    assert(authRePairResult.online === false, `Re-pairing reset online state to false (HG-SEC-06C Task B)`);

    // HG-SEC-06B Action 1: Reject caller-supplied operator_recovery boolean from JSON
    let callerBooleanRecoveryFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'pair',
        JSON.stringify({
          device_id: 'cam-re-001',
          display_name: 'Hijacked via JSON boolean',
          device_type: 'camera',
          operator_recovery: true,
          has_camera: true,
          capabilities: ['camera'],
        }),
      ]);
    } catch (e) {
      callerBooleanRecoveryFailed = true;
    }
    assert(callerBooleanRecoveryFailed, `Caller-supplied operator_recovery: true in JSON without trusted operator auth is rejected (HG-SEC-06B Action 1)`);

    // Recovery flag present but no operator secret configured / invalid secret: rejected
    let unauthenticatedRecoveryFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath,
        '--operator-recovery',
        '--operator-secret', 'wrong-operator-secret',
        'pair',
        JSON.stringify({
          device_id: 'cam-re-001',
          display_name: 'Unauth Recovery Attempt',
          device_type: 'camera',
          has_camera: true,
          capabilities: ['camera'],
        }),
      ], {
        env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
      });
    } catch (e) {
      unauthenticatedRecoveryFailed = true;
    }
    assert(unauthenticatedRecoveryFailed, `Operator recovery with invalid operator secret is rejected (HG-SEC-06B Action 1)`);

    // Operator-authorized recovery re-pairing with trusted operator secret
    const recoveryRePairRaw = execFileSync(cliPath, [
      '--registry', configPath,
      '--operator-recovery',
      '--operator-secret', 'trusted-operator-secret-2026',
      'pair',
      JSON.stringify({
        device_id: 'cam-re-001',
        display_name: 'Recovered Camera',
        device_type: 'camera',
        has_camera: true,
        capabilities: ['camera'],
      }),
    ], {
      env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
    }).toString('utf-8');
    const recoveryResult = JSON.parse(recoveryRePairRaw);
    const secretC = recoveryResult.provisioned_auth_secret;
    assert(typeof secretC === 'string' && secretC !== secretB, `Trusted operator recovery re-pairing minted secretC`);
    assert(recoveryResult.online === false, `Recovered device remains offline until authenticated heartbeat`);
    assert(recoveryResult.consent === 'not_granted', `Recovery invalidated prior consent (HG-SEC-06C Task B)`);

    // 17. Fix 4: Heartbeat sequence validation and replay protection
    const seqDevRaw = execFileSync(cliPath, [
      '--registry', configPath,
      '--operator-auth',
      'pair',
      JSON.stringify({
        device_id: 'cam-seq-001',
        display_name: 'Sequence Protected Camera',
        device_type: 'camera',
        has_camera: true,
        capabilities: ['camera'],
      }),
    ], {
      env: { ...process.env, HG_OPERATOR_SECRET: 'trusted-operator-secret-2026' },
    }).toString('utf-8');
    const seqDevResult = JSON.parse(seqDevRaw);
    const seqSecret = seqDevResult.provisioned_auth_secret;

    // First heartbeat with sequence 1 succeeds
    const hbSeq1Raw = execFileSync(cliPath, [
      '--registry', configPath, 'heartbeat', 'cam-seq-001', '1', String(Date.now()),
    ], {
      env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
    }).toString('utf-8');
    assert(JSON.parse(hbSeq1Raw).online === true, `Heartbeat sequence 1 accepted`);

    // Duplicate heartbeat with identical sequence 1 MUST BE REJECTED (Replay attack blocked)
    let duplicateSeqFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-seq-001', '1', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
      });
    } catch (e) {
      duplicateSeqFailed = true;
    }
    assert(duplicateSeqFailed, `Duplicate heartbeat sequence 1 rejected (Replay protection enforced)`);

    // Older sequence number (sequence 0 <= 1) MUST BE REJECTED
    let olderSeqFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-seq-001', '0', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
      });
    } catch (e) {
      olderSeqFailed = true;
    }
    assert(olderSeqFailed, `Older heartbeat sequence 0 rejected (Monotonicity enforced)`);

    // Malformed sequence number MUST BE REJECTED
    let malformedSeqFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-seq-001', 'invalid_seq_NaN', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
      });
    } catch (e) {
      malformedSeqFailed = true;
    }
    assert(malformedSeqFailed, `Malformed heartbeat sequence rejected`);

    // Monotonically advanced sequence 5 succeeds
    const hbSeq5Raw = execFileSync(cliPath, [
      '--registry', configPath, 'heartbeat', 'cam-seq-001', '5', String(Date.now()),
    ], {
      env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
    }).toString('utf-8');
    assert(JSON.parse(hbSeq5Raw).online === true, `Heartbeat sequence 5 accepted`);

    // 18. Fix 4: Replay protection survives application/process restart
    // Simulating process restart by running an entirely new CLI invocation against the persisted registry file
    let restartReplayFailed = false;
    try {
      execFileSync(cliPath, [
        '--registry', configPath, 'heartbeat', 'cam-seq-001', '5', String(Date.now()),
      ], {
        env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
      });
    } catch (e) {
      restartReplayFailed = true;
    }
    assert(restartReplayFailed, `Old sequence 5 rejected after restart (Watermark persisted on disk)`);

    // Fresh sequence 6 succeeds after restart
    const hbSeq6Raw = execFileSync(cliPath, [
      '--registry', configPath, 'heartbeat', 'cam-seq-001', '6', String(Date.now()),
    ], {
      env: { ...process.env, HG_DEVICE_SECRET: seqSecret },
    }).toString('utf-8');
    assert(JSON.parse(hbSeq6Raw).online === true, `Monotonically higher sequence 6 succeeds after restart`);

    // -----------------------------------------------------------------
    // TEST 8: WebRTC Signaling & Connection
    // -----------------------------------------------------------------
    console.log('\n[Test 8] WebRTC Signaling & Synthetic Media Connection');
    const badSignal = await request('POST', '/api/webrtc/signal', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://127.0.0.1:18880',
    }, { deviceId: 'camera-syn-001', viewToken: 'invalid-vt', sdp: {} });
    assert(badSignal.status === 400, `WebRTC signaling with invalid viewToken rejected HTTP 400`);

    const offlineSignal = await request('POST', '/api/webrtc/signal', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
      Origin: 'http://127.0.0.1:18880',
    }, { deviceId: 'camera-001', viewToken: 'vt_dummy', sdp: {} });
    assert(offlineSignal.status === 400 && (String(offlineSignal.body?.error).includes('physical_camera_unverified') || String(offlineSignal.body?.error).includes('device_offline')),
      `Physical camera streaming rejected HTTP 400 (hardware stream unverified/offline)`);

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

    const streamsRes = await request('GET', '/api/webrtc/streams', {
      Cookie: sessionCookie,
    });
    assert(streamsRes.status === 200 && streamsRes.body.length >= 1, `GET /api/webrtc/streams lists active session`);

    // -----------------------------------------------------------------
    // TEST 9: WebRTC Stream Ownership: Cross-Operator Stop Refusal
    // -----------------------------------------------------------------
    console.log('\n[Test 9] WebRTC Stream Ownership: Cross-Operator Tampering Refusal');
    // Login as a second operator session
    const opBLogin = await request('POST', '/api/auth/login', {}, { username: 'test-operator', password: 'SecurePassword2026!' });
    assert(opBLogin.status === 200, `Operator B logged in with independent session`);
    const opBCookie = opBLogin.headers['set-cookie'][0].split(';')[0];
    const opBCsrf = opBLogin.body.csrfToken;

    // Operator B attempts to stop Operator A's active stream
    const crossStop = await request('POST', '/api/webrtc/stop', {
      Cookie: opBCookie,
      'X-CSRF-Token': opBCsrf,
    }, { deviceId: 'camera-syn-001' });
    assert(crossStop.status === 403, `Cross-operator stop attempt rejected with HTTP 403 Forbidden`);
    assert(String(crossStop.body?.error).includes('forbidden'),
      `Explicit error: forbidden (cross-session stream tampering refused)`);

    // Operator A stops their own stream
    const stopRes = await request('POST', '/api/webrtc/stop', {
      Cookie: sessionCookie,
      'X-CSRF-Token': csrfToken,
    }, { deviceId: 'camera-syn-001' });
    assert(stopRes.status === 200 && stopRes.body.stopped === true, `Stream stopped cleanly by owning operator`);
    clientPc.close();

    // -----------------------------------------------------------------
    // TEST 10: Preserved MCP Conversational Functionality (Authenticated)
    // -----------------------------------------------------------------
    console.log('\n[Test 10] Preserved Native MCP Handshake & Tools');
    const mcpSess = await request('GET', '/api/mcp/session', { Cookie: sessionCookie });
    assert(mcpSess.status === 200, `MCP session endpoint active HTTP 200`);
    assert(mcpSess.body.connected === true, `Connected to native C++ hg_mcp_server`);
    assert(mcpSess.body.protocolVersion === '2025-11-25', `Protocol version is 2025-11-25`);

    const chatRes = await request('POST', '/api/mcp/chat', { Cookie: sessionCookie }, { query: 'Is everything okay?' });
    assert(chatRes.status === 200, `MCP chat returned HTTP 200`);
    assert(chatRes.body.toolInvoked === 'get_homeguardian_status', `Invoked MCP tool 'get_homeguardian_status'`);
    assert(chatRes.body.conversationalResponse.includes('Running'), `Conversational speech generated dynamically`);

    // -----------------------------------------------------------------
    // TEST 11: Session Logout & Invalidation Teardown
    // -----------------------------------------------------------------
    console.log('\n[Test 11] Operator Logout & Teardown Verification');
    const logoutRes = await request('POST', '/api/auth/logout', { Cookie: sessionCookie });
    assert(logoutRes.status === 200 && logoutRes.body.ok === true, `POST /api/auth/logout succeeded`);

    const postLogoutDevices = await request('GET', '/api/devices', { Cookie: sessionCookie });
    assert(postLogoutDevices.status === 401, `Destroyed session rejected HTTP 401 on subsequent calls`);

    console.log(`\n=======================================================`);
    console.log(`HG-UI-03 & RC2 RESULTS: ${passed} PASSED, ${failed} FAILED`);
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
