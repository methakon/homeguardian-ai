// HomeGuardian AI — Outbound Authenticated MCP Bridge: Coordinator (HGB1)
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// The Coordinator runs on the authorized coordination host (Oracle VPS or local test harness).
// It accepts outbound TCP connections from the edge device (Xiaomi Mi A3), performs mutual
// authentication using the HGB1 protocol, strictly enforces operation allowlists, and bridges
// client MCP requests to the remote edge hardware.

const net = require('node:net');
const http = require('node:http');
const crypto = require('node:crypto');

const {
  MAGIC,
  PROTOCOL_VERSION,
  BRIDGE_PROTOCOL_VERSION,
  LIMITS,
  ERROR_CODES,
  computeFingerprint,
  encodeFrame,
  validatePermittedMcpOperation,
  FrameParser,
} = require('./protocol');

class McpBridgeCoordinator {
  /**
   * @param {object} options
   * @param {string} [options.bindAddress='127.0.0.1'] - Coordinator bind interface (no wildcard)
   * @param {number} [options.tcpPort=9443] - TCP port for HGB1 edge connections
   * @param {number} [options.httpPort=18995] - HTTP port for client MCP requests
   * @param {string} [options.clientToken] - Shared Bearer token for client HTTP callers
   * @param {Map<string, { enrolmentSecret: string, name: string, revoked: boolean, fingerprint: string }>} [options.authorizedDevices]
   */
  constructor(options = {}) {
    const bindAddr = options.bindAddress || '127.0.0.1';
    if (bindAddr === '0.0.0.0' || bindAddr === '::' || bindAddr === '0') {
      throw new Error(`McpBridgeCoordinator: refusing to bind wildcard address (${bindAddr}); explicit interface required.`);
    }

    this.bindAddress = bindAddr;
    this.tcpPort = options.tcpPort || 9443;
    this.httpPort = options.httpPort || 18995;
    this.clientToken = options.clientToken || crypto.randomBytes(32).toString('hex');

    // Registered edge devices: deviceId -> { enrolmentSecret, name, revoked, fingerprint }
    this.authorizedDevices = options.authorizedDevices || new Map();

    // Active Edge Connection state
    this.activeEdge = null; // { socket, deviceId, connectedAt, lastSeen, parser }
    this.pendingRequests = new Map(); // id -> { resolve, reject, timer, createdAt }
    this.nextRequestId = 1;

    // Client sessions (Streamable HTTP MCP)
    this.clientSessions = new Map(); // sessionId -> { createdAt, lastSeen }

    // Internal servers
    this.tcpServer = null;
    this.httpServer = null;

    // Statistics
    this.stats = {
      connectionsAccepted: 0,
      handshakesSuccess: 0,
      handshakesFailed: 0,
      requestsTotal: 0,
      requestsSuccess: 0,
      requestsFailed: 0,
      requestsRejected: 0,
      reconnectCount: 0,
    };
  }

  authorizeDevice(deviceId, enrolmentSecret, name = 'Mi A3 Edge Node') {
    const fingerprint = computeFingerprint(enrolmentSecret);
    this.authorizedDevices.set(deviceId, {
      enrolmentSecret,
      name,
      revoked: false,
      fingerprint,
      lastSeen: 0,
      connectedAt: 0,
    });
  }

  revokeDevice(deviceId) {
    const dev = this.authorizedDevices.get(deviceId);
    if (dev) {
      dev.revoked = true;
      if (this.activeEdge && this.activeEdge.deviceId === deviceId) {
        this._terminateEdgeConnection(this.activeEdge.socket, 'Device credentials revoked');
      }
    }
  }

  isEdgeOnline() {
    return !!this.activeEdge && !this.activeEdge.socket.destroyed && this.activeEdge.socket.writable;
  }

  getEdgeInfo() {
    if (!this.activeEdge) return null;
    return {
      deviceId: this.activeEdge.deviceId,
      connectedAt: this.activeEdge.connectedAt,
      lastSeen: this.activeEdge.lastSeen,
      inflight: this.pendingRequests.size,
      online: this.isEdgeOnline(),
    };
  }

  getStatus() {
    return {
      status: 'operational',
      protocolVersion: PROTOCOL_VERSION,
      bridgeProtocol: BRIDGE_PROTOCOL_VERSION,
      edgeOnline: this.isEdgeOnline(),
      activeEdge: this.getEdgeInfo(),
      stats: { ...this.stats },
    };
  }

  start() {
    return new Promise((resolve, reject) => {
      let startedServers = 0;
      const onError = (err) => {
        this.stop();
        reject(err);
      };

      // 1. TCP Server for HGB1 edge connections
      this.tcpServer = net.createServer((socket) => {
        this._handleEdgeTcpConnection(socket);
      });

      this.tcpServer.on('error', onError);
      this.tcpServer.listen(this.tcpPort, this.bindAddress, () => {
        startedServers++;
        if (startedServers === 2) resolve();
      });

      // 2. HTTP Server for client MCP requests (Alexa, dashboard, judge tests)
      this.httpServer = http.createServer((req, res) => {
        this._handleHttpClientRequest(req, res);
      });

      this.httpServer.on('error', onError);
      this.httpServer.listen(this.httpPort, this.bindAddress, () => {
        startedServers++;
        if (startedServers === 2) resolve();
      });
    });
  }

  stop() {
    return new Promise((resolve) => {
      // Clear pending requests
      for (const [id, req] of this.pendingRequests.entries()) {
        clearTimeout(req.timer);
        req.reject(new Error('Coordinator stopped'));
      }
      this.pendingRequests.clear();

      if (this.activeEdge) {
        this._terminateEdgeConnection(this.activeEdge.socket, 'Coordinator shutting down');
      }

      let toClose = 0;
      let closed = 0;
      const done = () => {
        closed++;
        if (closed >= toClose) resolve();
      };

      if (this.tcpServer) {
        toClose++;
        this.tcpServer.close(() => done());
        this.tcpServer = null;
      }
      if (this.httpServer) {
        toClose++;
        this.httpServer.close(() => done());
        this.httpServer = null;
      }

      if (toClose === 0) resolve();
    });
  }

  // --------------------------------------------------------------------------
  // HGB1 TCP Handshake & Relay Logic
  // --------------------------------------------------------------------------

  _handleEdgeTcpConnection(socket) {
    this.stats.connectionsAccepted++;
    socket.setNoDelay(true);
    socket.setTimeout(LIMITS.HANDSHAKE_TIMEOUT_MS);

    let stage = 'HELLO'; // HELLO -> VERDICT -> FRAMES
    let lineBuf = Buffer.alloc(0);
    let matchedDevice = null;

    const onData = (chunk) => {
      if (stage === 'FRAMES') {
        // Frame parser handles data once authenticated
        if (this.activeEdge && this.activeEdge.parser) {
          this.activeEdge.parser.push(chunk);
        }
        return;
      }

      lineBuf = Buffer.concat([lineBuf, chunk]);
      if (lineBuf.length > LIMITS.MAX_HANDSHAKE_LINE * 2) {
        this.stats.handshakesFailed++;
        socket.destroy();
        return;
      }

      const newlineIdx = lineBuf.indexOf(0x0a); // '\n'
      if (newlineIdx === -1) {
        if (lineBuf.length > LIMITS.MAX_HANDSHAKE_LINE) {
          this.stats.handshakesFailed++;
          socket.destroy();
        }
        return;
      }

      const lineStr = lineBuf.subarray(0, newlineIdx).toString('utf8').replace(/\r$/, '');
      const remaining = lineBuf.subarray(newlineIdx + 1);
      lineBuf = remaining;

      if (stage === 'HELLO') {
        const parts = lineStr.split(' ');
        if (parts.length !== 3 || parts[0] !== MAGIC) {
          this.stats.handshakesFailed++;
          socket.destroy();
          return;
        }

        const deviceId = parts[1];
        const enrolmentFp = parts[2];

        const dev = this.authorizedDevices.get(deviceId);
        if (!dev || dev.revoked) {
          this.stats.handshakesFailed++;
          // Send bad secret or close immediately
          socket.write('REVOKED\n', () => socket.destroy());
          return;
        }

        // Verify fingerprint
        if (dev.fingerprint !== enrolmentFp) {
          this.stats.handshakesFailed++;
          socket.write('BAD_FINGERPRINT\n', () => socket.destroy());
          return;
        }

        matchedDevice = { id: deviceId, ...dev };
        stage = 'VERDICT';

        // Send back enrolment secret
        socket.write(dev.enrolmentSecret + '\n');
        return;
      }

      if (stage === 'VERDICT') {
        if (lineStr.trim() !== 'OK') {
          this.stats.handshakesFailed++;
          socket.destroy();
          return;
        }

        // Mutual authentication passed!
        this.stats.handshakesSuccess++;
        if (this.activeEdge) {
          this.stats.reconnectCount++;
          this._terminateEdgeConnection(this.activeEdge.socket, 'New connection from edge');
        }

        socket.setTimeout(LIMITS.RELAY_TIMEOUT_MS);

        const parser = new FrameParser(
          (frame) => this._onEdgeFrame(frame),
          (err) => {
            this._terminateEdgeConnection(socket, err.message);
          }
        );

        this.activeEdge = {
          socket,
          deviceId: matchedDevice.id,
          connectedAt: Date.now(),
          lastSeen: Date.now(),
          parser,
        };

        stage = 'FRAMES';

        // Process any leftover bytes that were buffered after the \n in VERDICT
        if (lineBuf.length > 0) {
          parser.push(lineBuf);
          lineBuf = Buffer.alloc(0);
        }
      }
    };

    socket.on('data', onData);
    socket.on('timeout', () => {
      this._terminateEdgeConnection(socket, 'Socket timeout');
    });
    socket.on('error', (err) => {
      this._terminateEdgeConnection(socket, `Socket error: ${err.message}`);
    });
    socket.on('close', () => {
      if (this.activeEdge && this.activeEdge.socket === socket) {
        this.activeEdge = null;
      }
    });
  }

  _terminateEdgeConnection(socket, reason) {
    if (this.activeEdge && this.activeEdge.socket === socket) {
      this.activeEdge = null;
    }
    try {
      socket.destroy();
    } catch (_) {}
  }

  _onEdgeFrame(frame) {
    if (this.activeEdge) {
      this.activeEdge.lastSeen = Date.now();
    }

    let obj;
    try {
      obj = JSON.parse(frame.toString('utf8'));
    } catch (e) {
      this.stats.requestsFailed++;
      return;
    }

    const id = obj.id;
    if (id !== undefined && this.pendingRequests.has(id)) {
      const pending = this.pendingRequests.get(id);
      this.pendingRequests.delete(id);
      clearTimeout(pending.timer);

      if (obj.error) {
        this.stats.requestsFailed++;
      } else {
        this.stats.requestsSuccess++;
      }
      pending.resolve(obj);
    }
  }

  // --------------------------------------------------------------------------
  // Outbound Request Relaying
  // --------------------------------------------------------------------------

  callEdge(jsonRpcObj, timeoutMs = LIMITS.RELAY_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
      if (!this.isEdgeOnline()) {
        const err = new Error('Edge device is offline');
        err.code = ERROR_CODES.EDGE_OFFLINE;
        return reject(err);
      }

      if (this.pendingRequests.size >= LIMITS.MAX_INFLIGHT_REQUESTS) {
        const err = new Error('Max in-flight requests reached on outbound bridge');
        err.code = ERROR_CODES.QUEUE_FULL;
        return reject(err);
      }

      this.stats.requestsTotal++;

      let id = jsonRpcObj.id;
      if (id === undefined || id === null) {
        id = this.nextRequestId++;
        jsonRpcObj.id = id;
      }

      let frameBuf;
      try {
        frameBuf = encodeFrame(jsonRpcObj);
      } catch (err) {
        this.stats.requestsRejected++;
        return reject(err);
      }

      const timer = setTimeout(() => {
        if (this.pendingRequests.has(id)) {
          this.pendingRequests.delete(id);
          this.stats.requestsFailed++;
          const err = new Error(`Request timed out after ${timeoutMs}ms`);
          err.code = ERROR_CODES.TIMEOUT;
          reject(err);
        }
      }, timeoutMs);

      this.pendingRequests.set(id, {
        resolve,
        reject,
        timer,
        createdAt: Date.now(),
      });

      try {
        this.activeEdge.socket.write(frameBuf);
      } catch (writeErr) {
        clearTimeout(timer);
        this.pendingRequests.delete(id);
        this.stats.requestsFailed++;
        reject(writeErr);
      }
    });
  }

  callMcp(method, params, timeoutMs = LIMITS.RELAY_TIMEOUT_MS) {
    const id = this.nextRequestId++;
    const req = {
      jsonrpc: '2.0',
      id,
      method,
    };
    if (params !== undefined) req.params = params;

    const validation = validatePermittedMcpOperation(req);
    if (!validation.valid) {
      this.stats.requestsRejected++;
      const err = new Error(validation.error);
      err.code = validation.code;
      return Promise.reject(err);
    }

    return this.callEdge(req, timeoutMs);
  }

  // --------------------------------------------------------------------------
  // HTTP Client Ingress (Streamable HTTP MCP 2025-11-25)
  // --------------------------------------------------------------------------

  _handleHttpClientRequest(req, res) {
    // CORS headers
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, DELETE, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, mcp-session-id');

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      return res.end();
    }

    // Status endpoint (no auth required for health monitoring)
    if (req.url === '/status' || req.url === '/edge/info') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      return res.end(JSON.stringify(this.getStatus(), null, 2));
    }

    // MCP routes require Bearer auth
    const authHeader = req.headers['authorization'] || '';
    const match = authHeader.match(/^Bearer\s+(.+)$/i);
    if (!match || match[1] !== this.clientToken) {
      res.writeHead(401, { 'Content-Type': 'application/json' });
      return res.end(JSON.stringify({
        jsonrpc: '2.0',
        error: { code: -32000, message: 'Unauthorized: invalid or missing bearer token' },
      }));
    }

    if (req.method === 'DELETE' && req.url === '/mcp') {
      const sessionId = req.headers['mcp-session-id'];
      if (sessionId) this.clientSessions.delete(sessionId);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      return res.end(JSON.stringify({ status: 'session terminated' }));
    }

    if (req.method === 'POST' && req.url === '/mcp') {
      let bodyBuf = Buffer.alloc(0);
      req.on('data', (chunk) => {
        bodyBuf = Buffer.concat([bodyBuf, chunk]);
        if (bodyBuf.length > LIMITS.MAX_FRAME) {
          res.writeHead(413, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({
            jsonrpc: '2.0',
            error: { code: -32600, message: 'Payload exceeds 64KB limit' },
          }));
          req.destroy();
        }
      });

      req.on('end', async () => {
        let jsonRpc;
        try {
          jsonRpc = JSON.parse(bodyBuf.toString('utf8'));
        } catch (_) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          return res.end(JSON.stringify({
            jsonrpc: '2.0',
            error: { code: -32700, message: 'Parse error: invalid JSON' },
          }));
        }

        // Validate method & tool allowlist
        const validation = validatePermittedMcpOperation(jsonRpc);
        if (!validation.valid) {
          this.stats.requestsRejected++;
          res.writeHead(200, { 'Content-Type': 'application/json' });
          return res.end(JSON.stringify({
            jsonrpc: '2.0',
            id: jsonRpc.id || null,
            error: { code: -32601, message: validation.error },
          }));
        }

        // Check if edge is online
        if (!this.isEdgeOnline()) {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          return res.end(JSON.stringify({
            jsonrpc: '2.0',
            id: jsonRpc.id || null,
            error: {
              code: -32001,
              message: 'Edge device is offline (LOCAL SYNTHETIC FIXTURE — MI A3 OFFLINE)',
            },
          }));
        }

        try {
          const edgeResp = await this.callEdge(jsonRpc);
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify(edgeResp));
        } catch (callErr) {
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({
            jsonrpc: '2.0',
            id: jsonRpc.id || null,
            error: { code: -32603, message: callErr.message },
          }));
        }
      });
      return;
    }

    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
  }
}

module.exports = {
  McpBridgeCoordinator,
};
