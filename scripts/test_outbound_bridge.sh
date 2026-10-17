#!/usr/bin/env bash
# HomeGuardian AI — Outbound MCP Bridge Test Runner (HG-NET-02E)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "================================================================================"
echo "  Executing Outbound Authenticated MCP Bridge Test Suite"
echo "================================================================================"

node "$ROOT/tests/test_outbound_mcp_bridge.js"
