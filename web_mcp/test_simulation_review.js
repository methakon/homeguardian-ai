#!/usr/bin/env node
'use strict';
// node --test web_mcp/test_simulation_review.js
// Integration never skips: requires the real, current native hg_alexa_bridge.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { SimulationReview, SimulationReviewError } = require('./simulation_review');
const binaryPath = process.env.HG_REVIEW_BRIDGE_BIN ||
  path.resolve(__dirname, '../hg_build/hg_alexa_bridge');
const scratch = process.env.TMPDIR || os.tmpdir();
const tagged = (code, status = 503) => e => e instanceof SimulationReviewError &&
  e.code === code && e.statusCode === status && !e.message.includes('/') &&
  e.tag === 'simulation_review';
async function fixture(fn) {
  const root = await fs.mkdtemp(path.join(scratch, 'hg-review-test-'));
  try { await fn(root); } finally { await fs.rm(root, { recursive: true, force: true }); }
}
async function empty(root) { assert.deepEqual(await fs.readdir(root), [], 'fixture leaked'); }

// These helpers are adversarial processes ONLY for failure-path checks. They
// never synthesize catalogue/scenario JSON. All successful results below must
// come from the real native C++ executable.
async function adversary(root, body) {
  const executable = path.join(root, 'guard-probe');
  await fs.writeFile(executable, '#!' + process.execPath + '\n' + body, { mode: 0o700 });
  const tempRoot = path.join(root, 'fixtures');
  await fs.mkdir(tempRoot);
  return { binaryPath: executable, tempRoot };
}

test('missing executable is a tagged, fail-closed 503 with no fixtures', async () => {
  await fixture(async root => {
    const service = new SimulationReview({ binaryPath: path.join(root, 'absent'), tempRoot: root });
    await assert.rejects(service.catalogue(), tagged('simulation_binary_unavailable'));
    const disabled = new SimulationReview({ binaryPath: '', tempRoot: root });
    await assert.rejects(disabled.catalogue(), tagged('simulation_binary_unavailable'));
    await empty(root);
  });
});

test('input validation rejects paths, NDJSON injection, objects and oversized IDs before spawn', async () => {
  await fixture(async root => {
    const service = new SimulationReview({ binaryPath, tempRoot: root });
    for (const id of ['', '../normal', 'normal\nshutdown', '--http', {}, null, 2, 'x'.repeat(65)])
      await assert.rejects(service.run(id), tagged('invalid_scenario', 400));
    await empty(root);
  });
});

test('wall deadline kills and reaps a TERM-resistant child; concurrency is global', async () => {
  await fixture(async root => {
    const setup = await adversary(root, `
      const fs = require('node:fs');
      fs.writeFileSync(${JSON.stringify(path.join(root, 'pid'))}, String(process.pid));
      process.on('SIGTERM', () => {});
      setInterval(() => {}, 1000);
    `);
    const service = new SimulationReview({ ...setup, timeoutMs: 250 });
    const running = service.catalogue();
    // Register a rejection handler immediately to avoid unhandled rejection races.
    const rejection = assert.rejects(running, tagged('simulation_timeout'));
    await assert.rejects(new SimulationReview(setup).catalogue(), tagged('simulation_busy', 429));
    await rejection;
    const pid = Number(await fs.readFile(path.join(root, 'pid'), 'utf8'));
    assert.throws(() => process.kill(pid, 0), { code: 'ESRCH' });
    await empty(setup.tempRoot);
    await assert.rejects(service.catalogue(), tagged('simulation_timeout'));
    await empty(setup.tempRoot);
  });
});

test('combined native output is bounded and sandbox is removed on overflow', async () => {
  await fixture(async root => {
    const setup = await adversary(root, "process.stderr.write('x'.repeat(8192)); setInterval(() => {}, 1000);");
    await assert.rejects(new SimulationReview({ ...setup, maxOutputBytes: 1024 }).catalogue(),
      tagged('simulation_output_limit'));
    await empty(setup.tempRoot);
  });
});

test('child receives no inherited secrets, config, acquisition flags, shell or public listener', async () => {
  await fixture(async root => {
    const setup = await adversary(root, `
      const fs = require('node:fs');
      const path = require('node:path');
      const [config, creds] = process.argv.slice(2);
      const report = { envKeys: Object.keys(process.env).sort(), args: process.argv.slice(2),
        cwd: process.cwd(), config: JSON.parse(fs.readFileSync(config)),
        configMode: fs.statSync(config).mode & 511,
        credsMode: fs.statSync(creds).mode & 511,
        dirMode: fs.statSync(process.cwd()).mode & 511 };
      fs.writeFileSync(${JSON.stringify(path.join(root, 'report'))}, JSON.stringify(report));
      // No fabricated protocol output: adapter must fail closed on EOF.
    `);
    await assert.rejects(new SimulationReview(setup).catalogue(), tagged('simulation_protocol_error'));
    const report = JSON.parse(await fs.readFile(path.join(root, 'report'), 'utf8'));
    assert.deepEqual(report.envKeys, ['HG_BRIDGE_AUDIT_DB', 'HG_BRIDGE_SECURE_CREDS',
      'HG_MCP_AUDIT_DB', 'HOME', 'LANG', 'PATH', 'TMPDIR'].sort());
    assert.equal(report.args.length, 2);
    assert(report.args.every(arg => arg.startsWith(report.cwd + '/')));
    assert.equal(report.config.media_capture_enabled, true);
    assert.equal(report.config.cloud_processing_enabled, false);
    assert(report.config.data_dir.startsWith(report.cwd + '/'));
    assert.equal(report.configMode, 0o600);
    assert.equal(report.credsMode, 0o600);
    assert.equal(report.dirMode, 0o700);
    await empty(setup.tempRoot);
  });
});

test('REAL native catalogue, all ten scenarios, durable summaries and cross-run isolation', async t => {
  await fixture(async root => {
    const sentinel = path.join(root, 'production-sentinel');
    await fs.writeFile(sentinel, 'untouched');
    const tempRoot = path.join(root, 'fixtures');
    await fs.mkdir(tempRoot);
    const service = new SimulationReview({ binaryPath, tempRoot });
    const catalogue = await service.catalogue();
    assert.equal(catalogue.ok, true);
    assert.equal(catalogue.synthetic, true);
    assert.equal(catalogue.source, 'native-cpp-synthetic');
    assert.equal(catalogue.scenarios.length, 10);
    assert.equal(new Set(catalogue.scenarios.map(s => s.id)).size, 10);
    assert(catalogue.scenarios.some(s => s.id === catalogue.default));
    await empty(tempRoot);
    await assert.rejects(service.run('not-a-scenario'), tagged('unknown_scenario', 400));
    await empty(tempRoot);
    for (const entry of catalogue.scenarios) {
      await t.test(entry.id, async () => {
        const response = await service.run(entry.id);
        const result = response.scenario;
        assert.equal(result.ok, true);
        assert.equal(result.synthetic, true);
        assert.equal(result.scenario_id, entry.id);
        assert.equal(response.status.ok, true);
        assert.equal(response.summary.ok, true);
        assert.equal(response.isolated, true);
        assert.equal(response.camera_acquisition, false);
        assert.equal(response.network_acquisition, false);
        assert.equal(result.capture_enabled, true); // Synthetic fixture gate only.
        assert.equal(result.alerts_delivered, result.alerts.length);
        assert.equal(result.alerts_persisted_this_run, result.alerts_delivered);
        assert.equal(result.persisted_alert_count, result.alerts_delivered);
        assert.equal(response.summary.alert_count, result.alerts_delivered);
        assert.deepEqual([...response.summary.alert_ids].sort(),
          result.alerts.map(a => a.alert_id).sort());
        assert(result.alerts.every(a => a.provenance === 'synthetic'));
        if (['normal', 'inconclusive', 'consent-denied'].includes(entry.id))
          assert.equal(result.alerts_delivered, 0);
        if (['default', 'unusual', 'synthetic-critical-impact'].includes(entry.id))
          assert(result.alerts_delivered > 0);
        if (entry.id === 'normal') assert.equal(result.detection.fusion_verdict, 'not_detected');
        if (entry.id === 'unusual') assert.equal(result.detection.fusion_verdict, 'detected');
        if (entry.id === 'synthetic-critical-impact') assert.equal(response.summary.highest_severity, 'critical');
        t.diagnostic(entry.id + ': alerts=' + result.alerts_delivered + ', cycles=' + result.cycles);
        await empty(tempRoot);
      });
    }
    // A positive run cannot leak alerts into the next normal run.
    await service.run('unusual');
    const normal = await service.run('normal');
    assert.equal(normal.summary.alert_count, 0);
    assert.equal(normal.scenario.persisted_alert_count, 0);
    assert.equal(await fs.readFile(sentinel, 'utf8'), 'untouched');
    assert.deepEqual((await fs.readdir(root)).sort(), ['fixtures', 'production-sentinel']);
    await empty(tempRoot);
  });
});
