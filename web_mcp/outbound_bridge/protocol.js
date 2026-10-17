// HomeGuardian AI — Outbound Authenticated MCP Bridge Protocol Specification (HGB1)
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// Protocol: HGB1 (Hermes 2 & Antigravity Unified Wire Protocol)
// Transport: Raw TCP / TLS stream
//
// Security Constraints:
//   - Outbound-only connection: Mi A3 Android service initiates TCP connection to Coordinator.
//   - Mutual authentication:
//       1. Edge -> Coordinator: "HGB1 <deviceId> <enrolmentFingerprint>\n" (max 256 bytes)
//       2. Coordinator -> Edge: "<enrolmentSecret>\n"
//       3. Edge -> Coordinator: "OK\n" (on constant-time match) or "DENY\n"
//   - Frame Format: [4-byte big-endian unsigned length][UTF-8 JSON-RPC string]
//   - Max Frame Size: 64 KB (65,536 bytes). Hard cap validated BEFORE buffer allocation.
//   - Strict operation allowlist: Only explicitly permitted MCP operations forwarded to loopback C++ server.
//   - Fail-closed: Any unpermitted method, unknown tool, malformed framing, or oversized payload is dropped.

const crypto = require('node:crypto');

const MAGIC = 'HGB1';
const PROTOCOL_VERSION = '2025-11-25';
const BRIDGE_PROTOCOL_VERSION = 'HGB1';

const LIMITS = Object.freeze({
  MAX_HANDSHAKE_LINE: 256,
  MAX_FRAME: 64 * 1024,             // 64 KB hard cap
  HANDSHAKE_TIMEOUT_MS: 8000,
  RELAY_TIMEOUT_MS: 15000,
  RECONNECT_MIN_MS: 1000,
  RECONNECT_MAX_MS: 60000,
  MAX_INFLIGHT_REQUESTS: 8,
});

// Strictly permitted MCP methods across the bridge
const PERMITTED_MCP_METHODS = Object.freeze(new Set([
  'initialize',
  'notifications/initialized',
  'tools/list',
  'tools/call',
  'ping',
]));

// Strictly permitted read-only tools
const PERMITTED_MCP_TOOLS = Object.freeze(new Set([
  'get_homeguardian_status',
  'get_alert_summary',
]));

const ERROR_CODES = Object.freeze({
  AUTH_REQUIRED: 'AUTH_REQUIRED',
  INVALID_CREDENTIALS: 'INVALID_CREDENTIALS',
  DEVICE_REVOKED: 'DEVICE_REVOKED',
  UNAUTHORIZED_OPERATION: 'UNAUTHORIZED_OPERATION',
  UNAUTHORIZED_TOOL: 'UNAUTHORIZED_TOOL',
  PAYLOAD_TOO_LARGE: 'PAYLOAD_TOO_LARGE',
  RATE_LIMITED: 'RATE_LIMITED',
  QUEUE_FULL: 'QUEUE_FULL',
  TIMEOUT: 'TIMEOUT',
  EDGE_OFFLINE: 'EDGE_OFFLINE',
  INTERNAL_ERROR: 'INTERNAL_ERROR',
});

/**
 * Computes the first 16 hex chars of SHA-256 for a secret string.
 * Matches ControlBridge.java fingerprint().
 * @param {string} secret
 * @returns {string}
 */
function computeFingerprint(secret) {
  if (typeof secret !== 'string') return '';
  return crypto.createHash('sha256').update(secret, 'utf8').digest('hex').substring(0, 16);
}

/**
 * Encodes a string or JSON object into a length-prefixed frame buffer:
 * [4-byte big-endian length][payload]
 * @param {string|Buffer|object} payload
 * @returns {Buffer}
 */
function encodeFrame(payload) {
  let buf;
  if (Buffer.isBuffer(payload)) {
    buf = payload;
  } else if (typeof payload === 'string') {
    buf = Buffer.from(payload, 'utf8');
  } else {
    buf = Buffer.from(JSON.stringify(payload), 'utf8');
  }

  if (buf.length > LIMITS.MAX_FRAME) {
    throw new Error(`Frame length ${buf.length} exceeds hard cap of ${LIMITS.MAX_FRAME} bytes`);
  }

  const frame = Buffer.allocUnsafe(4 + buf.length);
  frame.writeUInt32BE(buf.length, 0);
  buf.copy(frame, 4);
  return frame;
}

/**
 * Validates whether an incoming MCP JSON-RPC payload is permitted by the security contract.
 * @param {object} jsonRpc - parsed JSON-RPC request body
 * @returns {{ valid: boolean, error?: string, code?: string }}
 */
function validatePermittedMcpOperation(jsonRpc) {
  if (!jsonRpc || typeof jsonRpc !== 'object') {
    return { valid: false, error: 'Malformed payload: not a JSON object', code: ERROR_CODES.UNAUTHORIZED_OPERATION };
  }

  // Must have JSON-RPC 2.0 marker
  if (jsonRpc.jsonrpc !== '2.0') {
    return { valid: false, error: 'Invalid jsonrpc version; must be 2.0', code: ERROR_CODES.UNAUTHORIZED_OPERATION };
  }

  const method = jsonRpc.method;
  if (!method || typeof method !== 'string') {
    return { valid: false, error: 'Missing or invalid method string', code: ERROR_CODES.UNAUTHORIZED_OPERATION };
  }

  if (!PERMITTED_MCP_METHODS.has(method)) {
    return {
      valid: false,
      error: `Method '${method}' is not permitted across the outbound control bridge`,
      code: ERROR_CODES.UNAUTHORIZED_OPERATION,
    };
  }

  // If tools/call, strictly validate the tool name
  if (method === 'tools/call') {
    const params = jsonRpc.params;
    if (!params || typeof params !== 'object') {
      return { valid: false, error: 'tools/call missing params object', code: ERROR_CODES.UNAUTHORIZED_OPERATION };
    }
    const toolName = params.name;
    if (!toolName || typeof toolName !== 'string') {
      return { valid: false, error: 'tools/call missing params.name', code: ERROR_CODES.UNAUTHORIZED_OPERATION };
    }
    if (!PERMITTED_MCP_TOOLS.has(toolName)) {
      return {
        valid: false,
        error: `Tool '${toolName}' is prohibited across outbound bridge; strictly read-only tools allowed`,
        code: ERROR_CODES.UNAUTHORIZED_TOOL,
      };
    }
  }

  return { valid: true };
}

/**
 * Helper class to parse length-prefixed frames from a stream of chunks.
 */
class FrameParser {
  constructor(onFrame, onError) {
    this.onFrame = onFrame;
    this.onError = onError;
    this.buf = Buffer.alloc(0);
    this.expectedLen = null;
  }

  push(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    while (true) {
      if (this.expectedLen === null) {
        if (this.buf.length < 4) return;
        const len = this.buf.readUInt32BE(0);
        if (len <= 0 || len > LIMITS.MAX_FRAME) {
          if (this.onError) {
            this.onError(new Error(`Oversized or invalid frame length: ${len}`));
          }
          return;
        }
        this.expectedLen = len;
        this.buf = this.buf.subarray(4);
      }

      if (this.buf.length < this.expectedLen) {
        return; // wait for remaining bytes
      }

      const framePayload = this.buf.subarray(0, this.expectedLen);
      this.buf = this.buf.subarray(this.expectedLen);
      this.expectedLen = null;

      if (this.onFrame) {
        this.onFrame(framePayload);
      }
    }
  }
}

module.exports = {
  MAGIC,
  PROTOCOL_VERSION,
  BRIDGE_PROTOCOL_VERSION,
  LIMITS,
  PERMITTED_MCP_METHODS,
  PERMITTED_MCP_TOOLS,
  ERROR_CODES,
  computeFingerprint,
  encodeFrame,
  validatePermittedMcpOperation,
  FrameParser,
};
