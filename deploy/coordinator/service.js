#!/usr/bin/env node
// HomeGuardian AI — Production Outbound MCP Coordinator Service Daemon
//
// Milestone: HG-NET-02 (Secure Remote MCP Coordinator)
//
// Runs on the authorized coordination host (Oracle VPS or dedicated VPN gateway).
// Accepts outbound-only WebSocket connections from authorized edge devices (Xiaomi Mi A3),
// exposes an authenticated Streamable HTTP MCP ingress endpoint for authorized clients,
// and enforces strict operation allowlists, subnet confinement, and resource bounds.

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');

const { McpBridgeCoordinator } = require('../../web_mcp/outbound_bridge');

// Optional dotenv loader if coordinator.env exists
const envFile = process.env.COORD_ENV_FILE || '/etc/homeguardian-coordinator/coordinator.env';
if (fs.existsSync(envFile)) {
  try {
    const lines = fs.readFileSync(envFile, 'utf8').split('\n');
    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith('#')) continue;
      const idx = trimmed.indexOf('=');
      if (idx > 0) {
        const k = trimmed.substring(0, idx).trim();
        let v = trimmed.substring(idx + 1).trim();
        if ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'"))) {
          v = v.substring(1, v.length - 1);
        }
        if (!process.env[k]) {
          process.env[k] = v;
        }
      }
    }
  } catch (err) {
    console.warn(`[Coordinator] Warning: unable to parse ${envFile}: ${err.message}`);
  }
}

const CONFIG = {
  bindAddress: process.env.COORD_BIND || '10.42.0.1',
  wsPort: parseInt(process.env.COORD_WS_PORT || '18994', 10),
  httpPort: parseInt(process.env.COORD_HTTP_PORT || '18995', 10),
  devicesFile: process.env.COORD_DEVICES_FILE || '/etc/homeguardian-coordinator/devices.json',
  clientTokenFile: process.env.COORD_CLIENT_TOKEN_FILE || '/etc/homeguardian-coordinator/client_token.secret',
  allowedSubnets: (process.env.COORD_ALLOWED_SUBNETS || '10.42.0.,127.,::1,::ffff:10.42.0.,::ffff:127.')
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean),
};

function readSecureFile(filePath, desc) {
  if (!fs.existsSync(filePath)) {
    throw new Error(`${desc} file not found: ${filePath}`);
  }
  const stat = fs.statSync(filePath);
  const mode = stat.mode & 0o777;
  // In production, reject if world-readable or world-writable (mode & 0007 !== 0)
  if (process.env.NODE_ENV === 'production' && (mode & 0o007) !== 0) {
    throw new Error(`Insecure file permissions on ${filePath} (mode ${mode.toString(8)}); must be 0600 or 0640`);
  }
  return fs.readFileSync(filePath, 'utf8').trim();
}

function loadClientToken() {
  if (process.env.COORD_CLIENT_TOKEN) {
    return process.env.COORD_CLIENT_TOKEN.trim();
  }
  if (fs.existsSync(CONFIG.clientTokenFile)) {
    return readSecureFile(CONFIG.clientTokenFile, 'Client token');
  }
  // Generate and save if writable
  const generated = crypto.randomBytes(32).toString('hex');
  try {
    const dir = path.dirname(CONFIG.clientTokenFile);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
    fs.writeFileSync(CONFIG.clientTokenFile, generated + '\n', { mode: 0o600 });
    console.log(`[Coordinator] Generated initial client token at ${CONFIG.clientTokenFile}`);
    return generated;
  } catch (_) {
    console.warn('[Coordinator] Using ephemeral in-memory client token');
    return generated;
  }
}

function loadDevicesMap() {
  const map = new Map();
  if (!fs.existsSync(CONFIG.devicesFile)) {
    console.log(`[Coordinator] No devices file found at ${CONFIG.devicesFile}; starting with empty registry`);
    return map;
  }

  try {
    const raw = readSecureFile(CONFIG.devicesFile, 'Devices registry');
    const parsed = JSON.parse(raw);
    for (const [deviceId, dev] of Object.entries(parsed)) {
      if (dev && dev.authToken) {
        map.set(deviceId, {
          authToken: dev.authToken,
          name: dev.name || 'Edge Device',
          revoked: !!dev.revoked,
          lastSeen: dev.lastSeen || 0,
          connectedAt: dev.connectedAt || 0,
        });
      }
    }
    console.log(`[Coordinator] Loaded ${map.size} device(s) from ${CONFIG.devicesFile}`);
  } catch (err) {
    console.error(`[Coordinator] Failed to load devices registry: ${err.message}`);
  }
  return map;
}

async function main() {
  console.log('================================================================');
  console.log('  HomeGuardian AI — Remote MCP Bridge Coordinator Daemon');
  console.log('================================================================');

  const clientToken = loadClientToken();
  const authorizedDevices = loadDevicesMap();

  console.log(`[Coordinator] Configuration:`);
  console.log(`  - Bind Interface:     ${CONFIG.bindAddress}`);
  console.log(`  - Edge WS Port:       ${CONFIG.wsPort}`);
  console.log(`  - Client HTTP Port:   ${CONFIG.httpPort}`);
  console.log(`  - Permitted Subnets:  ${CONFIG.allowedSubnets.join(', ')}`);
  console.log(`  - Registered Devices: ${authorizedDevices.size}`);

  const coordinator = new McpBridgeCoordinator({
    bindAddress: CONFIG.bindAddress,
    wsPort: CONFIG.wsPort,
    httpPort: CONFIG.httpPort,
    clientToken,
    allowedSubnets: CONFIG.allowedSubnets,
    authorizedDevices,
  });

  await coordinator.start();
  console.log(`[Coordinator] Ready. Listening on ${CONFIG.bindAddress}:${CONFIG.wsPort} (WS) and ${CONFIG.bindAddress}:${CONFIG.httpPort} (HTTP)`);

  // Handle SIGHUP to reload device authorizations dynamically
  process.on('SIGHUP', () => {
    console.log('[Coordinator] Received SIGHUP. Reloading devices registry...');
    try {
      const refreshed = loadDevicesMap();
      for (const [id, dev] of refreshed.entries()) {
        const existing = coordinator.authorizedDevices.get(id);
        if (existing && existing.revoked !== dev.revoked && dev.revoked) {
          coordinator.revokeDevice(id);
        } else {
          coordinator.authorizeDevice(id, dev.authToken, dev.name);
          if (dev.revoked) {
            coordinator.revokeDevice(id);
          }
        }
      }
      // Check for removed devices
      for (const [id] of coordinator.authorizedDevices.entries()) {
        if (!refreshed.has(id)) {
          coordinator.revokeDevice(id);
          coordinator.authorizedDevices.delete(id);
        }
      }
      console.log(`[Coordinator] Registry reloaded: ${coordinator.authorizedDevices.size} device(s) active.`);
    } catch (err) {
      console.error(`[Coordinator] Error during SIGHUP reload: ${err.message}`);
    }
  });

  // Handle termination signals
  const shutdown = async (signal) => {
    console.log(`\n[Coordinator] Received ${signal}. Shutting down cleanly...`);
    try {
      await coordinator.close();
      console.log('[Coordinator] Shutdown complete.');
      process.exit(0);
    } catch (err) {
      console.error(`[Coordinator] Shutdown error: ${err.message}`);
      process.exit(1);
    }
  };

  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));
}

main().catch((err) => {
  console.error(`[Coordinator] Fatal error: ${err.message}`);
  process.exit(1);
});
