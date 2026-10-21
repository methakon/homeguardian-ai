// HomeGuardian AI — Outbound Authenticated MCP Bridge Protocol Specification
//
// Milestone: HG-NET-02E (Remote MCP Integration)
//
// Purpose:
//   Establishes a zero-trust, outbound-only control channel from the edge device
//   (Xiaomi Mi A3 foreground service or edge bridge daemon) to the coordinator
//   (Oracle VPS or local mock coordinator).
//
// Security Constraints:
//   - Outbound-only connection: Mi A3 connects out; coordinator never initiates TCP SYN to phone.
//   - Mutual authentication: Pre-shared device identity + secret, session authorization challenges.
//   - Strict operation allowlist: Only explicitly permitted MCP operations are forwarded to loopback C++ server.
//   - Bounded buffers, max in-flight constraints, and rate limiting prevent resource exhaustion.
//   - Fail-closed: Any unpermitted method, unknown tool, malformed framing, or oversized payload is dropped.

const PROTOCOL_VERSION = '2025-11-25';
const BRIDGE_PROTOCOL_VERSION = '1.0.0';

const MESSAGE_TYPES = Object.freeze({
  BRIDGE_AUTH: 'bridge_auth',
  BRIDGE_AUTH_OK: 'bridge_auth_ok',
  BRIDGE_AUTH_ERR: 'bridge_auth_err',
  MCP_REQUEST: 'mcp_request',
  MCP_RESPONSE: 'mcp_response',
  HEARTBEAT_PING: 'heartbeat_ping',
  HEARTBEAT_PONG: 'heartbeat_pong',
  SESSION_TERMINATE: 'session_terminate',
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

const LIMITS = Object.freeze({
  MAX_MESSAGE_BYTES: 64 * 1024,     // 64 KB per frame
  MAX_INFLIGHT_REQUESTS: 8,         // Bounded concurrent calls
  MAX_QUEUE_DEPTH: 16,              // Max pending requests
  DEFAULT_REQUEST_TIMEOUT_MS: 5000, // 5 second timeout for edge reply
  HEARTBEAT_INTERVAL_MS: 15000,     // 15 seconds ping interval
  HEARTBEAT_TIMEOUT_MS: 35000,      // 35 seconds without ping triggers disconnect
  MAX_RECONNECT_DELAY_MS: 30000,    // Exponential backoff ceiling
  BASE_RECONNECT_DELAY_MS: 1000,    // Initial backoff
});

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

module.exports = {
  PROTOCOL_VERSION,
  BRIDGE_PROTOCOL_VERSION,
  MESSAGE_TYPES,
  PERMITTED_MCP_METHODS,
  PERMITTED_MCP_TOOLS,
  LIMITS,
  ERROR_CODES,
  validatePermittedMcpOperation,
};
