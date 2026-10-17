// HomeGuardian AI — Outbound Authenticated MCP Bridge Module Export

const protocol = require('./protocol');
const { McpBridgeCoordinator } = require('./coordinator');
const { McpBridgeEdgeClient } = require('./edge_client');

module.exports = {
  ...protocol,
  McpBridgeCoordinator,
  McpBridgeEdgeClient,
};
