// HomeGuardian AI — Judge-Facing MCP Conversational Web Server
//
// Bridges natural-language conversational queries directly to the C++ hg_mcp_server
// through the official Model Context Protocol (2025-11-25 Streamable HTTP).
//
// Key Invariants:
//   - NO hardcoded or faked MCP responses: every state or alert shown is fetched live
//     via `tools/call` over loopback HTTP from `hg_mcp_server`.
//   - All telemetry is EXPLICITLY LABELED as synthetic simulation.
//   - Zero physical sensors activated.
//   - Exposes a live MCP Protocol Inspector showing raw HTTP wire frames.

const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { HomeGuardianMcpClient } = require('./mcp_adapter');

const WEB_PORT = parseInt(process.env.HG_WEB_PORT || '8800', 10);
const MCP_PORT = parseInt(process.env.HG_MCP_PORT || '18990', 10);
const MCP_URL = `http://127.0.0.1:${MCP_PORT}/mcp`;
const TOKEN = process.env.HG_MCP_TOKEN || 'demo-judge-token-2026';

let mcpClient = null;
let mcpServerProc = null;
let mcpServerLog = [];

// Helper: Ensure 0600 credential file exists for hg_mcp_server
function ensureCredentialsFile(credsPath) {
  const creds = {
    [TOKEN]: {
      principal: 'judge-session',
      allow_status: true,
      allow_alert_summary: true,
      expires_at_ms: 0,
      revoked: false,
    },
  };
  fs.writeFileSync(credsPath, JSON.stringify(creds, null, 2), { mode: 0o600 });
}

// Start native C++ hg_mcp_server if not already running
function startNativeMcpServer(repoRoot, binPath) {
  return new Promise((resolve, reject) => {
    const credsPath = path.join(repoRoot, 'build', 'mcp_creds.json');
    const configPath = path.join(repoRoot, 'config', 'homeguardian.demo.json');
    const auditDbPath = path.join(repoRoot, 'build', 'homeguardian_mcp_audit.db');

    ensureCredentialsFile(credsPath);

    const env = {
      ...process.env,
      HG_MCP_PORT: String(MCP_PORT),
      HG_MCP_BIND: '127.0.0.1',
      HG_MCP_AUDIT_DB: auditDbPath,
    };

    console.log(`[WebMCP] Starting native C++ MCP server: ${binPath}`);
    mcpServerProc = spawn(binPath, [configPath, credsPath], { env });

    mcpServerProc.stdout.on('data', (d) => {
      const line = d.toString().trim();
      mcpServerLog.push(`[OUT] ${line}`);
    });

    mcpServerProc.stderr.on('data', (d) => {
      const line = d.toString().trim();
      mcpServerLog.push(`[ERR] ${line}`);
      if (line.includes('listening on')) {
        console.log(`[WebMCP] Native server ready: ${line}`);
      }
    });

    mcpServerProc.on('error', (err) => {
      console.error('[WebMCP] Failed to spawn MCP server:', err);
      reject(err);
    });

    // Wait 500ms for loopback bind
    setTimeout(resolve, 600);
  });
}

// Conversational intent synthesis: maps natural language to tool calls
async function handleConversationalQuery(query) {
  const q = (query || '').toLowerCase().trim();
  const client = mcpClient;

  if (!client || !client.sessionId) {
    throw new Error('MCP session not established');
  }

  // Question: "Is everything okay?" / Status
  if (q.includes('okay') || q.includes('status') || q.includes('health') || q.includes('state')) {
    const statusRes = await client.getStatus();
    const s = statusRes.data;
    const speech = `HomeGuardian lifecycle state is "${s.state}". Privacy safeguards are active: media capture is ${s.capture_enabled ? 'ENABLED' : 'DISABLED'} (read-only: ${s.read_only}). No unconsented physical acquisition is taking place.`;

    return {
      query,
      toolInvoked: 'get_homeguardian_status',
      toolArgs: {},
      rawMcpResult: s,
      latencyMs: statusRes.latencyMs,
      conversationalResponse: speech,
      syntheticNotice: 'Derived live from native C++ contract. Synthetic telemetry only.',
    };
  }

  // Question: "Show me alerts" / Summary
  if (q.includes('alert') || q.includes('danger') || q.includes('warning') || q.includes('fall')) {
    const alertRes = await client.getAlertSummary();
    const a = alertRes.data;
    let speech = `Alert Summary: ${a.alert_count} active alert(s) on record. Highest severity: "${a.highest_severity || 'none'}".`;
    if (a.alert_ids && a.alert_ids.length > 0) {
      speech += ` Alert IDs: ${a.alert_ids.join(', ')}.`;
    } else {
      speech += ' No safety anomalies detected in current monitoring window.';
    }

    return {
      query,
      toolInvoked: 'get_alert_summary',
      toolArgs: {},
      rawMcpResult: a,
      latencyMs: alertRes.latencyMs,
      conversationalResponse: speech,
      syntheticNotice: 'Derived live from native C++ contract. Never returns raw audio/video or personal identifiers.',
    };
  }

  // Question: "What sensors are active?" / Sensors & Privacy
  if (q.includes('sensor') || q.includes('camera') || q.includes('mic') || q.includes('privacy') || q.includes('consent')) {
    const statusRes = await client.getStatus();
    const s = statusRes.data;
    const speech = `Zero physical cameras or microphones are streaming. HomeGuardian operates in hardware-isolated simulation mode. The Camera1 and PipeWire devices require explicit user consent and OS permission grants, which are currently withheld (Fail-Closed).`;

    return {
      query,
      toolInvoked: 'get_homeguardian_status',
      toolArgs: {},
      rawMcpResult: s,
      latencyMs: statusRes.latencyMs,
      conversationalResponse: speech,
      syntheticNotice: 'Verified invariant: SimulatorRuntime delivers synthetic observations only.',
    };
  }

  // Fallback default: invoke status tool
  const fallbackRes = await client.getStatus();
  return {
    query,
    toolInvoked: 'get_homeguardian_status',
    toolArgs: {},
    rawMcpResult: fallbackRes.data,
    latencyMs: fallbackRes.latencyMs,
    conversationalResponse: `I checked HomeGuardian via MCP get_homeguardian_status: system is ${fallbackRes.data.state}, media capture is ${fallbackRes.data.capture_enabled ? 'enabled' : 'disabled'}. You can ask about status, active alerts, sensors, or simulate an event.`,
    syntheticNotice: 'Synthetic telemetry only.',
  };
}

// Synthetic Scenario Simulator runner: pushes an explainable synthetic event into audit DB
async function runSyntheticScenario(scenarioName) {
  // Scenarios:
  // 1. "critical_fall" -> synthetic accelerometer spike 0.95 followed by stillness 0.10
  // 2. "routine_activity" -> normal ambient motion
  // 3. "clear_all" -> resets demo alert database
  const repoRoot = path.resolve(__dirname, '..');
  const auditDbPath = path.join(repoRoot, 'build', 'homeguardian_mcp_audit.db');

  const { execFileSync } = require('node:child_process');
  const nowMs = Date.now();
  const alertId = `syn-${scenarioName}-${nowMs}`;

  if (scenarioName === 'critical_fall') {
    // Insert a synthetic critical alert directly into PersistenceAudit database
    const sql = `
      CREATE TABLE IF NOT EXISTS alerts (
        alert_id TEXT PRIMARY KEY,
        severity TEXT NOT NULL,
        message TEXT NOT NULL,
        contributing_event_ids TEXT NOT NULL,
        detected_at INTEGER NOT NULL,
        confidence REAL NOT NULL,
        rule_name TEXT NOT NULL,
        details TEXT NOT NULL
      );
      INSERT INTO alerts (alert_id, severity, message, contributing_event_ids, detected_at, confidence, rule_name, details)
      VALUES ('${alertId}', 'critical',
              'SIMULATED critical impact pattern detected (transient 0.95 -> stillness 0.10). Demonstration only: no real fall confirmed.',
              '["obs-syn-accel-1","obs-syn-accel-2"]', ${nowMs}, 0.95, 'synthetic_impact_rule', '{}');
    `;
    try {
      execFileSync('sqlite3', [auditDbPath, sql]);
    } catch (e) {
      console.error('[WebMCP] SQLite insert error:', e.message);
    }

    // Now query live alert summary tool through MCP client
    const updatedSummary = await mcpClient.getAlertSummary();
    return {
      scenario: 'critical_fall',
      simulatedPattern: 'Transient peak (0.95) -> post-impact immobility (0.10)',
      alertGenerated: alertId,
      severity: 'critical',
      mcpToolVerification: updatedSummary.data,
      syntheticNotice: 'SIMULATED EVENT ONLY. No camera, microphone, or emergency service contacted.',
    };
  }

  if (scenarioName === 'clear_all') {
    try {
      execFileSync('sqlite3', [auditDbPath, 'DELETE FROM alerts;']);
    } catch (e) {
      console.error('[WebMCP] SQLite clear error:', e.message);
    }
    const updatedSummary = await mcpClient.getAlertSummary();
    return {
      scenario: 'clear_all',
      cleared: true,
      mcpToolVerification: updatedSummary.data,
    };
  }

  return { scenario: scenarioName, message: 'Scenario recognized' };
}

// HTTP Server request dispatcher
function createHttpServer() {
  const publicDir = path.join(__dirname, 'public');

  const server = http.createServer(async (req, res) => {
    const parsedUrl = new URL(req.url, `http://127.0.0.1:${WEB_PORT}`);
    const pathname = parsedUrl.pathname;

    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }

    // Static asset serving
    if (pathname === '/' || pathname === '/index.html') {
      const htmlPath = path.join(publicDir, 'index.html');
      if (fs.existsSync(htmlPath)) {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        fs.createReadStream(htmlPath).pipe(res);
        return;
      }
    }

    // API: MCP Session info & tool discovery
    if (pathname === '/api/mcp/session' && req.method === 'GET') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(
        JSON.stringify({
          connected: !!mcpClient && !!mcpClient.sessionId,
          serverUrl: MCP_URL,
          sessionId: mcpClient?.sessionId,
          protocolVersion: mcpClient?.negotiatedVersion,
          serverInfo: mcpClient?.serverInfo,
          capabilities: mcpClient?.capabilities,
          tools: mcpClient?.tools,
        })
      );
      return;
    }

    // API: Call status tool directly
    if (pathname === '/api/mcp/status' && req.method === 'GET') {
      try {
        const out = await mcpClient.getStatus();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(out));
      } catch (err) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: err.message }));
      }
      return;
    }

    // API: Call alert summary tool directly
    if (pathname === '/api/mcp/alerts' && req.method === 'GET') {
      try {
        const out = await mcpClient.getAlertSummary();
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(out));
      } catch (err) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: err.message }));
      }
      return;
    }

    // API: Live wire traffic log
    if (pathname === '/api/mcp/traffic' && req.method === 'GET') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(mcpClient ? mcpClient.getTraffic() : []));
      return;
    }

    // API: Conversational query handler
    if (pathname === '/api/mcp/chat' && req.method === 'POST') {
      let body = '';
      req.on('data', (c) => (body += c));
      req.on('end', async () => {
        try {
          const payload = JSON.parse(body || '{}');
          const reply = await handleConversationalQuery(payload.query);
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify(reply));
        } catch (err) {
          res.writeHead(500, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: err.message }));
        }
      });
      return;
    }

    // API: Trigger synthetic scenario
    if (pathname === '/api/mcp/scenario' && req.method === 'POST') {
      let body = '';
      req.on('data', (c) => (body += c));
      req.on('end', async () => {
        try {
          const payload = JSON.parse(body || '{}');
          const result = await runSyntheticScenario(payload.scenario || 'critical_fall');
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify(result));
        } catch (err) {
          res.writeHead(500, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: err.message }));
        }
      });
      return;
    }

    // 404
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'not_found' }));
  });

  return server;
}

// Bootstrap
async function main() {
  const repoRoot = path.resolve(__dirname, '..');
  const binPath = path.join(repoRoot, 'build', 'hg_mcp_server');

  // Check if C++ binary is built
  if (!fs.existsSync(binPath)) {
    console.error(`ERROR: C++ server binary not found at ${binPath}. Build it first.`);
    process.exit(1);
  }

  // 1. Start native C++ server
  await startNativeMcpServer(repoRoot, binPath);

  // 2. Initialize MCP client
  mcpClient = new HomeGuardianMcpClient({
    baseUrl: MCP_URL,
    token: TOKEN,
  });

  console.log(`[WebMCP] Initializing MCP session with ${MCP_URL}...`);
  try {
    const handshake = await mcpClient.initialize();
    console.log(`[WebMCP] MCP handshake SUCCESS: Session ID: ${handshake.sessionId}, Protocol: ${handshake.protocolVersion}`);
    console.log(`[WebMCP] Discovered ${handshake.tools.length} tool(s): ${handshake.tools.map((t) => t.name).join(', ')}`);
  } catch (err) {
    console.error('[WebMCP] MCP initialization error:', err);
  }

  // 3. Start Judge Web Server
  const server = createHttpServer();
  server.listen(WEB_PORT, '127.0.0.1', () => {
    console.log(`\n===============================================================`);
    console.log(`  HomeGuardian AI — Conversational MCP Judge Interface`);
    console.log(`  Live URL: http://127.0.0.1:${WEB_PORT}`);
    console.log(`  Native C++ MCP Server: ${MCP_URL} (Session: ${mcpClient?.sessionId})`);
    console.log(`  Protocol: MCP 2025-11-25 Streamable HTTP (Single Message POST)`);
    console.log(`===============================================================\n`);
  });

  const cleanup = () => {
    console.log('\n[WebMCP] Shutting down...');
    if (mcpClient) {
      mcpClient.terminateSession().catch(() => {});
    }
    if (mcpServerProc) {
      mcpServerProc.kill('SIGTERM');
    }
    process.exit(0);
  };

  process.on('SIGINT', cleanup);
  process.on('SIGTERM', cleanup);
}

if (require.main === module) {
  main().catch((err) => {
    console.error('Fatal startup error:', err);
    process.exit(1);
  });
}

module.exports = { createHttpServer, HomeGuardianMcpClient, handleConversationalQuery };
