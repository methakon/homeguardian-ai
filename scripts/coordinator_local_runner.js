#!/usr/bin/env node
// coordinator_local_runner.js — start the HomeGuardian MCP bridge coordinator on loopback only.
//
// Invoked by scripts/coordinator_local.sh. Not meant to be run directly by hand, though it can be.
//
// Isolation guarantees (these are the point of the file):
//   - binds 127.0.0.1 only; the class itself refuses 0.0.0.0 / :: / 0
//   - client token and device token are generated per run and never written to disk or logged
//   - the device registry is strictly an in-memory test registry resolved from the coordinator
//     source tree; no production identity, no credential file, no Oracle endpoint
//   - no persistence, no supervision, no boot configuration. It dies with the process.
//
// Exit codes: 0 clean shutdown, 1 startup failure, 2 usage/source error.

'use strict';

const path = require('node:path');
const fs = require('node:fs');
const crypto = require('node:crypto');

function parseArgs(argv) {
  const out = { wsPort: 18994, httpPort: 18995, source: null, readyFile: null };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--source') out.source = argv[++i];
    else if (a === '--ws-port') out.wsPort = Number(argv[++i]);
    else if (a === '--http-port') out.httpPort = Number(argv[++i]);
    else if (a === '--ready-file') out.readyFile = argv[++i];
    else if (a === '-h' || a === '--help') { console.log('usage: coordinator_local_runner.js --source DIR [--ws-port N] [--http-port N] [--ready-file F]'); process.exit(0); }
    else { console.error(`unknown argument: ${a}`); process.exit(2); }
  }
  if (!out.source) { console.error('--source is required (directory containing web_mcp/outbound_bridge)'); process.exit(2); }
  return out;
}

const args = parseArgs(process.argv);

// Resolve the coordinator module from the caller-supplied source tree.
const entry = path.join(args.source, 'web_mcp', 'outbound_bridge', 'index.js');
if (!fs.existsSync(entry)) {
  console.error(`coordinator module not found at ${entry}`);
  console.error('the coordinator lives on the common MCP bridge branch; pass --source pointing at a checkout that has it');
  process.exit(2);
}

let McpBridgeCoordinator;
try {
  ({ McpBridgeCoordinator } = require(entry));
} catch (e) {
  console.error(`failed to load coordinator module: ${e.message}`);
  process.exit(2);
}
if (typeof McpBridgeCoordinator !== 'function') {
  console.error('coordinator module did not export McpBridgeCoordinator');
  process.exit(2);
}

(async () => {
  // Per-run generated credentials. Never persisted, never logged.
  const clientToken = crypto.randomBytes(32).toString('hex');
  const testDeviceId = `local-test-device-${crypto.randomBytes(4).toString('hex')}`;
  const testDeviceToken = crypto.randomBytes(32).toString('hex');

  let coord;
  try {
    coord = new McpBridgeCoordinator({
      bindAddress: '127.0.0.1',
      wsPort: args.wsPort,
      httpPort: args.httpPort,
      clientToken,
      authorizedDevices: new Map(),          // empty test registry
    });
  } catch (e) {
    console.error(`coordinator construction failed: ${e.message}`);
    process.exit(1);
  }

  // Register the one throwaway identity so auth paths are exercisable without any real identity.
  coord.authorizeDevice(testDeviceId, testDeviceToken, 'Local test device');

  try {
    await coord.start();
  } catch (e) {
    const msg = String(e && e.message || e);
    if (/EADDRINUSE/.test(msg)) console.error(`startup failed: port already in use (${msg})`);
    else if (/EACCES/.test(msg)) console.error(`startup failed: permission denied (${msg})`);
    else console.error(`startup failed: ${msg}`);
    process.exit(1);
  }

  // Sanity: the bind must be loopback. The class already enforces this, but assert it so a future
  // change to the constructor cannot silently widen the exposure.
  const addr = (coord.httpServer && coord.httpServer.address()) || null;
  if (!addr || (addr.address !== '127.0.0.1' && addr.address !== '::1')) {
    console.error(`refusing to run: bound to unexpected address ${addr ? addr.address : 'unknown'}`);
    try { await coord.close(); } catch {}
    process.exit(1);
  }

  console.log(`coordinator ready pid=${process.pid}`);
  console.log(`  websocket(edge)  ws://127.0.0.1:${args.wsPort}`);
  console.log(`  http(mcp client) http://127.0.0.1:${args.httpPort}/mcp`);
  console.log(`  bind=${addr.address}  test-registry=1 entry  tokens=per-run-generated`);

  if (args.readyFile) {
    try { fs.writeFileSync(args.readyFile, `ready ${process.pid}\n`, { mode: 0o600 }); } catch {}
  }

  let shuttingDown = false;
  const shutdown = async (sig) => {
    if (shuttingDown) return;
    shuttingDown = true;
    console.log(`shutting down (${sig})`);
    if (coord.livenessTimer) clearInterval(coord.livenessTimer);
    try { await coord.close(); console.log('shutdown complete'); } catch (e) { console.error(`shutdown error: ${e.message}`); }
    process.exit(0);
  };
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
})().catch((e) => {
  console.error(`unexpected failure: ${e && e.message}`);
  process.exit(1);
});
