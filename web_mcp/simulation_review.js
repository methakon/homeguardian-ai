'use strict';

// Trusted, operator-supplied executable only. This is a fixture isolation boundary,
// not an OS security sandbox for an untrusted executable. Never enable --http or
// camera flags here. Authentication of the reviewer belongs to the gateway.
// Required native stdio extension (not present in older bridge builds):
//   {op:'scenarios'} -> {ok:true,default,scenarios:[...]}
//   {op:'scenario',id:'normal'} -> scenario_result_json(...) (unwrapped)
// Then the existing authenticated version=1 status/alert_summary contract is
// read in the same child. No HTTP-mode fallback is permitted.
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const { spawn } = require('node:child_process');

class SimulationReviewError extends Error {
  constructor(code, statusCode = 503) {
    super(code);
    this.name = 'SimulationReviewError';
    this.code = code;
    this.status = this.statusCode = statusCode;
    this.tag = 'simulation_review';
  }
}
const fail = (code, status) => new SimulationReviewError(code, status);
const provenance = Object.freeze({ synthetic: true, source: 'native-cpp-synthetic',
  isolated: true, camera_acquisition: false, network_acquisition: false,
  production_store_modified: false });
let occupied = false; // One child across all instances; no unbounded waiting queue.

class SimulationReview {
  constructor({ binaryPath = process.env.HG_REVIEW_BRIDGE_BIN,
    tempRoot = os.tmpdir(), timeoutMs = 15000, maxOutputBytes = 1024 * 1024 } = {}) {
    if (binaryPath !== undefined && binaryPath !== '' && (typeof binaryPath !== 'string' ||
        !path.isAbsolute(binaryPath))) throw fail('simulation_binary_unavailable');
    if (typeof tempRoot !== 'string' || !path.isAbsolute(tempRoot))
      throw fail('simulation_fixture_unavailable');
    if (!Number.isInteger(timeoutMs) || timeoutMs < 10 || timeoutMs > 30000 ||
        !Number.isInteger(maxOutputBytes) || maxOutputBytes < 64 || maxOutputBytes > 1024 * 1024)
      throw fail('simulation_invalid_limits');
    this.binaryPath = binaryPath;
    this.tempRoot = tempRoot;
    this.timeoutMs = timeoutMs;
    this.maxOutputBytes = maxOutputBytes;
  }

  async catalogue() { return this._execute(null); }

  async run(id) {
    if (typeof id !== 'string' || !/^[a-z0-9][a-z0-9-]{0,63}$/.test(id))
      throw fail('invalid_scenario', 400);
    return this._execute(id);
  }

  async _execute(id) {
    if (occupied) throw fail('simulation_busy', 429);
    occupied = true;
    let directory;
    try {
      if (!this.binaryPath) throw fail('simulation_binary_unavailable');
      try {
        const st = await fs.stat(this.binaryPath);
        if (!st.isFile()) throw new Error('not a file');
        await fs.access(this.binaryPath, require('node:fs').constants.X_OK);
      } catch { throw fail('simulation_binary_unavailable'); }
      directory = await fs.mkdtemp(path.join(this.tempRoot, 'hg-review-'));
      await fs.chmod(directory, 0o700);
      const config = path.join(directory, 'config.json');
      const credentials = path.join(directory, 'credentials.json');
      const token = crypto.randomBytes(32).toString('hex');
      await fs.writeFile(config, JSON.stringify({
        // Native synthetic runner requires this gate on. No physical sensor is
        // configured, acquired, or activated; this setting dies with the fixture.
        media_capture_enabled: true, cloud_processing_enabled: false,
        data_dir: path.join(directory, 'data'), server_host: '127.0.0.1',
        server_port: 8080, log_level: 'critical'
      }), { mode: 0o600, flag: 'wx' });
      await fs.writeFile(credentials, JSON.stringify({ [token]: {
        principal: 'isolated-synthetic-review', allow_status: true,
        allow_alert_summary: true, expires_at_ms: 0, revoked: false
      } }), { mode: 0o600, flag: 'wx' });
      return await this._child(directory, config, credentials, token, id);
    } catch (error) {
      if (error instanceof SimulationReviewError) throw error;
      throw fail('simulation_unavailable'); // Never expose native output, paths or credentials.
    } finally {
      try {
        if (directory) await fs.rm(directory, { recursive: true, force: true });
      } catch { throw fail('simulation_cleanup_failed'); }
      finally { occupied = false; }
    }
  }

  _child(directory, config, credentials, token, id) {
    return new Promise((resolve, reject) => {
      const child = spawn(this.binaryPath, [config, credentials], {
        cwd: directory, shell: false, detached: process.platform !== 'win32',
        stdio: ['pipe', 'pipe', 'pipe'],
        // Deliberate allowlist: no inherited HOME/config, camera credentials,
        // proxies, cloud credentials, LD_PRELOAD, NODE_OPTIONS or library paths.
        env: { PATH: '/usr/bin:/bin', LANG: 'C', HOME: directory, TMPDIR: directory,
          HG_BRIDGE_SECURE_CREDS: '1',
          HG_BRIDGE_AUDIT_DB: path.join(directory, 'audit.db'),
          HG_MCP_AUDIT_DB: path.join(directory, 'audit.db') }
      });
      let pending = null, buffer = '', bytes = 0, failure = null, result;
      let finished = false, closed = false, killTimer;
      const signal = sig => {
        if (!child.pid) return;
        try {
          if (process.platform !== 'win32') process.kill(-child.pid, sig);
          else child.kill(sig);
        } catch (e) { if (e.code !== 'ESRCH') child.kill(sig); }
      };
      const stop = error => {
        if (failure || closed) return;
        failure = error;
        if (pending) { pending.reject(error); pending = null; }
        child.stdin.destroy();
        signal('SIGTERM');
        killTimer = setTimeout(() => signal('SIGKILL'), 100);
      };
      const timer = setTimeout(() => stop(fail('simulation_timeout')), this.timeoutMs);
      const account = chunk => {
        bytes += chunk.length;
        if (bytes > this.maxOutputBytes) { stop(fail('simulation_output_limit')); return false; }
        return !failure;
      };
      child.stderr.on('data', account); // Bounded, discarded; never returned to caller.
      child.stdout.on('data', chunk => {
        if (!account(chunk)) return;
        buffer += chunk.toString('utf8');
        let index;
        while ((index = buffer.indexOf('\n')) !== -1 && !failure) {
          const line = buffer.slice(0, index);
          buffer = buffer.slice(index + 1);
          if (!pending || line.length > 256 * 1024) {
            stop(fail('simulation_protocol_error')); break;
          }
          let value;
          try { value = JSON.parse(line); } catch { stop(fail('simulation_protocol_error')); break; }
          const waiter = pending;
          pending = null;
          if (!value || typeof value !== 'object' || Array.isArray(value))
            waiter.reject(fail('simulation_protocol_error'));
          else waiter.resolve(value);
        }
      });
      child.on('error', () => stop(fail('simulation_binary_unavailable')));
      child.stdin.on('error', () => stop(fail('simulation_protocol_error')));
      child.on('close', code => {
        closed = true;
        clearTimeout(timer);
        clearTimeout(killTimer);
        if (pending) { pending.reject(fail('simulation_protocol_error')); pending = null; }
        if (failure) reject(failure);
        else if (!finished || code !== 0 || buffer.trim()) reject(fail('simulation_protocol_error'));
        else resolve(result);
      });
      const request = value => new Promise((res, rej) => {
        const line = JSON.stringify(value) + '\n';
        if (failure) return rej(failure);
        if (pending || Buffer.byteLength(line) > 4096) return rej(fail('simulation_input_limit'));
        pending = { resolve: res, reject: rej };
        child.stdin.write(line);
      });
      (async () => {
        const catalogue = await request({ op: 'scenarios' });
        if (catalogue.ok !== true || !Array.isArray(catalogue.scenarios) ||
            catalogue.scenarios.length < 1 || catalogue.scenarios.length > 100 ||
            new Set(catalogue.scenarios.map(s => s.id)).size !== catalogue.scenarios.length ||
            catalogue.scenarios.some(s => typeof s.id !== 'string' ||
              !/^[a-z0-9][a-z0-9-]{0,63}$/.test(s.id)))
          throw fail('simulation_protocol_error');
        if (id === null) result = { ...catalogue, ...provenance };
        else {
          if (!catalogue.scenarios.some(s => s.id === id)) throw fail('unknown_scenario', 400);
          const scenario = await request({ op: 'scenario', id });
          if (scenario.ok !== true || scenario.synthetic !== true ||
              scenario.scenario_id !== id || scenario.error)
            throw fail('simulation_run_failed');
          const status = await request({ version: 1, operation: 'status', credential: token });
          const summary = await request({ version: 1, operation: 'alert_summary', credential: token });
          if (status.ok !== true || summary.ok !== true ||
              scenario.alerts_delivered !== scenario.alerts_persisted_this_run ||
              scenario.alerts_delivered !== summary.alert_count ||
              scenario.alerts_delivered !== scenario.persisted_alert_count)
            throw fail('simulation_run_failed');
          result = { ok: true, scenario, status, summary, ...provenance };
        }
        finished = true;
        child.stdin.end('shutdown\n');
      })().catch(error => stop(error instanceof SimulationReviewError ? error :
        fail('simulation_protocol_error')));
    });
  }
}

let defaultInstance;
const defaults = () => defaultInstance || (defaultInstance = new SimulationReview());
module.exports = { SimulationReview, SimulationReviewError,
  catalogue: () => defaults().catalogue(), run: id => defaults().run(id) };
