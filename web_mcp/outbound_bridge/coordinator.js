// HomeGuardian AI — Outbound Authenticated MCP Bridge: Coordinator
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// The Coordinator runs on the authorized coordination host (Oracle VPS or test harness).
// It accepts outbound connections from the edge device (Xiaomi Mi A3), performs mutual
// authentication, strictly enforces operation allowlists, and bridges client MCP requests
// to the remote edge hardware.

const http = require('node:http');
const crypto = require('node:crypto');
const { WebSocketServer } = require('ws');

const {
  PROTOCOL_VERSION,
  BRIDGE_PROTOCOL_VERSION,
  MESSAGE_TYPES,
  LIMITS,
  ERROR_CODES,
  validatePermittedMcpOperation,
} = require('./protocol');

class McpBridgeCoordinator {
  /**
   * @param {object} options
   * @param {string} [options.bindAddress='127.0.0.1'] - Coordinator bind interface
   * @param {number} [options.wsPort=18994] - WebSocket port for edge connections
   * @param {number} [options.httpPort=18995] - HTTP port for client MCP requests
   * @param {string} [options.clientToken] - Shared Bearer token for client HTTP callers
   * @param {Map<string, { authToken: string, name: string, revoked: boolean }>} [options.authorizedDevices]
   */
  constructor(options = {}) {
    const bindAddr = options.bindAddress || '127.0.0.1';
    if (bindAddr === '0.0.0.0' || bindAddr === '::' || bindAddr === '0') {
      throw new Error(`McpBridgeCoordinator: refusing to bind wildcard address (${bindAddr}); explicit interface required.`);
    }

    this.bindAddress = bindAddr;
    this.wsPort = options.wsPort || 18994;
    this.httpPort = options.httpPort || 18995;
    this.clientToken = options.clientToken || crypto.randomBytes(32).toString('hex');

    // Registered edge devices: deviceId -> { authToken, name, revoked, lastSeen, connectedAt }
    this.authorizedDevices = options.authorizedDevices || new Map();

    // Active Edge Connection state
    this.activeEdge = null; // { ws, deviceId, sessionId, connectedAt, lastSeen }
    this.pendingRequests = new Map(); // correlationId -> { resolve, reject, timer, createdAt }

    // Client sessions (Streamable HTTP MCP)
    this.clientSessions = new Map(); // sessionId -> { createdAt, lastSeen }

    // Internal servers
    this.wss = null;
    this.httpServer = null;
    this.livenessTimer = null;

    // Statistics
    this.stats = {
      requestsTotal: 0,
      requestsSuccess: 0,
      requestsFailed: 0,
      requestsRejected: 0,
      reconnectCount: 0,
    };
  }

  authorizeDevice(deviceId, authToken, name = 'Mi A3 Edge Node') {
    this.authorizedDevices.set(deviceId, {
      authToken,
      name,
      revoked: false,
      lastSeen: 0,
      connectedAt: 0,
    });
  }

  revokeDevice(deviceId) {
    const dev = this.authorizedDevices.get(deviceId);
    if (dev) {
      dev.revoked = true;
      if (this.activeEdge && this.activeEdge.deviceId === deviceId) {
        this._terminateEdgeConnection(this.activeEdge.ws, 4403, 'Device credentials revoked');
      }
    }
  }

  isEdgeOnline() {
    return !!this.activeEdge && this.activeEdge.ws.readyState === 1; // 1 = OPEN
  }

  getEdgeInfo() {
    if (!this.activeEdge) return null;
    return {
      deviceId: this.activeEdge.deviceId,
      sessionId: this.activeEdge.sessionId,
      connectedAt: this.activeEdge.connectedAt,
      lastSeen: this.activeEdge.lastSeen,
      inflight: this.pendingRequests.size,
      online: this.isEdgeOnline(),
    };
  }

  async start() {
    // 1. Initialize WebSocket server for outbound edge connections
    await new Promise((resolve, reject) => {
      this.wss = new WebSocketServer(
        {
          host: this.bindAddress,
          port: this.wsPort,
          maxPayload: LIMITS.MAX_MESSAGE_BYTES,
        },
        () => resolve()
      );
      this.wss.on('error', reject);
    });

    this.wss.on('connection', (ws, req) => this._handleEdgeConnection(ws, req));

    // 2. Initialize HTTP server for client Streamable HTTP MCP ingress
    this.httpServer = http.createServer((req, res) => this._handleClientHttp(req, res));
    await new Promise((resolve, reject) => {
      this.httpServer.listen(this.httpPort, this.bindAddress, () => resolve());
      this.httpServer.on('error', reject);
    });

    // 3. Start background heartbeat liveness monitor
    this.livenessTimer = setInterval(() => this._checkEdgeLiveness(), 5000);
  }

  async close() {
    if (this.livenessTimer) {
      clearInterval(this.livenessTimer);
      this.livenessTimer = null;
    }

    // Fail all pending requests
    for (const [corrId, entry] of this.pendingRequests.entries()) {
      clearTimeout(entry.timer);
      entry.reject(new Error(ERROR_CODES.EDGE_OFFLINE));
    }
    this.pendingRequests.clear();

    if (this.activeEdge) {
      try {
        this.activeEdge.ws.close(1000, 'Coordinator shutting down');
      } catch (_) {}
      this.activeEdge = null;
    }

    if (this.wss) {
      await new Promise((r) => this.wss.close(r));
      this.wss = null;
    }

    if (this.httpServer) {
      await new Promise((r) => this.httpServer.close(r));
      this.httpServer = null;
    }
  }

  // --------------------------------------------------------------------------
  // Edge WebSocket Protocol Handling
  // --------------------------------------------------------------------------

  _handleEdgeConnection(ws, req) {
    let authenticated = false;
    let deviceId = null;

    // Guard: Auth timeout if handshake frame not received within 5s
    const authTimeout = setTimeout(() => {
      if (!authenticated) {
        this._terminateEdgeConnection(ws, 4401, 'Handshake authentication timed out');
      }
    }, 5000);

    ws.on('message', (raw) => {
      if (raw.length > LIMITS.MAX_MESSAGE_BYTES) {
        this._terminateEdgeConnection(ws, 4413, 'Message frame exceeded 64KB');
        return;
      }

      let msg;
      try {
        msg = JSON.parse(raw.toString('utf8'));
      } catch (_) {
        this._terminateEdgeConnection(ws, 4400, 'Malformed JSON frame');
        return;
      }

      if (!authenticated) {
        // First frame must be bridge_auth
        if (msg.type !== MESSAGE_TYPES.BRIDGE_AUTH) {
          this._terminateEdgeConnection(ws, 4401, 'First message must be bridge_auth');
          return;
        }

        const authRes = this._verifyEdgeAuth(msg);
        if (!authRes.ok) {
          ws.send(JSON.stringify({
            type: MESSAGE_TYPES.BRIDGE_AUTH_ERR,
            code: authRes.code,
            message: authRes.error,
          }));
          this._terminateEdgeConnection(ws, authRes.code === ERROR_CODES.DEVICE_REVOKED ? 4403 : 4401, authRes.error);
          return;
        }

        clearTimeout(authTimeout);
        authenticated = true;
        deviceId = msg.device_id;

        // If an existing edge session is open, drop it cleanly
        if (this.activeEdge && this.activeEdge.ws !== ws) {
          this.stats.reconnectCount++;
          try {
            this.activeEdge.ws.close(1000, 'Superceded by new authenticated connection');
          } catch (_) {}
        }

        const sessionId = `edge_sess_${crypto.randomBytes(16).toString('hex')}`;
        const now = Date.now();
        this.activeEdge = {
          ws,
          deviceId,
          sessionId,
          connectedAt: now,
          lastSeen: now,
        };

        const devRecord = this.authorizedDevices.get(deviceId);
        if (devRecord) {
          devRecord.lastSeen = now;
          devRecord.connectedAt = now;
        }

        ws.send(JSON.stringify({
          type: MESSAGE_TYPES.BRIDGE_AUTH_OK,
          session_id: sessionId,
          protocol_version: PROTOCOL_VERSION,
          heartbeat_interval_ms: LIMITS.HEARTBEAT_INTERVAL_MS,
        }));
        return;
      }

      // Handle authenticated frames
      this._handleAuthenticatedEdgeFrame(ws, msg);
    });

    ws.on('close', () => {
      clearTimeout(authTimeout);
      if (this.activeEdge && this.activeEdge.ws === ws) {
        this.activeEdge = null;
        // Fail pending requests
        for (const [corrId, entry] of this.pendingRequests.entries()) {
          clearTimeout(entry.timer);
          entry.reject(new Error(ERROR_CODES.EDGE_OFFLINE));
        }
        this.pendingRequests.clear();
      }
    });

    ws.on('error', () => {
      try { ws.close(); } catch (_) {}
    });
  }

  _verifyEdgeAuth(msg) {
    const { device_id, auth_token, bridge_version } = msg;
    if (!device_id || !auth_token) {
      return { ok: false, code: ERROR_CODES.AUTH_REQUIRED, error: 'device_id and auth_token required' };
    }

    const reg = this.authorizedDevices.get(device_id);
    if (!reg) {
      return { ok: false, code: ERROR_CODES.INVALID_CREDENTIALS, error: 'Unrecognized device identity' };
    }

    if (reg.revoked) {
      return { ok: false, code: ERROR_CODES.DEVICE_REVOKED, error: 'Device credentials have been revoked' };
    }

    // Constant-time token comparison
    const a = Buffer.from(auth_token);
    const b = Buffer.from(reg.authToken);
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) {
      return { ok: false, code: ERROR_CODES.INVALID_CREDENTIALS, error: 'Invalid device auth_token' };
    }

    return { ok: true };
  }

  _handleAuthenticatedEdgeFrame(ws, msg) {
    if (this.activeEdge) {
      this.activeEdge.lastSeen = Date.now();
    }

    switch (msg.type) {
      case MESSAGE_TYPES.HEARTBEAT_PING:
        ws.send(JSON.stringify({ type: MESSAGE_TYPES.HEARTBEAT_PONG, timestamp: Date.now() }));
        break;

      case MESSAGE_TYPES.MCP_RESPONSE: {
        const corrId = msg.correlation_id;
        if (!corrId) return;
        const pending = this.pendingRequests.get(corrId);
        if (pending) {
          clearTimeout(pending.timer);
          this.pendingRequests.delete(corrId);
          this.stats.requestsSuccess++;
          pending.resolve({
            statusCode: msg.status_code || 200,
            body: msg.body,
          });
        }
        break;
      }

      case MESSAGE_TYPES.SESSION_TERMINATE:
        this._terminateEdgeConnection(ws, 1000, 'Session termination requested by edge');
        break;

      default:
        // Ignore unhandled message
        break;
    }
  }

  _terminateEdgeConnection(ws, code, reason) {
    try {
      ws.close(code, reason);
    } catch (_) {}
  }

  _checkEdgeLiveness() {
    if (!this.activeEdge) return;
    const elapsed = Date.now() - this.activeEdge.lastSeen;
    if (elapsed > LIMITS.HEARTBEAT_TIMEOUT_MS) {
      this._terminateEdgeConnection(this.activeEdge.ws, 4008, 'Heartbeat timeout');
      this.activeEdge = null;
    }
  }

  // --------------------------------------------------------------------------
  // Request Dispatcher: Bridges local callers to remote edge
  // --------------------------------------------------------------------------

  async dispatchMcpRequest(jsonRpcBody, clientSessionId = null) {
    this.stats.requestsTotal++;

    // 1. Check if edge hardware is connected
    if (!this.isEdgeOnline()) {
      this.stats.requestsFailed++;
      const err = new Error('Remote Mi A3 edge hardware is offline or disconnected');
      err.code = ERROR_CODES.EDGE_OFFLINE;
      err.status = 503;
      throw err;
    }

    // 2. Strict Zero-Trust Operation Allowlist
    const validation = validatePermittedMcpOperation(jsonRpcBody);
    if (!validation.valid) {
      this.stats.requestsRejected++;
      const err = new Error(validation.error);
      err.code = validation.code;
      err.status = 400;
      throw err;
    }

    // 3. Queue bounds check
    if (this.pendingRequests.size >= LIMITS.MAX_INFLIGHT_REQUESTS) {
      this.stats.requestsRejected++;
      const err = new Error('Coordinator request queue full; maximum concurrent in-flight limit reached');
      err.code = ERROR_CODES.QUEUE_FULL;
      err.status = 503;
      throw err;
    }

    // 4. Dispatch over outbound channel
    const correlationId = `req_${crypto.randomBytes(16).toString('hex')}`;
    const frame = {
      type: MESSAGE_TYPES.MCP_REQUEST,
      correlation_id: correlationId,
      client_session_id: clientSessionId,
      body: jsonRpcBody,
      timestamp: Date.now(),
    };

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pendingRequests.has(correlationId)) {
          this.pendingRequests.delete(correlationId);
          this.stats.requestsFailed++;
          const timeoutErr = new Error('Timeout waiting for remote edge response');
          timeoutErr.code = ERROR_CODES.TIMEOUT;
          timeoutErr.status = 504;
          reject(timeoutErr);
        }
      }, LIMITS.DEFAULT_REQUEST_TIMEOUT_MS);

      this.pendingRequests.set(correlationId, { resolve, reject, timer, createdAt: Date.now() });

      try {
        this.activeEdge.ws.send(JSON.stringify(frame));
      } catch (err) {
        clearTimeout(timer);
        this.pendingRequests.delete(correlationId);
        this.stats.requestsFailed++;
        reject(err);
      }
    });
  }

  // --------------------------------------------------------------------------
  // Client Streamable HTTP MCP Ingress Handler
  // --------------------------------------------------------------------------

  async _handleClientHttp(req, res) {
    const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);

    if (url.pathname !== '/mcp') {
      res.writeHead(404, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'not_found' }));
      return;
    }

    // 1. Method check: only POST or DELETE allowed for Streamable HTTP
    if (req.method !== 'POST' && req.method !== 'DELETE') {
      res.writeHead(405, { 'Allow': 'POST, DELETE', 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'method_not_allowed' }));
      return;
    }

    // 2. Client Authentication check
    const authHeader = req.headers['authorization'] || '';
    if (!authHeader.startsWith('Bearer ') || authHeader.slice(7) !== this.clientToken) {
      res.writeHead(401, {
        'WWW-Authenticate': 'Bearer realm="homeguardian-mcp"',
        'Content-Type': 'application/json',
      });
      res.end(JSON.stringify({ error: 'unauthorized', message: 'Valid Bearer token required' }));
      return;
    }

    // 3. Handle DELETE (Session teardown)
    if (req.method === 'DELETE') {
      const sessId = req.headers['mcp-session-id'];
      if (!sessId || !this.clientSessions.has(sessId)) {
        res.writeHead(404, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'session_not_found' }));
        return;
      }
      this.clientSessions.delete(sessId);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true, sessionTerminated: true }));
      return;
    }

    // 4. Validate Content-Type
    const contentType = req.headers['content-type'] || '';
    if (!contentType.includes('application/json')) {
      res.writeHead(415, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'unsupported_media_type', message: 'Content-Type must be application/json' }));
      return;
    }

    // 5. Read body with bounded size
    let bodyRaw = '';
    req.on('data', (chunk) => {
      bodyRaw += chunk;
      if (bodyRaw.length > LIMITS.MAX_MESSAGE_BYTES) {
        res.writeHead(413, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'payload_too_large' }));
        req.destroy();
      }
    });

    req.on('end', async () => {
      let jsonRpc;
      try {
        jsonRpc = JSON.parse(bodyRaw);
      } catch (_) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } }));
        return;
      }

      // Check session requirement
      let clientSessionId = req.headers['mcp-session-id'];
      if (jsonRpc.method === 'initialize') {
        // Mint new client session
        clientSessionId = `coord_sess_${crypto.randomBytes(32).toString('hex')}`;
        this.clientSessions.set(clientSessionId, { createdAt: Date.now(), lastSeen: Date.now() });
      } else {
        if (!clientSessionId || !this.clientSessions.has(clientSessionId)) {
          res.writeHead(404, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'session_not_found', message: 'Valid Mcp-Session-Id header required' }));
          return;
        }
        this.clientSessions.get(clientSessionId).lastSeen = Date.now();
      }

      // Dispatch to remote edge hardware
      try {
        const edgeReply = await this.dispatchMcpRequest(jsonRpc, clientSessionId);
        res.writeHead(edgeReply.statusCode, {
          'Content-Type': 'application/json',
          'Mcp-Session-Id': clientSessionId,
        });
        res.end(typeof edgeReply.body === 'string' ? edgeReply.body : JSON.stringify(edgeReply.body));
      } catch (err) {
        const status = err.status || 500;
        const respHeaders = { 'Content-Type': 'application/json' };
        if (status === 503) {
          respHeaders['Retry-After'] = '2';
        }
        res.writeHead(status, respHeaders);
        res.end(JSON.stringify({
          error: err.code || 'BRIDGE_DISPATCH_ERROR',
          message: err.message,
        }));
      }
    });
  }
}

module.exports = {
  McpBridgeCoordinator,
};
