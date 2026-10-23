const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const http = require('node:http');
const { once } = require('node:events');
const { WebSocket } = require('ws');
const { McpBridgeCoordinator } = require('../web_mcp/outbound_bridge/coordinator');

function socket(coordinator) {
  const ws = new EventEmitter();
  ws.readyState = 1;
  ws.sent = [];
  ws.send = raw => ws.sent.push(JSON.parse(raw));
  // Deliberately delay close event to exercise queued frames during teardown.
  ws.close = code => { ws.closeCode = code; ws.readyState = 2; };
  coordinator._handleEdgeConnection(ws, { socket: { remoteAddress: '127.0.0.1' } });
  return ws;
}

function auth(ws, token = 'secret') {
  ws.emit('message', Buffer.from(JSON.stringify({ type: 'bridge_auth', device_id: 'edge', auth_token: token })));
}

async function request(c, body, headers = {}, url = '/mcp', method = 'POST', response = {}) {
  const req = new EventEmitter();
  Object.assign(req, { url, method, headers: { host: 'localhost', authorization: 'Bearer client', 'content-type': 'application/json', ...headers } });
  const res = Object.assign(response, { writeHead(status, headers) { this.status = status; this.headers = headers; }, end(raw) { this.raw = raw; this.writableEnded = true; } });
  await c._handleClientHttp(req, res);
  if (!res.writableEnded) {
    req.emit('data', Buffer.from(JSON.stringify(body)));
    // Await the actual async callback so a rejected handler fails this test.
    await req.listeners('end')[0]();
  }
  return res;
}

test('malformed HTTP targets and non-object JSON are contained', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client' });
  const malformed = await request(c, {}, { host: '[' });
  assert.equal(malformed.status, 400);
  for (const body of [null, [], 1, 'text']) {
    assert.equal((await request(c, body)).status, 400);
  }
});

test('malformed WebSocket JSON and credential types are contained', () => {
  const c = new McpBridgeCoordinator();
  c.authorizeDevice('edge', 'secret');
  for (const body of [null, [], 1, { type: 'bridge_auth', device_id: 'edge', auth_token: {} }]) {
    const ws = socket(c);
    try {
      assert.doesNotThrow(() => ws.emit('message', Buffer.from(JSON.stringify(body))));
      assert.ok(ws.closeCode >= 4400);
    } finally { ws.emit('close'); }
  }
});

test('rotation immediately invalidates the established channel and pending requests', async () => {
  const c = new McpBridgeCoordinator();
  c.authorizeDevice('edge', 'secret');
  const ws = socket(c);
  auth(ws);
  const pending = c.dispatchMcpRequest({ jsonrpc: '2.0', id: 1, method: 'ping' });
  const rejected = assert.rejects(pending, /EDGE_OFFLINE/);
  c.authorizeDevice('edge', 'rotated');
  try {
    assert.equal(c.activeEdge, null);
    assert.equal(ws.closeCode, 4403);
    assert.equal(c.pendingRequests.size, 0);
    await rejected;
  } finally { ws.emit('close'); }
});

test('superseded or revoked sockets cannot heartbeat or resolve requests', () => {
  const c = new McpBridgeCoordinator();
  c.authorizeDevice('edge', 'secret');
  const old = socket(c);
  auth(old);
  const current = socket(c);
  auth(current);
  let resolved = false;
  const timer = setTimeout(() => {}, 1000);
  c.pendingRequests.set('pending', { timer, resolve() { resolved = true; }, reject() {} });
  try {
    c.activeEdge.lastSeen = 1;
    old.emit('message', Buffer.from(JSON.stringify({ type: 'heartbeat_ping' })));
    assert.equal(c.activeEdge.lastSeen, 1);
    old.emit('message', Buffer.from(JSON.stringify({ type: 'mcp_response', correlation_id: 'pending' })));
    assert.equal(resolved, false);
    // Registry revocation must be checked at frame time, even before close delivery.
    c.authorizedDevices.get('edge').revoked = true;
    current.emit('message', Buffer.from(JSON.stringify({ type: 'mcp_response', correlation_id: 'pending' })));
    assert.equal(resolved, false);
    assert.equal(c.activeEdge, null);
  } finally { clearTimeout(timer); old.emit('close'); current.emit('close'); }
});

test('failed initializes never retain or advertise client sessions', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client' });
  const init = { jsonrpc: '2.0', id: 1, method: 'initialize' };
  const offline = await request(c, init);
  assert.equal(offline.status, 503);
  assert.equal(c.clientSessions.size, 0);
  for (const reply of [
    { statusCode: 500, body: { error: 'failure' } },
    { statusCode: 200, body: { jsonrpc: '2.0', id: 1, error: { code: -32602, message: 'bad params' } } },
  ]) {
    c.dispatchMcpRequest = async () => reply;
    const res = await request(c, init);
    assert.equal(c.clientSessions.size, 0);
    assert.equal(res.headers['Mcp-Session-Id'], undefined);
  }
});

test('disconnected initializing clients do not leave orphan sessions', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client' });
  const response = {};
  c.dispatchMcpRequest = async () => {
    response.destroyed = true;
    return { statusCode: 200, body: { result: {} } };
  };
  await request(c, { method: 'initialize' }, {}, '/mcp', 'POST', response);
  assert.equal(c.clientSessions.size, 0);
  assert.equal(c.initializingSessions, 0);
  assert.equal(response.headers, undefined);
});

test('client session capacity includes in-flight initialize reservations', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client', maxClientSessions: 1 });
  let complete;
  let calls = 0;
  c.dispatchMcpRequest = () => ++calls === 1
    ? new Promise(resolve => { complete = resolve; })
    : Promise.resolve({ statusCode: 200, body: { result: {} } });
  const init = { jsonrpc: '2.0', id: 1, method: 'initialize' };
  const first = request(c, init);
  await new Promise(resolve => setImmediate(resolve));
  const second = await request(c, init);
  assert.equal(second.status, 503);
  complete({ statusCode: 200, body: { jsonrpc: '2.0', id: 1, result: {} } });
  const success = await first;
  assert.ok(success.headers['Mcp-Session-Id']);
  assert.equal(c.clientSessions.size, 1);
  assert.equal((await request(c, init)).status, 503);
});

test('expired sessions are rejected for POST and DELETE and capacity is reclaimed', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client', maxClientSessions: 1, clientSessionTtlMs: 10 });
  c.clientSessions.set('expired', { createdAt: 1, lastSeen: 1 });
  assert.equal((await request(c, { method: 'ping' }, { 'mcp-session-id': 'expired' })).status, 404);
  c.clientSessions.set('expired', { createdAt: 1, lastSeen: 1 });
  assert.equal((await request(c, null, { 'mcp-session-id': 'expired' }, '/mcp', 'DELETE')).status, 404);
  c.clientSessions.set('expired', { createdAt: 1, lastSeen: 1 });
  c.dispatchMcpRequest = async () => ({ statusCode: 200, body: { result: {} } });
  assert.equal((await request(c, { method: 'initialize' })).status, 200);
  assert.equal(c.clientSessions.size, 1);
});

test('session limits have finite defaults and reject unbounded options', () => {
  const c = new McpBridgeCoordinator();
  assert.equal(c.maxClientSessions, 1024);
  assert.equal(c.clientSessionTtlMs, 30 * 60 * 1000);
  for (const value of [0, -1, Infinity, NaN, '1', 1.5]) {
    assert.throws(() => new McpBridgeCoordinator({ maxClientSessions: value }), /maxClientSessions/);
    assert.throws(() => new McpBridgeCoordinator({ clientSessionTtlMs: value }), /clientSessionTtlMs/);
  }
});

test('HTTP body size rejection cannot dispatch on end or write headers twice', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client' });
  let dispatched = false;
  c.dispatchMcpRequest = async () => { dispatched = true; return { statusCode: 200, body: { result: {} } }; };
  const req = new EventEmitter();
  Object.assign(req, { url: '/mcp', method: 'POST', headers: { authorization: 'Bearer client', 'content-type': 'application/json' }, destroy() {} });
  let writes = 0;
  const res = { writeHead() { writes++; }, end() { this.writableEnded = true; } };
  await c._handleClientHttp(req, res);
  req.emit('data', Buffer.from(JSON.stringify({ method: 'initialize', padding: 'x'.repeat(70000) })));
  req.emit('data', Buffer.from(' '));
  await req.listeners('end')[0]();
  assert.equal(dispatched, false);
  assert.equal(writes, 1);
  assert.equal(c.clientSessions.size, 0);
});

test('HTTP body callback contains unexpected asynchronous failures', async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client' });
  // First pruning occurs at ingress, second inside the async end callback.
  let calls = 0;
  c._pruneClientSessions = () => { if (++calls === 2) throw new Error('body callback failure'); };
  const res = await request(c, { method: 'initialize' });
  assert.equal(res.status, 500);
  assert.equal(c.clientSessions.size, 0);
});

test('real loopback HTTP/WS remains usable after malformed input, reconnect and rotation', { timeout: 10000 }, async () => {
  const c = new McpBridgeCoordinator({ clientToken: 'client', maxClientSessions: 2 });
  c.wsPort = c.httpPort = 0;
  c.authorizeDevice('edge', 'secret');
  const sockets = [];
  await c.start();
  const open = async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${c.wss.address().port}`);
    sockets.push(ws);
    await once(ws, 'open');
    return ws;
  };
  const handshake = async token => {
    const ws = await open();
    const received = once(ws, 'message');
    ws.send(JSON.stringify({ type: 'bridge_auth', device_id: 'edge', auth_token: token }));
    return { ws, reply: JSON.parse((await received)[0]) };
  };
  const post = (body, headers = {}, method = 'POST', path = '/mcp') => new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port: c.httpServer.address().port, path, method,
      headers: { authorization: 'Bearer client', 'content-type': 'application/json', ...headers } }, res => {
      let raw = '';
      res.on('data', data => { raw += data; });
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: JSON.parse(raw) }));
    });
    req.on('error', reject);
    req.end(method === 'POST' ? JSON.stringify(body) : undefined);
  });
  try {
    assert.equal((await post({}, { host: '[' })).status, 400);
    for (const body of [null, [], 1, 'text']) assert.equal((await post(body)).status, 400);
    for (const body of [null, [], { type: 'bridge_auth', device_id: 'edge', auth_token: {} }]) {
      const ws = await open();
      const closed = once(ws, 'close');
      ws.send(JSON.stringify(body));
      assert.ok((await closed)[0] >= 4400);
    }
    const first = await handshake('secret');
    assert.equal(first.reply.type, 'bridge_auth_ok');
    const firstClosed = once(first.ws, 'close');
    const second = await handshake('secret');
    assert.equal(second.reply.type, 'bridge_auth_ok');
    assert.equal((await firstClosed)[0], 1000);
    const session = c.activeEdge.sessionId;
    c.authorizeDevice('edge', 'secret', 'renamed');
    assert.equal(c.activeEdge.sessionId, session, 'unchanged credential reload preserves channel');
    second.ws.on('message', raw => {
      const frame = JSON.parse(raw);
      if (frame.type === 'mcp_request') second.ws.send(JSON.stringify({
        type: 'mcp_response', correlation_id: frame.correlation_id,
        status_code: 200, body: { jsonrpc: '2.0', id: frame.body.id, result: {} },
      }));
    });
    const init = await post({ jsonrpc: '2.0', id: 1, method: 'initialize' });
    assert.equal(init.status, 200);
    const id = init.headers['mcp-session-id'];
    assert.ok(id);
    assert.equal((await post({ jsonrpc: '2.0', id: 2, method: 'ping' }, { 'mcp-session-id': id })).status, 200);
    const secondClosed = once(second.ws, 'close');
    c.authorizeDevice('edge', 'rotated');
    assert.equal(c.isEdgeOnline(), false);
    assert.equal((await secondClosed)[0], 4403);
    const rejected = await handshake('secret');
    assert.equal(rejected.reply.type, 'bridge_auth_err');
    const replacement = await handshake('rotated');
    assert.equal(replacement.reply.type, 'bridge_auth_ok');
    // Invalid authenticated JSON must also be contained.
    const replacementClosed = once(replacement.ws, 'close');
    replacement.ws.send('null');
    assert.equal((await replacementClosed)[0], 4400);
    assert.equal((await post({}, {}, 'GET', '/health')).status, 200);
    assert.equal((await post(null, { 'mcp-session-id': id }, 'DELETE')).status, 200);
  } finally {
    for (const ws of sockets) ws.terminate();
    await c.close();
  }
});

test('HTTP server contains rejected async ingress handlers', async () => {
  const c = new McpBridgeCoordinator();
  c.wsPort = c.httpPort = 0;
  await c.start();
  try {
    c._handleClientHttp = async () => { throw new Error('unexpected ingress error'); };
    const status = await new Promise((resolve, reject) => {
      const req = http.get({ host: '127.0.0.1', port: c.httpServer.address().port, timeout: 1000 }, res => {
        res.resume();
        res.on('end', () => resolve(res.statusCode));
      });
      req.on('error', reject);
      req.on('timeout', () => req.destroy(new Error('response timeout')));
    });
    assert.equal(status, 500);
  } finally { await c.close(); }
});