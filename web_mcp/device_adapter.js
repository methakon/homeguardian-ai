// HomeGuardian AI — Authoritative C++ Device Registry Node.js Adapter
//
// Milestone HG-UI-03:
//   - Sourced directly from C++ DeviceRegistry via hg_device_registry_cli
//   - Fall-back validation against canonical devices.json if CLI is unavailable
//   - Zero hardcoded mock arrays

const path = require('node:path');
const fs = require('node:fs');
const { execFileSync } = require('node:child_process');

class DeviceRegistryAdapter {
  constructor(options = {}) {
    const repoRoot = options.repoRoot || path.resolve(__dirname, '..');
    this.cliPath = options.cliPath || path.join(repoRoot, 'build', 'hg_device_registry_cli');
    this.registryPath = options.registryPath || path.join(repoRoot, 'config', 'devices.json');
  }

  // Ensure default registry exists
  ensureRegistry() {
    if (!fs.existsSync(this.registryPath)) {
      const configDir = path.dirname(this.registryPath);
      if (!fs.existsSync(configDir)) {
        fs.mkdirSync(configDir, { recursive: true });
      }
      if (fs.existsSync(this.cliPath)) {
        try {
          execFileSync(this.cliPath, ['--registry', this.registryPath, 'init_defaults']);
          return;
        } catch (err) {
          throw new Error(`authoritative_registry_init_failed: ${err.message}`);
        }
      }
      throw new Error('authoritative_registry_unavailable: registry file absent and hg_device_registry_cli unavailable');
    }
  }

  // List all registered peripheral devices
  listDevices() {
    this.ensureRegistry();

    // Fail-Closed: Execute authoritative C++ binary exclusively
    if (!fs.existsSync(this.cliPath)) {
      throw new Error('authoritative_registry_cli_unavailable: hg_device_registry_cli binary is required');
    }
    const out = execFileSync(this.cliPath, ['--registry', this.registryPath, 'list'], {
      encoding: 'utf-8',
      timeout: 4000,
    });
    return JSON.parse(out);
  }

  // Get a single device
  getDevice(deviceId) {
    const devices = this.listDevices();
    return devices.find((d) => d.id === deviceId) || null;
  }

  // Update consent & capture for a device
  setConsent(deviceId, granted, subject = 'operator', purpose = 'live_view') {
    this.ensureRegistry();

    if (!fs.existsSync(this.cliPath)) {
      throw new Error('authoritative_registry_cli_unavailable: hg_device_registry_cli binary is required');
    }
    const out = execFileSync(
      this.cliPath,
      ['--registry', this.registryPath, 'set_consent', deviceId, String(granted), subject, purpose],
      { encoding: 'utf-8', timeout: 4000 }
    );
    return JSON.parse(out);
  }
}

module.exports = { DeviceRegistryAdapter };
