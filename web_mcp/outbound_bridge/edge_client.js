// HomeGuardian AI — Outbound Authenticated MCP Bridge: Edge Client (HGB1)
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// The Edge Client runs on the edge device (Xiaomi Mi A3 companion or Node test runner).
// It initiates the outbound TCP connection to the authorized coordinator speaking HGB1,
// verifies mutual authentication, validates operations, and forwards strictly permitted
// MCP requests to the local C++ MCP server bound to 127.0.0.1:8790.

const net = require('node:net');
const http = require('node:http');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');

const {
  MAGIC,
  LIMITS,
  PERMITTED_MCP_METHODS,
  computeFingerprint,
  encodeFrame,
  FrameParser,
} = require('./protocol');

class McpBridgeEdgeClient extends EventEmitter {
  /**
   * @param {object} options
   * @param {string} options.coordinatorHost - Outbound Coordinator host (e.g. 127.0.0.1 or VPN IP)
   * @param {number} options.coordinatorPort - Outbound Coordinator TCP port (e.g. 9443 or test port)
   * @param {string} options.deviceId - Unique device identifier (e.g. hg-android-...)
   * @param {string} options.enrolmentSecret - Pre-shared CSPRNG secret for coordinator authentication
   * @param {string} [options.localMcpUrl='http://127.0.0.1:8790/mcp'] - Local loopback C++ server
   * @param {string} [options.localMcpToken] - Bearer token for authenticating with local C++ server (never leaves host)
   * @param {boolean} [options.autoReconnect=true] - Enable exponential backoff reconnect
   */
  constructor(options = {}) {
    super();

    if (!options.coordinatorHost) throw new Error('coordinatorHost is required');
    if (!options.coordinatorPort) throw new Error('coordinatorPort is required');
    if (!options.deviceId) throw new Error('deviceId is required');
    if (!options.enrolmentSecret) throw new Error('enrolmentSecret is required');

    this.coordinatorHost = options.coordinatorHost;
    this.coordinatorPort = options.coordinatorPort;
    this.deviceId = options.deviceId;
    this.enrolmentSecret = options.enrolmentSecret;
    this.localMcpUrl = options.localMcpUrl || 'http://127.0.0.1:8790/mcp';
    this.localMcpToken = options.localMcpToken || '';
    this.autoReconnect = options.autoReconnect !== false;

    this.socket = null;
    this.connected = false;
    this.authenticated = false;
    this.explicitClose = false;

    this.reconnectTimer = null;
    this.reconnectDelay = LIMITS.RECONNECT_MIN_MS;

    this.stats = {
      messagesReceived: 0,
      messagesForwarded: 0,
      messagesRejected: 0,
      reconnectCount: 0,
    };
  }

  connect() {
    this.explicitClose = false;
    this._connectSocket();
  }

  disconnect() {
    this.explicitClose = true;
    this.autoReconnect = false;
    this._cleanup();
    if (this.socket) {
      try {
        this.socket.destroy();
      } catch (_) {}
      this.socket = null;
    }
  }

  _cleanup() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.connected = false;
    this.authenticated = false;
  }

  _connectSocket() {
    this._cleanup();

    const socket = new net.Socket();
    this.socket = socket;
    socket.setNoDelay(true);
    socket.setTimeout(LIMITS.HANDSHAKE_TIMEOUT_MS);

    let stage = 'HELLO_SENT';
    let lineBuf = Buffer.alloc(0);
    let parser = null;

    socket.connect(this.coordinatorPort, this.coordinatorHost, () => {
      this.connected = true;
      const fp = computeFingerprint(this.enrolmentSecret);
      socket.write(`${MAGIC} ${this.deviceId} ${fp}\n`);
    });

    socket.on('data', (chunk) => {
      if (this.authenticated && parser) {
        parser.push(chunk);
        return;
      }

      lineBuf = Buffer.concat([lineBuf, chunk]);
      const newlineIdx = lineBuf.indexOf(0x0a);
      if (newlineIdx === -1) {
        if (lineBuf.length > LIMITS.MAX_HANDSHAKE_LINE) {
          socket.destroy();
        }
        return;
      }

      const lineStr = lineBuf.subarray(0, newlineIdx).toString('utf8').replace(/\r$/, '');
      const remaining = lineBuf.subarray(newlineIdx + 1);
      lineBuf = remaining;

      if (stage === 'HELLO_SENT') {
        // Coordinator presents secret
        const presented = lineStr.trim();
        const expected = this.enrolmentSecret.trim();

        const match = this._constantTimeCompare(presented, expected);
        if (!match) {
          socket.write('DENY\n');
          this.emit('error', new Error('Coordinator presented invalid enrolment secret'));
          socket.destroy();
          return;
        }

        socket.write('OK\n');
        this.authenticated = true;
        this.reconnectDelay = LIMITS.RECONNECT_MIN_MS;
        socket.setTimeout(LIMITS.RELAY_TIMEOUT_MS);

        parser = new FrameParser(
          (frame) => this._onFrame(frame),
          (err) => {
            this.emit('error', err);
            socket.destroy();
          }
        );

        this.emit('authenticated');

        if (lineBuf.length > 0) {
          parser.push(lineBuf);
          lineBuf = Buffer.alloc(0);
        }
      }
    });

    socket.on('timeout', () => {
      socket.destroy();
    });

    socket.on('error', (err) => {
      this.emit('error', err);
    });

    socket.on('close', () => {
      this.connected = false;
      this.authenticated = false;
      this.emit('disconnected');

      if (!this.explicitClose && this.autoReconnect) {
        this.stats.reconnectCount++;
        this.reconnectTimer = setTimeout(() => {
          this._connectSocket();
        }, this.reconnectDelay);
        this.reconnectDelay = Math.min(this.reconnectDelay * 2, LIMITS.RECONNECT_MAX_MS);
      }
    });
  }

  _constantTimeCompare(a, b) {
    const bufA = Buffer.from(a, 'utf8');
    const bufB = Buffer.from(b, 'utf8');
    if (bufA.length !== bufB.length) return false;
    return crypto.timingSafeEqual(bufA, bufB);
  }

  async _onFrame(frame) {
    this.stats.messagesReceived++;
    let jsonRpc;
    let reqId = -1;

    try {
      jsonRpc = JSON.parse(frame.toString('utf8'));
      reqId = jsonRpc.id !== undefined ? jsonRpc.id : -1;
    } catch (_) {
      this.stats.messagesRejected++;
      this._sendError(reqId, -32700, 'malformed request');
      return;
    }

    const method = jsonRpc.method;
    if (!method || !PERMITTED_MCP_METHODS.has(method)) {
      this.stats.messagesRejected++;
      this._sendError(reqId, -32601, `method not allowed: ${method}`);
      return;
    }

    try {
      const resp = await this._forwardToLocalMcp(jsonRpc);
      this.stats.messagesForwarded++;
      this._sendFrame(resp);
    } catch (relayErr) {
      this.stats.messagesRejected++;
      this._sendError(reqId, -32603, `relay failed: ${relayErr.message}`);
    }
  }

  _sendError(id, code, message) {
    const errObj = {
      jsonrpc: '2.0',
      id: id,
      error: { code, message },
    };
    this._sendFrame(errObj);
  }

  _sendFrame(payload) {
    if (!this.socket || this.socket.destroyed || !this.authenticated) return;
    try {
      const buf = encodeFrame(payload);
      this.socket.write(buf);
    } catch (_) {}
  }

  _forwardToLocalMcp(jsonRpc) {
    return new Promise((resolve, reject) => {
      const url = new URL(this.localMcpUrl);
      const postData = JSON.stringify(jsonRpc);

      const headers = {
        'Content-Type': 'application/json',
        'Accept': 'application/json',
        'Content-Length': Buffer.byteLength(postData),
      };

      if (this.localMcpToken) {
        headers['Authorization'] = `Bearer ${this.localMcpToken}`;
      }

      if (this.localMcpSessionId) {
        headers['Mcp-Session-Id'] = this.localMcpSessionId;
      }

      const req = http.request(
        {
          hostname: url.hostname,
          port: url.port || 80,
          path: url.pathname + (url.search || ''),
          method: 'POST',
          headers,
          timeout: 4000,
        },
        (res) => {
          const sid = res.headers['mcp-session-id'];
          if (sid) {
            this.localMcpSessionId = sid;
          }

          let body = '';
          res.setEncoding('utf8');
          res.on('data', (c) => (body += c));
          res.on('end', () => {
            try {
              const parsed = JSON.parse(body);
              resolve(parsed);
            } catch (_) {
              resolve({
                jsonrpc: '2.0',
                id: jsonRpc.id,
                error: { code: -32603, message: `Invalid JSON from loopback MCP (status ${res.statusCode})` },
              });
            }
          });
        }
      );

      req.on('timeout', () => {
        req.destroy();
        reject(new Error('Local MCP request timeout'));
      });

      req.on('error', (err) => {
        reject(err);
      });

      req.write(postData);
      req.end();
    });
  }
}

module.exports = {
  McpBridgeEdgeClient,
};
