#!/usr/bin/env bash
# H-04 READ-ONLY pre-flight bundle for the Oracle VPS (x402-endpoint, 161.33.146.238).
#
# HOW TO USE: run this ON THE VPS as your normal login user and return the whole output.
#   bash oracle_preflight_readonly.sh | tee /tmp/oracle-preflight-$(date +%F).txt
#
# GUARANTEES, by construction:
#   * READ-ONLY. Nothing here writes, starts, stops, installs, reloads or reconfigures anything. The
#     only writes are the output file you redirect yourself.
#   * NO PRIVILEGE ESCALATION, AT ALL. This script contains no privilege-escalation command and
#     never asks for a password. Commands that need
#     privileges are ATTEMPTED and, if refused, print "needs privileges - SKIPPED" instead of failing
#     or prompting. You may re-run any single SKIPPED line with whatever access you already have; the
#     bundle does not ask you to grant any.
#   * NO SECRETS. It prints no key material, no tokens, no password hashes, no mail credentials and no
#     environment dumps. Output is piped through a redactor that drops lines containing secret-shaped
#     text, and the few files that could carry a secret (tunnel config, PM2 environment) are filtered
#     to identifiers only. If you spot anything sensitive in the output, delete that line before
#     sending it - the bundle is designed so that should not be necessary.
#   * BOUNDED. Every command is wrapped in `timeout`, so a wedged service cannot hang the run.
#
# It changes no state, so it is safe to run during business hours.

set -uo pipefail
export LC_ALL=C
TMO=10

# Redactor: drop the VALUE of anything secret-shaped while keeping the key visible, and drop any line
# that looks like a private key block. Applied to the whole report at the end.
redact() {
    sed -E \
        -e 's/((token|secret|password|passwd|key|apikey|api_key|credential|auth)[A-Za-z0-9_-]*[[:space:]]*[:=][[:space:]]*)[^[:space:]]+/\1<redacted>/Ig' \
        -e '/-----BEGIN [A-Z ]*PRIVATE KEY-----/,/-----END [A-Z ]*PRIVATE KEY-----/d' \
        -e 's#(https?://)[^/@[:space:]]+@#\1<redacted>@#g'
}

section() { printf '\n===== %s =====\n' "$1"; }
run() { printf '$ %s\n' "$*"; timeout "$TMO" sh -c "$*" 2>&1 || printf '  [exit %s]\n' "$?"; }
try() {  # like run(), but a privilege refusal is reported as SKIPPED, not as an error
    printf '$ %s\n' "$*"
    out="$(timeout "$TMO" sh -c "$*" 2>&1)"; rc=$?
    if printf '%s' "$out" | grep -qiE 'permission denied|operation not permitted|must be root|are you root'; then
        printf '  needs privileges - SKIPPED (this bundle does not request any)\n'
    else
        printf '%s\n' "$out"
        [[ $rc -ne 0 ]] && printf '  [exit %s]\n' "$rc"
    fi
}

{
section "0. identity and scope"
run 'date -Is'
run 'hostname; uname -srm'
run 'cat /etc/os-release | head -4'
run 'cat /etc/oracle-release 2>/dev/null | head -2'
run 'curl -s -m 3 -H "Authorization: Bearer Oracle" http://169.254.169.254/opc/v2/instance/ | tr "," "\n" | grep -iE "\"(displayName|shape|region|availabilityDomain)\"" 2>/dev/null || echo "  (metadata endpoint not reachable from here - use the console)"'

section "1. resources (the budget question)"
run 'free -m'
run 'grep -E "^(MemTotal|MemAvailable|SwapTotal|SwapFree|Committed_AS)" /proc/meminfo'
run 'vmstat 1 3 | tail -3'
run 'df -h / /var 2>/dev/null'
run 'nproc; uptime'

section "2. listeners (what is actually bound, by protocol)"
run 'ss -lntp'
run 'ss -lnup'
run 'ss -lntp | grep -cE ":(25|465|587|993|995)\b"'

section "3. processes and memory by RSS (the x402/PM2 footprint)"
run 'ps -eo pid,ppid,rss,pmem,etime,comm,args --sort=-rss | head -15'
run 'ps -eo rss,comm | awk "NR>1{rss[\$2]+=\$1} END{for (c in rss) printf \"%-24s %8.1f MB\n\", c, rss[c]/1024}" | sort -k2 -nr | head -12'
run 'command -v pm2 >/dev/null && pm2 list || echo "  pm2 not on PATH for this user"'
run 'command -v pm2 >/dev/null && pm2 jlist 2>/dev/null | tr "," "\n" | grep -iE "\"(name|pm_id|status|pid|restart_time)\"" | head -20 || echo "  (no pm2 for this user)"'
run 'ps -eo pid,rss,etime,args | grep -E "[Pp][Mm]2" | grep -v grep || echo "  no pm2 daemon process"'

section "4. services and their state"
for svc in opensmtpd dovecot fail2ban ssh sshd nginx cloudflared; do
    run "systemctl is-active $svc 2>/dev/null || echo 'not active/unknown'"
done
run 'systemctl list-units --type=service --state=running --no-pager --no-legend | awk "{print \$1}" | head -30'
run 'systemctl list-unit-files --state=enabled --no-pager --no-legend 2>/dev/null | grep -iE "pm2|cloudflared|smtp|dovecot|nginx|wireguard|wg" || echo "  none matching"'
run 'crontab -l 2>/dev/null | head -20 || echo "  no user crontab"'

section "5. mail: availability, identity, queue (no credentials printed)"
run 'ss -lntp | grep -E ":(25|465|587|993|995)\b"'
for port in 25 465 587 993 995; do
    run "echo | timeout 6 openssl s_client -connect 127.0.0.1:$port $([ "$port" = 587 ] && echo -starttls smtp) -brief 2>&1 | grep -iE 'protocol|ciphersuite|verify|subject|issuer' | head -6"
done
run "echo | timeout 6 openssl s_client -connect 127.0.0.1:993 2>/dev/null | openssl x509 -noout -subject -issuer -dates -fingerprint -sha256 2>/dev/null || echo '  (no certificate read from 993)'"
try 'smtpctl show queue 2>/dev/null | head -5'
try 'smtpctl show stats 2>/dev/null | head -10'
run 'ls -ld /var/mail /var/vmail /var/spool/smtpd 2>/dev/null || echo "  (mail storage paths differ - listed for the owner to annotate)"'
run 'command -v doveadm >/dev/null && echo "doveadm present (mailbox listing intentionally NOT run: it needs an account name)" || echo "no doveadm"'
run 'fail2ban-client status 2>/dev/null | head -5 || echo "  fail2ban-client not readable/available for this user"'

section "6. nginx and the tunnel (identifiers only, no tokens)"
run 'nginx -T 2>/dev/null | grep -E "server_name|listen|proxy_pass" | head -20 || echo "  (nginx -T needs privileges or nginx is absent)"'
run 'ls -1 /etc/nginx/sites-enabled/ 2>/dev/null || echo "  none readable"'
run 'ls -l /etc/cloudflared/ 2>/dev/null || echo "  no /etc/cloudflared for this user"'
run 'systemctl cat cloudflared 2>/dev/null | grep -E "^ExecStart|^User" | head -4 || echo "  (unit not readable)"'
# hostnames only: drop anything that looks like a token, and never dump the whole file
run 'grep -hoE "(hostname|service)[[:space:]]*:[[:space:]]*[^[:space:]]+" /etc/cloudflared/*.yml /etc/cloudflared/*.yaml 2>/dev/null | head -20 || echo "  (tunnel config not readable - dashboard-managed tunnels expose routes only in the Cloudflare console)"'

section "7. firewall, as configured (no changes made)"
try 'iptables -S'
try 'iptables -t nat -S'
try 'nft list ruleset 2>/dev/null | head -40'
run 'sysctl net.ipv4.ip_forward 2>/dev/null'
run 'ls -l /etc/wireguard/ 2>/dev/null || echo "  no /etc/wireguard (WireGuard not configured - expected)"'
run 'command -v wg >/dev/null && wg --version || echo "  wireguard-tools not installed (expected)"'
run 'lsmod 2>/dev/null | grep -ci wireguard; echo "  (0 = kernel module not loaded)"'

section "8. DNS, as seen from this host and the public resolver"
run 'dig +short x402.berhampore.in @1.1.1.1'
run 'dig +short homeguardian.berhampore.in @1.1.1.1'
run 'dig +short MX berhampore.in @1.1.1.1'
run 'dig +short TXT berhampore.in @1.1.1.1 | head -3'
run 'dig +short TXT _dmarc.berhampore.in @1.1.1.1'
run 'dig +short x402.berhampore.in; dig +short x402.berhampore.in @1.1.1.1 | grep -c cfargotunnel'

section "9. repository/working state (for the app being retired)"
run 'ls -ld /home/* 2>/dev/null | head -6'
run 'ls -1 "$HOME" 2>/dev/null | head -15 || echo "  (own home not listable - unusual)"'
run 'ls -1 /opt /srv 2>/dev/null | head -20'
run 'journalctl -n 5 --no-pager 2>/dev/null | tail -5 || echo "  (journal not readable for this user)"'

section "10. end of bundle"
run 'echo "report generated $(date -Is) on $(hostname)"'
} 2>&1 | redact
