// HomeGuardian AI — Outbound Authenticated MCP Bridge: Edge Client
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// The Edge Client runs on the edge device (Xiaomi Mi A3 Android foreground service or companion).
// It initiates the outbound connection to the authorized coordinator, handles authentication,
// validates operations, and forwards strictly permitted MCP requests to the local C++ MCP server
// bound to 127.0.0.1:8790.

const http = require('node:http');
const { EventEmitter } = require('node:events');
const { WebSocket } = require('ws');

const {
  PROTOCOL_VERSION,
  BRIDGE_PROTOCOL_VERSION,
  MESSAGE_TYPES,
  LIMITS,
  ERROR_CODES,
  validatePermittedMcpOperation,
} = require('./protocol');

class McpBridgeEdgeClient extends EventEmitter {
  /**
   * @param {object} options
   * @param {string} options.coordinatorUrl - Outbound WebSocket URL (e.g. ws://10.42.0.1:18994)
   * @param {string} options.deviceId - Unique device identifier (e.g. mi_a3_edge_001)
   * @param {string} options.authToken - Shared cryptographic secret for coordinator mutual auth
   * @param {string} [options.localMcpUrl='http://127.0.0.1:8790/mcp'] - Local loopback C++ server
   * @param {string} [options.localMcpToken] - Bearer token for authenticating with local C++ server
   * @param {boolean} [options.autoReconnect=true] - Enable exponential backoff reconnect
   */
  constructor(options = {}) {
    super();

    if (!options.coordinatorUrl) throw new Error('coordinatorUrl is required');
    if (!options.deviceId) throw new Error('deviceId is required');
    if (!options.authToken) throw new Error('authToken is required');

    this.coordinatorUrl = options.coordinatorUrl;
    this.deviceId = options.deviceId;
    this.authToken = options.authToken;
    this.localMcpUrl = options.localMcpUrl || 'http://127.0.0.1:8790/mcp';
    this.localMcpToken = options.localMcpToken || '';
    this.autoReconnect = options.autoReconnect !== false;

    this.ws = null;
    this.connected = false;
    this.authenticated = false;
    this.revoked = false;
    this.explicitClose = false;

    this.sessionId = null;
    this.localMcpSessionId = null; // Captured from local C++ server on initialize

    this.heartbeatTimer = null;
    this.reconnectTimer = null;
    this.reconnectAttempts = 0;

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
    if (this.ws) {
      try {
        this.ws.close(1000, 'Edge client closing');
      } catch (_) {}
      this.ws = null;
    }
  }

  _cleanup() {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.connected = false;
    this.authenticated = false;
    this.sessionId = null;
  }

  _connectSocket() {
    this._cleanup();

    try {
      this.ws = new WebSocket(this.coordinatorUrl);
    } catch (err) {
      this._scheduleReconnect();
      return;
    }

    this.ws.on('open', () => {
      this.connected = true;
      // Step 1: Send outbound mutual auth handshake frame
      const authFrame = {
        type: MESSAGE_TYPES.BRIDGE_AUTH,
        device_id: this.deviceId,
        auth_token: this.authToken,
        bridge_version: BRIDGE_PROTOCOL_VERSION,
        timestamp: Date.now(),
      };
      this._send(authFrame);
    });

    this.ws.on('message', (raw) => {
      if (raw.length > LIMITS.MAX_MESSAGE_BYTES) return;
      let msg;
      try {
        msg = JSON.parse(raw.toString('utf8'));
      } catch (_) {
        return;
      }
      this._handleIncomingMessage(msg);
    });

    this.ws.on('close', (code, reason) => {
      const wasAuth = this.authenticated;
      this._cleanup();
      this.emit('disconnected', { code, reason: reason?.toString() });

      if (code === 4403 || reason?.toString().includes('revoked')) {
        this.revoked = true;
        this.emit('revoked', { reason: reason?.toString() });
        return; // Do not reconnect if revoked
      }

      if (!this.explicitClose && this.autoReconnect) {
        this._scheduleReconnect();
      }
    });

    this.ws.on('error', (err) => {
      this.emit('error', err);
    });
  }

  _handleIncomingMessage(msg) {
    this.stats.messagesReceived++;

    switch (msg.type) {
      case MESSAGE_TYPES.BRIDGE_AUTH_OK:
        this.authenticated = true;
        this.sessionId = msg.session_id;
        this.reconnectAttempts = 0;
        this._startHeartbeat(msg.heartbeat_interval_ms || LIMITS.HEARTBEAT_INTERVAL_MS);
        this.emit('connected', { sessionId: this.sessionId });
        break;

      case MESSAGE_TYPES.BRIDGE_AUTH_ERR:
        this.emit('auth_error', { code: msg.code, message: msg.message });
        if (msg.code === ERROR_CODES.DEVICE_REVOKED) {
          this.revoked = true;
        }
        break;

      case MESSAGE_TYPES.HEARTBEAT_PONG:
        // Coordinator acknowledged liveness
        break;

      case MESSAGE_TYPES.MCP_REQUEST:
        this._handleMcpRequest(msg);
        break;

      default:
        break;
    }
  }

  _startHeartbeat(intervalMs) {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    this.heartbeatTimer = setInterval(() => {
      if (this.connected && this.authenticated) {
        this._send({ type: MESSAGE_TYPES.HEARTBEAT_PING, timestamp: Date.now() });
      }
    }, intervalMs);
  }

  _scheduleReconnect() {
    if (this.explicitClose || this.revoked || !this.autoReconnect) return;

    this.reconnectAttempts++;
    this.stats.reconnectCount++;

    // Exponential backoff with jitter
    const delay = Math.min(
      LIMITS.MAX_RECONNECT_DELAY_MS,
      LIMITS.BASE_RECONNECT_DELAY_MS * Math.pow(2, Math.min(this.reconnectAttempts, 5))
    ) + Math.floor(Math.random() * 500);

    this.emit('reconnecting', { attempt: this.reconnectAttempts, delayMs: delay });

    this.reconnectTimer = setTimeout(() => {
      this._connectSocket();
    }, delay);
  }

  _send(obj) {
    if (this.ws && this.ws.readyState === 1) { // 1 = OPEN
      this.ws.send(JSON.stringify(obj));
    }
  }

  // --------------------------------------------------------------------------
  // Forwarding Engine: Forwards valid MCP operations to local loopback server
  // --------------------------------------------------------------------------

  async _handleMcpRequest(msg) {
    const correlationId = msg.correlation_id;
    const body = msg.body;

    // 1. Strict Zero-Trust Operation Allowlist Check
    const validation = validatePermittedMcpOperation(body);
    if (!validation.valid) {
      this.stats.messagesRejected++;
      this._send({
        type: MESSAGE_TYPES.MCP_RESPONSE,
        correlation_id: correlationId,
        status_code: 400,
        body: {
          jsonrpc: '2.0',
          id: body?.id !== undefined ? body.id : null,
          error: {
            code: -32602,
            message: validation.error,
          },
        },
      });
      return;
    }

    // 2. Forward to local loopback C++ server
    try {
      this.stats.messagesForwarded++;
      const localResult = await this._forwardToLocalMcpServer(body);

      // If initialize, capture local Mcp-Session-Id
      if (body.method === 'initialize' && localResult.sessionId) {
        this.localMcpSessionId = localResult.sessionId;
      }

      this._send({
        type: MESSAGE_TYPES.MCP_RESPONSE,
        correlation_id: correlationId,
        status_code: localResult.statusCode,
        body: localResult.body,
      });
    } catch (err) {
      this._send({
        type: MESSAGE_TYPES.MCP_RESPONSE,
        correlation_id: correlationId,
        status_code: 502,
        body: {
          jsonrpc: '2.0',
          id: body?.id !== undefined ? body.id : null,
          error: {
            code: -32603,
            message: `Edge local MCP server error: ${err.message}`,
          },
        },
      });
    }
  }

  _forwardToLocalMcpServer(jsonRpcBody) {
    return new Promise((resolve, reject) => {
      try {
        const u = new URL(this.localMcpUrl);
        const postData = JSON.stringify(jsonRpcBody);

        const headers = {
          'Content-Type': 'application/json',
          'Accept': 'application/json, text/event-stream',
          'Content-Length': Buffer.byteLength(postData),
        };

        if (this.localMcpToken) {
          headers['Authorization'] = `Bearer ${this.localMcpToken}`;
        }

        if (this.localMcpSessionId && jsonRpcBody.method !== 'initialize') {
          headers['Mcp-Session-Id'] = this.localMcpSessionId;
        }

        const req = http.request(
          {
            hostname: u.hostname,
            port: u.port,
            path: u.pathname,
            method: 'POST',
            headers,
            timeout: 3000,
          },
          (res) => {
            let bodyRaw = '';
            res.on('data', (d) => { bodyRaw += d; });
            res.on('end', () => {
              let parsed;
              try {
                parsed = JSON.parse(bodyRaw);
              } catch (_) {
                parsed = bodyRaw;
              }
              const sid = res.headers['mcp-session-id'] || null;
              resolve({
                statusCode: res.statusCode,
                body: parsed,
                sessionId: sid,
              });
            });
          }
        );

        req.on('error', reject);
        req.on('timeout', () => {
          req.destroy();
          reject(new Error('Local C++ MCP server request timeout'));
        });

        req.write(postData);
        req.end();
      } catch (err) {
        reject(err);
      }
    });
  }
}

module.exports = {
  McpBridgeEdgeClient,
};
