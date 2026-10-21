# HomeGuardian AI — Remote MCP Coordinator Deployment Package

**Milestone:** HG-NET-02 (Secure Remote MCP Coordinator)  
**Target Environment:** Oracle VPS (WireGuard VPN Gateway `10.42.0.1`, Ubuntu, 1GB RAM)  
**Edge Hardware:** Xiaomi Mi A3 (Android 11, WireGuard VPN Peer `10.42.0.2`)  
**Safety Classification:** Level 1 — Zero Impact on Production Mail Services (Postfix/Dovecot)

---

## 1. Architecture & Security Invariants

The Coordinator acts as the central authenticated rendezvous for HomeGuardian remote control:
1. **Outbound-Only Ingress**: The edge device (Xiaomi Mi A3) dials outbound to `ws://10.42.0.1:18994/`. The coordinator never initiates connections to the phone, preserving NAT traversal and battery.
2. **Mutual Authentication**: The phone authenticates using its provisioned `device_id` and 32-byte hex CSPRNG `auth_token`. Unrecognized or revoked tokens close with WebSocket code `4401` or `4403`.
3. **Subnet Confinement**: WebSocket connections are strictly accepted only from the private VPN subnet (`10.42.0.0/24`) or loopback (`127.0.0.1`). Any attempt to connect from external/public WAN interfaces is immediately dropped with code `4403`.
4. **Operation Allowlist**: Only read-only inspection operations (`initialize`, `tools/list`, `ping`, and `tools/call` for `get_homeguardian_status` and `get_alert_summary`) are bridged. Mutating tools (`set_capture_enabled`, camera controls) and arbitrary methods are rejected fail-closed with HTTP `400`.
5. **No Media Relaying**: The coordinator explicitly rejects all video and audio traffic. Camera streams use dedicated peer-to-peer encrypted channels.
6. **Zero Mail Impact**: On the 1GB RAM Oracle VPS, the coordinator runs under strict cgroup limits (`MemoryMax=120M`, `CPUQuota=25%`, `--max-old-space-size=64`). Mail ports (`25`, `587`, `993`, `110`) are untouched.

---

## 2. Package File Manifest

| File | Purpose | Security Controls |
|---|---|---|
| `service.js` | Production standalone coordinator daemon | Signal handling (`SIGHUP`, `SIGTERM`), zero-secret logging, dynamic reload |
| `coordinator.service` | Hardened systemd unit file | `MemoryMax=120M`, `CPUQuota=25%`, `ProtectSystem=strict`, `NoNewPrivileges=true` |
| `manage_devices.sh` | CLI device enrollment, revocation, and token rotation | CSPRNG token generation (`openssl rand -hex 32`), `0600` permissions |
| `healthcheck.sh` | Health check and monitoring utility | Reports coordinator status, truthful edge state, RAM/CPU, and mail health |
| `install.sh` | Safe installation script | Pre-flight auditing, mail port verification, `--dry-run` support |
| `rollback.sh` | Clean uninstallation script | Stops service, cleans files, archives config to `/root`, verifies released ports |
| `test_package.sh` | Self-contained package test | Runs automated end-to-end tests in an isolated sandbox |

---

## 3. Deployment & Operational Workflow

### Step 1: Pre-Flight Audit (Dry-Run)
Run the dry-run installer to verify Node.js, available RAM, and mail isolation:
```bash
./install.sh --dry-run
```

### Step 2: Live Installation (Root)
```bash
sudo ./install.sh
```

### Step 3: Enrolling the Xiaomi Mi A3
Generate an enrollment token for the Mi A3 hardware:
```bash
sudo ./manage_devices.sh enroll "mi_a3_prod_001" "Xiaomi Mi A3 Android 11"
```
*Note the returned 64-character token and provision it securely to the Android EdgeService.*

### Step 4: Health & Edge Status Verification
Check coordinator health and edge connectivity:
```bash
./healthcheck.sh
```
When the Mi A3 is disconnected:
```
[Edge Hardware State]  DEVICE STATUS: LOCAL SYNTHETIC FIXTURE — MI A3 OFFLINE
```
When the Mi A3 connects over WireGuard:
```
[Edge Hardware State]  DEVICE STATUS: ONLINE (device_id: mi_a3_prod_001)
```

### Step 5: Revoking Compromised Credentials
If a device is lost or compromised:
```bash
sudo ./manage_devices.sh revoke "mi_a3_prod_001"
```
The coordinator immediately terminates active WebSocket connections with code `4403` and blocks all reconnection attempts.

### Step 6: Safe Rollback
To cleanly remove the coordinator and verify mail service integrity:
```bash
sudo ./rollback.sh
```
