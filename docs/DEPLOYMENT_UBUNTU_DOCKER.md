# HomeGuardian AI — Production Deployment Guide (Ubuntu & Docker)

**Target Platforms:** Ubuntu 22.04 LTS / 24.04 LTS (x86_64, aarch64/ARM64)  
**Container Engine:** Docker Engine 24.0+, Docker Compose v2.20+  
**Release Version:** 1.0.0 (RC2)  
**Security Standard:** Principle of Least Privilege, Loopback-first, Zero Cloud Exposure  

---

## 1. Architecture & Deployment Topology

HomeGuardian AI is organized into three cooperating services:
1. **Core Pipeline & Alexa Bridge (`hg_alexa_bridge`):** C++20 core responsible for sensor event correlation, fusion algorithms, safety rule evaluation, and SQLite persistence.
2. **Native MCP Server (`hg_mcp_server`):** C++20 Streamable HTTP server implementing the Model Context Protocol (spec 2025-11-25) with OS-backed CSPRNG 256-bit session IDs and credential-bound session isolation.
3. **Web Gateway & Signaling Server (`web_mcp/server.js`):** Node.js gateway providing operator authentication (HttpOnly cookies + CSRF tokens), authoritative device management via `hg_device_registry_cli`, conversational MCP translation, and WebRTC signaling for camera viewing.

```
+--------------------------------------------------------------------------------+
|                              Host / Client Browser                             |
|               (Local Operator UI / WebRTC Viewer / MCP Chat)                   |
+--------------------------------------------------------------------------------+
        |                                                 |
        | HTTPS (Port 443 / 8443)                         | WebRTC (Loopback Host ICE)
        v                                                 v
+-------------------------------+                 +------------------------------+
|     Nginx Reverse Proxy       |                 |     WebRTC Media Engine      |
|  (TLS Termination / CSP)      | <-------------> |     (webrtc_streamer.js)     |
+-------------------------------+                 +------------------------------+
        | Loopback HTTP (127.0.0.1:18880)                 |
        v                                                 v
+-------------------------------+                 +------------------------------+
|      Node.js Web Gateway      |                 |     C++ Device Registry      |
|      (web_mcp/server.js)      | <-------------> |   (hg_device_registry_cli)   |
+-------------------------------+                 +------------------------------+
        | Loopback MCP (127.0.0.1:18980)                 |
        v                                                 |
+-------------------------------+                         |
|     Native C++ MCP Server     |                         |
|       (hg_mcp_server)         |                         |
+-------------------------------+                         |
        |                                                 |
        +-----------------------+-------------------------+
                                |
                                v
                +-------------------------------+
                |   Core C++ Pipeline Engine    |
                |       (hg_alexa_bridge)       |
                +-------------------------------+
                                |
                                v
                +-------------------------------+
                |   SQLite Durable Audit Log    |
                | (homeguardian_bridge_audit.db)|
                +-------------------------------+
```

---

## 2. Option A: Native Ubuntu Deployment (systemd)

### 2.1 System Prerequisites & Dependencies

Install required build tools, libraries, and Node.js LTS:

```bash
sudo apt update && sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  libsqlite3-dev \
  libssl-dev \
  curl \
  git

# Install Node.js 20 LTS (if using Node gateway)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs
```

### 2.2 System User & Directory Layout

Create an isolated unprivileged system user and directory structure:

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin homeguardian

sudo mkdir -p /opt/homeguardian/bin \
              /etc/homeguardian/secrets \
              /var/lib/homeguardian/data \
              /var/log/homeguardian

sudo chown -R homeguardian:homeguardian /var/lib/homeguardian /var/log/homeguardian /etc/homeguardian
sudo chmod 750 /var/lib/homeguardian /etc/homeguardian
sudo chmod 700 /etc/homeguardian/secrets
```

### 2.3 Build and Install Binaries

```bash
cd /opt/homeguardian/src # or your source workspace
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHOMEGUARDIAN_BUILD_TESTS=OFF
cmake --build build --target hg_alexa_bridge hg_mcp_server hg_device_registry_cli -j$(nproc)

# Install binaries to /opt/homeguardian/bin
sudo install -m 755 build/hg_alexa_bridge /opt/homeguardian/bin/
sudo install -m 755 build/hg_mcp_server /opt/homeguardian/bin/
sudo install -m 755 build/hg_device_registry_cli /opt/homeguardian/bin/
```

### 2.4 Credential & Configuration Provisioning

Generate secure, OS-randomized credentials:

```bash
# 1. Generate Bridge & MCP operator tokens
OPERATOR_TOKEN=$(openssl rand -hex 32)
cat <<EOF | sudo tee /etc/homeguardian/secrets/mcp_creds.json >/dev/null
{
  "${OPERATOR_TOKEN}": {
    "principal": "operator",
    "allow_status": true,
    "allow_alert_summary": true,
    "expires_at_ms": 0,
    "revoked": false
  }
}
EOF
sudo chmod 600 /etc/homeguardian/secrets/mcp_creds.json

# 2. Store operator token file
printf '%s' "$OPERATOR_TOKEN" | sudo tee /etc/homeguardian/secrets/operator_token.txt >/dev/null
sudo chmod 600 /etc/homeguardian/secrets/operator_token.txt

# 3. Generate Bridge secret
openssl rand -hex 32 | sudo tee /etc/homeguardian/secrets/bridge_secret.txt >/dev/null
sudo chmod 600 /etc/homeguardian/secrets/bridge_secret.txt

# 4. Copy default configuration
sudo cp config/homeguardian.demo.json /etc/homeguardian/config.json
sudo chmod 644 /etc/homeguardian/config.json

# 5. Initialize device registry
sudo -u homeguardian /opt/homeguardian/bin/hg_device_registry_cli \
  --registry /var/lib/homeguardian/data/devices.json init_defaults
sudo chmod 600 /var/lib/homeguardian/data/devices.json
```

### 2.5 Systemd Service Units

#### Service 1: `homeguardian-mcp.service` (`/etc/systemd/system/homeguardian-mcp.service`)
```ini
[Unit]
Description=HomeGuardian Native C++ MCP Service
After=network.target
Documentation=https://github.com/methakon/homeguardian-ai

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/var/lib/homeguardian/data
Environment=HG_MCP_PORT=18980
Environment=HG_MCP_HOST=127.0.0.1
ExecStart=/opt/homeguardian/bin/hg_mcp_server \
  /etc/homeguardian/config.json \
  /etc/homeguardian/secrets/mcp_creds.json
Restart=always
RestartSec=5s
MemoryMax=128M
TasksMax=64
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian/data /var/log/homeguardian
ReadOnlyPaths=/etc/homeguardian
PrivateTmp=true
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

#### Service 2: `homeguardian-bridge.service` (`/etc/systemd/system/homeguardian-bridge.service`)
```ini
[Unit]
Description=HomeGuardian Native HTTP Alexa Bridge & Sensor Pipeline
After=network.target
Documentation=https://github.com/methakon/homeguardian-ai

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/var/lib/homeguardian/data
ExecStart=/opt/homeguardian/bin/hg_alexa_bridge \
  --http /etc/homeguardian/config.json \
  /etc/homeguardian/secrets/mcp_creds.json \
  --port 18777 \
  --token-file /etc/homeguardian/secrets/operator_token.txt \
  --secret-file /etc/homeguardian/secrets/bridge_secret.txt
Restart=always
RestartSec=5s
MemoryMax=128M
TasksMax=64
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian/data /var/log/homeguardian
ReadOnlyPaths=/etc/homeguardian
PrivateTmp=true
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

#### Service 3: `homeguardian-gateway.service` (`/etc/systemd/system/homeguardian-gateway.service`)
```ini
[Unit]
Description=HomeGuardian Web Gateway & WebRTC Signaling
After=homeguardian-mcp.service homeguardian-bridge.service
Documentation=https://github.com/methakon/homeguardian-ai

[Service]
Type=simple
User=homeguardian
Group=homeguardian
WorkingDirectory=/opt/homeguardian/web_mcp
Environment=NODE_ENV=production
Environment=HG_WEB_PORT=18880
Environment=HG_MCP_PORT=18980
Environment=HG_REGISTRY_PATH=/var/lib/homeguardian/data/devices.json
Environment=HG_REGISTRY_CLI=/opt/homeguardian/bin/hg_device_registry_cli
Environment=HG_OPERATOR_USER=admin
EnvironmentFile=/etc/homeguardian/secrets/gateway.env
ExecStart=/usr/bin/node server.js
Restart=always
RestartSec=5s
MemoryMax=256M
TasksMax=128
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/homeguardian/data /var/log/homeguardian
ReadOnlyPaths=/etc/homeguardian /opt/homeguardian
PrivateTmp=true
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

Generate `/etc/homeguardian/secrets/gateway.env`:
```bash
GATEWAY_PASS=$(openssl rand -hex 24)
cat <<EOF | sudo tee /etc/homeguardian/secrets/gateway.env >/dev/null
HG_OPERATOR_PASSWORD=${GATEWAY_PASS}
EOF
sudo chmod 600 /etc/homeguardian/secrets/gateway.env
```

Enable and start services:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now homeguardian-mcp homeguardian-bridge homeguardian-gateway
sudo systemctl status homeguardian-mcp homeguardian-bridge homeguardian-gateway
```

### 2.6 Nginx Reverse Proxy with HTTPS

Install and configure Nginx for TLS termination and WebSockets:

```nginx
# /etc/nginx/sites-available/homeguardian
server {
    listen 80;
    server_name homeguardian.local;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name homeguardian.local;

    ssl_certificate /etc/ssl/certs/homeguardian.crt;
    ssl_certificate_key /etc/ssl/private/homeguardian.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;

    # Security Headers
    add_header X-Frame-Options "DENY" always;
    add_header X-Content-Type-Options "nosniff" always;
    add_header Content-Security-Policy "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; media-src 'self' blob:; connect-src 'self' ws: wss:;" always;
    add_header Strict-Transport-Security "max-age=63072000; includeSubDomains" always;

    # Gateway Proxy
    location / {
        proxy_pass http://127.0.0.1:18880;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_read_timeout 86400s;
        proxy_send_timeout 86400s;
    }
}
```

---

## 3. Option B: Docker Containerized Deployment

### 3.1 Directory Preparation

```bash
mkdir -p deploy/docker/certs \
         deploy/docker/secrets \
         deploy/docker/data

# Initialize credentials
cat <<EOF > deploy/docker/secrets/creds.json
{
  "demo-token": {
    "principal": "demo",
    "allow_status": true,
    "allow_alert_summary": true,
    "expires_at_ms": 0,
    "revoked": false
  }
}
EOF
chmod 600 deploy/docker/secrets/creds.json

# Copy environment template
cp deploy/docker/.env.example deploy/docker/.env
```

### 3.2 Provisioning Mutual TLS Device Boundary (Optional mTLS Ingress)

```bash
# Generate CA and server certificates
bash tools/provision_ca.sh init
bash tools/provision_ca.sh issue-server
```

### 3.3 Running via Docker Compose

```bash
# Build runtime image
docker compose -f deploy/docker/docker-compose.yml build

# Launch services in detached mode
docker compose -f deploy/docker/docker-compose.yml up -d

# Verify container health
docker compose -f deploy/docker/docker-compose.yml ps
```

### 3.4 Container Security Checks

The Docker deployment enforces:
- **Zero Root Execution:** Runs under user `10001:10001`.
- **Read-Only Secret Mounts:** `creds.json` mounted `:ro` via Docker secrets.
- **Persistent Data Volume:** Database lives in named volume `hg_data` mapped to `/data`.
- **Loopback Internal Binding:** Port `8080` remains loopback inside the container; only the mTLS ingress publishes port `8443`.

---

## 4. Maintenance & Operations

### 4.1 SQLite Online Database Backups

HomeGuardian writes alerts to SQLite with WAL (Write-Ahead Logging) enabled. Create live, zero-downtime backups:

```bash
# Perform live online backup
sqlite3 /var/lib/homeguardian/data/homeguardian_bridge_audit.db \
  ".backup '/var/lib/homeguardian/backups/audit-$(date +%F_%H%M%S).db'"

# Prune backups older than 30 days
find /var/lib/homeguardian/backups/ -name "audit-*.db" -mtime +30 -delete
```

### 4.2 Log Rotation

Create `/etc/logrotate.d/homeguardian`:

```text
/var/log/homeguardian/*.log {
    daily
    rotate 14
    compress
    delaycompress
    missingok
    notifempty
    create 0640 homeguardian homeguardian
    postrotate
        systemctl reload homeguardian-bridge >/dev/null 2>&1 || true
    endscript
}
```

### 4.3 Service Health Verification

```bash
# 1. Health endpoint check
curl -sf http://127.0.0.1:18880/api/health | jq .

# 2. Native C++ bridge health
curl -sf http://127.0.0.1:18777/api/health | jq .

# 3. Check memory consumption
systemctl status homeguardian-bridge homeguardian-mcp homeguardian-gateway | grep Memory
```
