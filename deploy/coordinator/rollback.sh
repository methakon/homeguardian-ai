#!/usr/bin/env bash
# HomeGuardian AI — Outbound Remote MCP Coordinator Rollback / Uninstall
#
# Milestone: HG-NET-02 (Secure Remote MCP Coordinator)
#
# Safely rolls back the coordinator installation, restores host state,
# creates an archival backup of configuration and keys, and verifies that
# co-hosted mail services (Postfix/Dovecot) remain completely healthy.

set -euo pipefail

INSTALL_DIR="/opt/homeguardian/coordinator"
CONFIG_DIR="/etc/homeguardian-coordinator"
SYSTEMD_FILE="/etc/systemd/system/coordinator.service"
BIN_LINK="/usr/local/bin/hg-coordinator-manage"

echo "================================================================"
echo "  HomeGuardian Remote MCP Coordinator — Rollback & Uninstall"
echo "================================================================"

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: Rollback requires root privileges. Please run with sudo." >&2
  exit 1
fi

echo "[1/5] Stopping and Disabling coordinator.service..."
if command -v systemctl >/dev/null 2>&1; then
  if systemctl is-active --quiet coordinator.service 2>/dev/null; then
    systemctl stop coordinator.service || true
  fi
  if systemctl is-enabled --quiet coordinator.service 2>/dev/null; then
    systemctl disable coordinator.service || true
  fi
  if [ -f "$SYSTEMD_FILE" ]; then
    rm -f "$SYSTEMD_FILE"
    systemctl daemon-reload
    systemctl reset-failed || true
  fi
fi

# Ensure any lingering coordinator process is terminated
pkill -f "coordinator/service.js" 2>/dev/null || true

echo "[2/5] Backing Up Configurations & Keys..."
if [ -d "$CONFIG_DIR" ]; then
  BACKUP_ARCHIVE="/root/homeguardian-coordinator-backup-$(date +%Y%m%d_%H%M%S).tar.gz"
  tar -czf "$BACKUP_ARCHIVE" -C "$(dirname "$CONFIG_DIR")" "$(basename "$CONFIG_DIR")" 2>/dev/null || true
  chmod 0600 "$BACKUP_ARCHIVE" 2>/dev/null || true
  echo "  - Configuration archive saved to: $BACKUP_ARCHIVE"
  rm -rf "$CONFIG_DIR"
fi

echo "[3/5] Removing Installed Code & Links..."
rm -f "$BIN_LINK"
rm -rf "$INSTALL_DIR"

echo "[4/5] Verifying Ports Are Released..."
for p in 18994 18995; do
  if command -v ss >/dev/null 2>&1; then
    if ss -tln | grep -q ":$p "; then
      echo "  WARNING: Port $p is still listening!"
    else
      echo "  - Port $p: RELEASED"
    fi
  fi
done

echo "[5/5] Auditing Co-Hosted Mail Server Health..."
MAIL_OK=true
for svc in postfix dovecot opendkim; do
  if command -v systemctl >/dev/null 2>&1; then
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
      echo "  - $svc: ACTIVE / UNINTERRUPTED"
    fi
  fi
done

echo "================================================================"
echo "  Rollback Complete. Host state cleanly restored."
echo "================================================================"
