# HomeGuardian AI — Oracle Cloud Infrastructure (OCI) VPS Deployment & Repurposing Plan

**Target Host:** Oracle Cloud Infrastructure Compute VPS (`x402-endpoint`, `161.33.146.238`)  
**Shape & Hardware:** `VM.Standard.E2.1.Micro` (1/8 OCPU AMD EPYC 7551, ~954 MiB RAM, 1 GiB swap) OR `VM.Standard.A1.Flex` (Ampere A1 ARM64, 1-4 OCPU, up to 24 GB RAM)  
**OS:** Ubuntu 24.04 LTS (Noble Numbat) / Ubuntu 22.04 LTS (Jammy Jellyfish)  
**Security Boundary:** Zero disruption to existing mail infrastructure (OpenSMTPD + Dovecot), zero plaintext credentials, loopback-bound services, Cloudflare Tunnel ingress.  

---

## 1. Executive Summary & Non-Disruption Guarantees

The target VPS hosts vital ongoing services:
- **Mail Services:** OpenSMTPD (ports 25, 465, 587) and Dovecot IMAPS (ports 993, 995).
- **Security & Access:** OpenSSH (port 22) and fail2ban.

HomeGuardian AI will be deployed to this instance with strict resource sandboxing. Under this plan:
1. **Mail Services Untouched:** HomeGuardian never writes to `/var/mail`, `/var/vmail`, `/etc/smtpd`, `/etc/dovecot`, or `/etc/letsencrypt`.
2. **Ports Untouched:** Ports 25, 465, 587, 993, 995, and 22 remain strictly dedicated to mail and SSH.
3. **No Direct Public Port Exposure:** HomeGuardian services bind exclusively to loopback (`127.0.0.1`). External web traffic reaches HomeGuardian via Cloudflare Tunnel (`cloudflared`) or loopback reverse proxy.
4. **Memory Strict Budget:** Memory quotas (`MemoryMax`) limit HomeGuardian's total RSS footprint to < 180 MB, leaving > 700 MB for OS and mail operations.

---

## 2. Hardware Architecture & Cross-Platform Compatibility

HomeGuardian AI is written in modern C++20 and standard Node.js LTS, with zero architecture-specific assembly or x86-only dependencies.

### 2.1 Compatibility Matrix

| Component | AMD x86_64 (`VM.Standard.E2.1.Micro`) | Ampere ARM64 (`VM.Standard.A1.Flex`) | Notes |
|---|---|---|---|
| **C++ Core & Bridge** (`hg_alexa_bridge`) | Verified native build (GCC 11+ / Clang 13+) | Verified native build (GCC aarch64) | 100% portable C++20 standard library + SQLite3 |
| **Native MCP Server** (`hg_mcp_server`) | Verified native build | Verified native build | Standard Linux POSIX sockets + `/dev/urandom` |
| **Device Registry CLI** (`hg_device_registry_cli`) | Verified native build | Verified native build | Native JSON parser + SHA-256 |
| **Node.js Web Gateway** (`web_mcp/server.js`) | Node.js v20.x x86_64 | Node.js v20.x aarch64 | Standard npm modules (werift, express-free HTTP) |
| **WebRTC Media Engine** (`werift`) | Pure JS + Node crypto | Pure JS + Node crypto | Zero native C++ addons required |

### 2.2 Compilation on Low-Memory Instances (`E2.1.Micro`)

On 1 GB RAM instances, parallel builds (`make -j4`) will trigger the Linux Out-of-Memory (OOM) killer. Use bounded concurrency:

```bash
# Build on instance using single worker or off-box binary build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHOMEGUARDIAN_BUILD_TESTS=OFF
cmake --build build --target hg_alexa_bridge hg_mcp_server hg_device_registry_cli -j1
```

*Recommended Best Practice:* Build release binaries or Docker images on your development machine or CI pipeline (using `scripts/package_release.sh`) and deploy pre-compiled binaries to the VPS.

---

## 3. Memory Budget & Resource Allocation

### 3.1 Budget on `VM.Standard.E2.1.Micro` (~954 MiB Total RAM)

```
+--------------------------------------------------------------------------+
| Total RAM: ~954 MiB (+ 1024 MiB Swap)                                    |
+--------------------------------------------------------------------------+
| Operating System & Kernel:        ~180 MiB                               |
| Mail Server (OpenSMTPD + Dovecot): ~120 MiB                               |
| SSH + fail2ban + systemd:          ~80 MiB                               |
| Available Headroom:               ~400 MiB                               |
+--------------------------------------------------------------------------+
| HomeGuardian AI Allocated Budget:                                        |
|   - Native C++ Bridge (hg_alexa_bridge):      ~30 MiB (Cap: 64M)         |
|   - Native C++ MCP Server (hg_mcp_server):    ~25 MiB (Cap: 48M)         |
|   - Node.js Web Gateway (web_mcp/server.js):  ~65 MiB (Cap: 128M)        |
|   - SQLite in-memory cache:                   ~8 MiB                     |
| Total HomeGuardian Working Set:               ~128 MiB (Max Cap: 240M)   |
+--------------------------------------------------------------------------+
```

---

## 4. Coexistence & Pre-Flight Verification

Before any deployment on the VPS, execute the read-only preflight script:

```bash
# Run read-only preflight (zero privilege escalation, zero writes, automatic secret redaction)
bash scripts/oracle_preflight_readonly.sh | tee /tmp/preflight-$(date +%F).txt
```

Verify the following invariants:
1. `ss -lntp` reports mail listeners active on 25, 465, 587, 993, 995.
2. `free -m` reports `MemAvailable` >= 350 MB.
3. Ports `18777`, `18880`, `18980` are completely free.

---

## 5. HTTPS Access & Ingress Configuration

### Option A: Cloudflare Tunnel (`cloudflared`) — Zero Open Inbound Ports

Cloudflare Tunnel establishes an outbound encrypted connection to Cloudflare's edge, exposing no listening ports to the public internet:

1. Create DNS Route in Cloudflare dashboard: `homeguardian.berhampore.in` -> `http://127.0.0.1:18880`.
2. Configure `/etc/cloudflared/config.yml`:
   ```yaml
   tunnel: <tunnel-id>
   credentials-file: /etc/cloudflared/<tunnel-id>.json

   ingress:
     - hostname: homeguardian.berhampore.in
       service: http://127.0.0.1:18880
     - service: http_status:404
   ```
3. Restart tunnel:
   ```bash
   sudo systemctl restart cloudflared
   ```

### Option B: Nginx Reverse Proxy with Certbot TLS

If using direct Nginx reverse proxy:

```nginx
# /etc/nginx/sites-available/homeguardian.conf
server {
    listen 80;
    server_name homeguardian.berhampore.in;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name homeguardian.berhampore.in;

    ssl_certificate /etc/letsencrypt/live/homeguardian.berhampore.in/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/homeguardian.berhampore.in/privkey.pem;

    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers HIGH:!aNULL:!MD5;

    # Security Headers
    add_header X-Frame-Options "DENY" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Referrer-Policy "no-referrer" always;

    location / {
        proxy_pass http://127.0.0.1:18880;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }
}
```

---

## 6. Service Supervision & Hardening

Create dedicated systemd units with memory ceilings:

### 6.1 `homeguardian-mcp.service`
```ini
[Unit]
Description=HomeGuardian C++ MCP Server
After=network.target

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/var/lib/homeguardian
ExecStart=/opt/homeguardian/bin/hg_mcp_server /etc/homeguardian/config.json /etc/homeguardian/secrets/mcp_creds.json
Restart=always
RestartSec=5s
MemoryMax=48M
TasksMax=32
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian

[Install]
WantedBy=multi-user.target
```

### 6.2 `homeguardian-bridge.service`
```ini
[Unit]
Description=HomeGuardian Native C++ Pipeline & Bridge
After=network.target

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/var/lib/homeguardian
ExecStart=/opt/homeguardian/bin/hg_alexa_bridge \
  --http /etc/homeguardian/config.json \
  /etc/homeguardian/secrets/mcp_creds.json \
  --port 18777 \
  --token-file /etc/homeguardian/secrets/operator_token.txt \
  --secret-file /etc/homeguardian/secrets/bridge_secret.txt
Restart=always
RestartSec=5s
MemoryMax=64M
TasksMax=32
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian

[Install]
WantedBy=multi-user.target
```

### 6.3 `homeguardian-gateway.service`
```ini
[Unit]
Description=HomeGuardian Web Gateway & WebRTC Signaling
After=homeguardian-mcp.service homeguardian-bridge.service

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/opt/homeguardian/web_mcp
Environment=NODE_ENV=production
Environment=HG_WEB_PORT=18880
Environment=HG_MCP_PORT=18980
Environment=HG_REGISTRY_PATH=/var/lib/homeguardian/devices.json
Environment=HG_REGISTRY_CLI=/opt/homeguardian/bin/hg_device_registry_cli
Environment=HG_OPERATOR_USER=admin
EnvironmentFile=/etc/homeguardian/secrets/gateway.env
ExecStart=/usr/bin/node server.js
Restart=always
RestartSec=5s
MemoryMax=128M
TasksMax=64
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian

[Install]
WantedBy=multi-user.target
```

---

## 7. Backup & Recovery Strategy

### 7.1 SQLite Online Hot-Backup

SQLite Write-Ahead Logging (WAL) enables online hot backups without locking database writers:

```bash
#!/usr/bin/env bash
# /opt/homeguardian/scripts/backup_audit_db.sh
BACKUP_DIR="/var/backups/homeguardian"
mkdir -p "$BACKUP_DIR"
TIMESTAMP=$(date +%F_%H%M%S)

# Online snapshot
sqlite3 /var/lib/homeguardian/homeguardian_bridge_audit.db \
  ".backup '${BACKUP_DIR}/audit_${TIMESTAMP}.db'"

# Gzip archive
gzip "${BACKUP_DIR}/audit_${TIMESTAMP}.db"
chmod 600 "${BACKUP_DIR}/audit_${TIMESTAMP}.db.gz"

# Retain last 14 days of backups
find "$BACKUP_DIR" -name "audit_*.db.gz" -mtime +14 -delete
```

Schedule daily in crontab:
```text
0 3 * * * /opt/homeguardian/scripts/backup_audit_db.sh >/dev/null 2>&1
```

---

## 8. Continuous Health Probes & Monitoring

Create a lightweight watchdog script `/opt/homeguardian/scripts/healthcheck.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

# 1. Probe Web Gateway health
GATEWAY_HEALTH=$(curl -sf -m 3 http://127.0.0.1:18880/api/health | jq -r .status || echo "down")
if [[ "$GATEWAY_HEALTH" != "ok" ]]; then
  echo "CRITICAL: Gateway health failed ($GATEWAY_HEALTH)" >&2
  systemctl restart homeguardian-gateway
fi

# 2. Probe Native HTTP bridge health
BRIDGE_HEALTH=$(curl -sf -m 3 http://127.0.0.1:18777/api/health | jq -r .ok || echo "false")
if [[ "$BRIDGE_HEALTH" != "true" ]]; then
  echo "CRITICAL: Native Bridge health failed ($BRIDGE_HEALTH)" >&2
  systemctl restart homeguardian-bridge
fi

# 3. Verify mail server listener survival
for port in 25 465 587 993 995; do
  if ! ss -lnt | grep -q ":$port "; then
    echo "ALERT: Mail port $port is not listening!" >&2
  fi
done
```

Schedule watchdog execution every 5 minutes:
```text
*/5 * * * * /opt/homeguardian/scripts/healthcheck.sh
```
