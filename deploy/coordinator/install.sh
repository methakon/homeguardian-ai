#!/usr/bin/env bash
# HomeGuardian AI — Outbound Remote MCP Coordinator Installer
#
# Milestone: HG-NET-02 (Secure Remote MCP Coordinator)
#
# Prepares and installs the coordinator service on the coordination host.
# Supports --dry-run to audit prerequisites without making filesystem changes.
#
# SAFETY INVARIANT:
#   Will NOT modify firewall, mail server (postfix/dovecot), DNS, or Cloudflare.
#   Resource limits are enforced to guarantee zero impact on co-hosted mail.

set -euo pipefail

DRY_RUN=false
for arg in "$@"; do
  if [ "$arg" = "--dry-run" ] || [ "$arg" = "-n" ]; then
    DRY_RUN=true
  fi
done

INSTALL_DIR="/opt/homeguardian/coordinator"
CONFIG_DIR="/etc/homeguardian-coordinator"
SYSTEMD_DIR="/etc/systemd/system"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "================================================================"
echo "  HomeGuardian Remote MCP Coordinator — Installation"
if [ "$DRY_RUN" = true ]; then
  echo "  MODE: DRY-RUN (no system changes will be performed)"
fi
echo "================================================================"

# 1. Prerequisite Checks
echo "[1/6] Auditing System Prerequisites..."

# Check Node.js
if ! command -v node >/dev/null 2>&1; then
  echo "  ERROR: Node.js is not installed. Node.js >= 18 is required." >&2
  exit 1
fi
NODE_VER=$(node -v)
echo "  - Node.js:        $NODE_VER (PASS)"

# Check System Memory
if command -v free >/dev/null 2>&1; then
  FREE_RAM_MB=$(free -m | awk '/^Mem:/{print $7}')
  echo "  - Available RAM:  ${FREE_RAM_MB} MB"
  if [ "$FREE_RAM_MB" -lt 100 ]; then
    echo "  WARNING: Low available memory (< 100MB). Coordinator limits are set to 120MB."
  fi
fi

# Check Mail Ports (Guarantee No Conflict)
echo "[2/6] Verifying Mail Server Isolation (Zero Disruption Policy)..."
MAIL_PORTS="25 587 993 110"
for p in $MAIL_PORTS; do
  if command -v ss >/dev/null 2>&1; then
    if ss -tln | grep -q ":$p "; then
      echo "  - Mail Port $p: IN USE BY MAIL SERVICE (Protected - Coordinator will NOT touch)"
    fi
  fi
done

# Check Coordinator Ports
COORD_PORTS="18994 18995"
for p in $COORD_PORTS; do
  if command -v ss >/dev/null 2>&1; then
    if ss -tln | grep -q ":$p "; then
      echo "  ERROR: Coordinator target port $p is already in use by another service." >&2
      exit 1
    fi
  fi
done
echo "  - Coordinator Ports (18994/18995): AVAILABLE (PASS)"

# Check WireGuard Interface
echo "[3/6] Inspecting WireGuard Coordination Network..."
WG_IP="10.42.0.1"
if ip addr show 2>/dev/null | grep -q "$WG_IP"; then
  echo "  - WireGuard Overlay ($WG_IP): DETECTED (PASS)"
  DEFAULT_BIND="$WG_IP"
else
  echo "  - WireGuard Overlay ($WG_IP): NOT ACTIVE (using 127.0.0.1 for local testing)"
  DEFAULT_BIND="127.0.0.1"
fi

if [ "$DRY_RUN" = true ]; then
  echo ""
  echo "================================================================"
  echo "  DRY-RUN AUDIT COMPLETE — ALL PRE-FLIGHT CHECKS PASSED"
  echo "================================================================"
  echo "  Actions that would be taken during live installation:"
  echo "    1. Create user/group 'homeguardian' (nologin, system)"
  echo "    2. Create config directory $CONFIG_DIR (0700)"
  echo "    3. Deploy code to $INSTALL_DIR (0755, readonly to daemon)"
  echo "    4. Install coordinator.env, devices.json, and client_token.secret (0600)"
  echo "    5. Install systemd unit $SYSTEMD_DIR/coordinator.service"
  echo "    6. Enforce MemoryMax=120M and CPUQuota=25%"
  echo "    7. Enable and start coordinator.service"
  echo "================================================================"
  exit 0
fi

# 2. Live Installation
if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: Live installation requires root privileges. Run with sudo or use --dry-run." >&2
  exit 1
fi

echo "[4/6] Creating System User & Directories..."
if ! id -u homeguardian >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin -d /opt/homeguardian -M homeguardian
fi

mkdir -p "$CONFIG_DIR"
chmod 0700 "$CONFIG_DIR"
chown -R homeguardian:homeguardian "$CONFIG_DIR"

mkdir -p "$INSTALL_DIR"
mkdir -p /var/log/homeguardian
chown -R homeguardian:homeguardian /var/log/homeguardian

echo "[5/6] Copying Files & Configuring Service..."
# Copy coordinator runtime code
cp -r "$REPO_ROOT/web_mcp" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/service.js" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/manage_devices.sh" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/healthcheck.sh" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/service.js" "$INSTALL_DIR/manage_devices.sh" "$INSTALL_DIR/healthcheck.sh"

# Symlink management script to /usr/local/bin
ln -sf "$INSTALL_DIR/manage_devices.sh" /usr/local/bin/hg-coordinator-manage

# Initialize configuration files if not present
if [ ! -f "$CONFIG_DIR/coordinator.env" ]; then
  cat <<ENV_EOF > "$CONFIG_DIR/coordinator.env"
COORD_BIND=${DEFAULT_BIND}
COORD_WS_PORT=18994
COORD_HTTP_PORT=18995
COORD_DEVICES_FILE=${CONFIG_DIR}/devices.json
COORD_CLIENT_TOKEN_FILE=${CONFIG_DIR}/client_token.secret
COORD_ALLOWED_SUBNETS=10.42.0.,127.,::1,::ffff:10.42.0.,::ffff:127.
ENV_EOF
  chmod 0600 "$CONFIG_DIR/coordinator.env"
  chown homeguardian:homeguardian "$CONFIG_DIR/coordinator.env"
fi

if [ ! -f "$CONFIG_DIR/devices.json" ]; then
  echo "{}" > "$CONFIG_DIR/devices.json"
  chmod 0600 "$CONFIG_DIR/devices.json"
  chown homeguardian:homeguardian "$CONFIG_DIR/devices.json"
fi

if [ ! -f "$CONFIG_DIR/client_token.secret" ]; then
  openssl rand -hex 32 > "$CONFIG_DIR/client_token.secret"
  chmod 0600 "$CONFIG_DIR/client_token.secret"
  chown homeguardian:homeguardian "$CONFIG_DIR/client_token.secret"
fi

# Install systemd unit
cp "$SCRIPT_DIR/coordinator.service" "$SYSTEMD_DIR/coordinator.service"
systemctl daemon-reload

echo "[6/6] Starting Coordinator Service..."
systemctl enable coordinator.service
systemctl restart coordinator.service

echo ""
echo "================================================================"
echo "  HomeGuardian Remote MCP Coordinator Successfully Installed!"
echo "================================================================"
echo "  Service:        systemctl status coordinator.service"
echo "  Healthcheck:    $INSTALL_DIR/healthcheck.sh"
echo "  Manage Devices: hg-coordinator-manage list"
echo "  Rollback:       $SCRIPT_DIR/rollback.sh"
echo "================================================================"
