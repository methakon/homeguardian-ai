# H-04 - Oracle VPS repurposing: HomeGuardian WireGuard relay + x402 retirement plan

**STATUS: PLAN ONLY, NOTHING EXECUTED ON THE VPS.**
No connection has been made to `161.33.146.238`; nothing has been started, stopped, installed,
reconfigured, rebooted or deleted; no security list, NSG, host-firewall rule, DNS record or
Cloudflare route has been touched. The inventory in section P is recorded as **owner-supplied** -
this plan does not treat it as independently verified, and section J's pre-flight is how it becomes
verified before any decision is taken.

Target instance: `x402-endpoint`, `161.33.146.238`, region `ap-tokyo-1`,
shape `VM.Standard.E2.1.Micro`, Ubuntu 24.04, ~954 MiB RAM + 1 GiB swap.
Intent: **repurpose this instance** (no new instance is created).

---

## P. Owner-supplied inventory (as given; to be verified read-only in the pre-flight)

| item | value |
|---|---|
| PM2 Node application (x402) | ~52.9 MB |
| PM2 daemon | ~50 MB |
| Cloudflare tunnel | routes `x402.berhampore.in` -> `127.0.0.1:8402` |
| nginx (port 80) | default site only |
| mail services | live (OpenSMTPD + Dovecot) |
| RAM available in the snapshot | ~443 MiB |
| disk | ~4.5 GB of 45 GB used |

## PROTECTED (never touched by this migration)

OpenSMTPD on 25, 465, 587 - Dovecot on 993, 995 - mailboxes, mail queues, accounts, configuration,
certificates and mail-related DNS - SSH and fail2ban. HomeGuardian's changes touch **no mail file,
no mail port, no mail DNS, no mail certificate, no mail account**. The only shared resources are RAM/
CPU/disk, the DNS zone (adding one record) and the firewall (adding one UDP rule plus isolation rules).

---

## A. Mail-server protection and before/after health checks

### A.1 What is protected (enumerate, then verify - do not assume paths)

Service, config, accounts and data for: `opensmtpd`, `dovecot`, `fail2ban`, `ssh`; mailbox storage
(`/var/mail/*` or a virtual-mailbox tree), the mail spool/queue, `/etc/smtpd/`, `/etc/dovecot/`,
mail TLS material, and the mail DNS records (MX/SPF/DKIM/DMARC/autoconfig).

### A.2 Before/after check set (read-only, no credentials, run by the owner or with the owner's access)

```bash
# 1. listeners: exactly these and no fewer
ss -lntp | grep -E ':(25|465|587|993|995)\b'
# 2. services
systemctl is-active opensmtpd dovecot fail2ban ssh
# 3. TLS on submission (587 STARTTLS) and IMAPS (993) - handshake + chain, no login
openssl s_client -connect 127.0.0.1:587 -starttls smtp -brief </dev/null 2>&1 | head -12
openssl s_client -connect 127.0.0.1:993 -brief        </dev/null 2>&1 | head -12
# 4. certificate identity + expiry (so a later change cannot silently swap it)
echo | openssl s_client -connect 127.0.0.1:993 2>/dev/null | openssl x509 -noout -subject -dates -fingerprint -sha256
# 5. queue and mailbox state
sudo smtpctl show queue                 # OpenSMTPD; expect an empty or unchanged queue
sudo doveadm mailbox list -u <a-real-account> | head    # mailbox survives
sudo doveadm stats | head -5
# 6. fail2ban
sudo fail2ban-client status; sudo fail2ban-client status sshd
# 7. mail DNS, from OUTSIDE the VPS (the hub), so it is the public view that is checked
dig +short MX berhampore.in @1.1.1.1; dig +short TXT berhampore.in @1.1.1.1; dig +short TXT _dmarc.berhampore.in @1.1.1.1
```
Additionally, one end-to-end mail probe the owner runs (an external account to a mailbox on this
box, and a submission from a mailbox) - **before** the window and **after** each phase. Record
timestamps and delivery latency.

**Gate:** the after-set must be identical to the before-set (same sockets, same certificate
fingerprint, empty queue, same fail2ban status, mail delivered within the normal latency). Any
difference stops the migration and triggers the rollback of the last phase.

---

## B. x402 assets to retire - dependencies and the reversible procedure

### B.1 Assets (verify each read-only BEFORE touching anything)

| # | asset | how to identify it | dependency to check first |
|---|---|---|---|
| 1 | x402 Node app, port 8402 | `pm2 list`, `pm2 describe x402` (gives script + cwd + env) | must be the only consumer of 8402: `ss -lntp \| grep 8402` |
| 2 | PM2 daemon (~50 MB) | `pm2 jlist` (is anything else managed?) | **only retire if x402 is the only app** |
| 3 | app directory + its config/.env | from `pm2 describe x402` cwd | a second service may read the same .env |
| 4 | PM2 resurrection unit | `systemctl list-unit-files \| grep -i pm2`, `systemctl is-enabled pm2-*.service` | - |
| 5 | Cloudflare tunnel route | `/etc/cloudflared/config.yml` or `cloudflared tunnel list` + `info <name>` | see **I** - the tunnel may serve other hostnames |
| 6 | DNS `x402.berhampore.in` | zone export (read-only) - record the EXACT record type/value/ttl/proxy state | - |
| 7 | nginx | `nginx -T \| grep -n 8402`; `ls /etc/nginx/sites-enabled` | inventory says default site only - CONFIRM and then leave nginx alone |
| 8 | cron/timers/logrotate | `crontab -l`, `ls /etc/cron.*/`, `systemctl list-timers` | - |

### B.2 Reversible backup (before any stop; no secrets are committed to git)

```bash
sudo install -d -m 700 /root/retire-x402-$(date +%F)
sudo pm2 save                                  # make the dump current FIRST
sudo cp -a ~/.pm2/dump.pm2 /root/retire-x402-*/          # what resurrect needs
sudo tar czf /root/retire-x402-*/x402-app.tgz -C <app-cwd> .
sudo cp -a /etc/cloudflared /root/retire-x402-*/
sudo systemctl cat pm2-* > /root/retire-x402-*/pm2-units.txt 2>/dev/null
sudo crontab -l > /root/retire-x402-*/crontab.txt 2>/dev/null
{ ss -lntp; sudo iptables -S; dig +short x402.berhampore.in; } > /root/retire-x402-*/before-state.txt
```
Then copy that directory to the Ubuntu hub (the owner's own transfer) - the VPS is the thing being
changed, so the backup must not live only on it. Nothing here is committed to the repository.

### B.3 Retirement = stop + disable, not delete

```bash
sudo pm2 stop x402 && sudo pm2 delete x402      # (only after the backup exists)
sudo systemctl disable --now pm2-<user>.service  # only if nothing else needs PM2
# remove ONLY the x402 ingress line from the tunnel config, then: sudo systemctl reload cloudflared
# DNS: remove/unproxy x402.berhampore.in ONLY with explicit approval, AFTER the pilot passes
```
**Files are kept.** `pm2 delete` removes the app from the daemon's list, so the backup (dump +
directory) is what makes this reversible.

### B.4 Rollback for x402

```bash
# restore the app
sudo tar xzf /root/retire-x402-*/x402-app.tgz -C <original-cwd>
sudo systemctl enable --now pm2-<user>.service     # or: pm2 resurrect (uses dump.pm2)
sudo pm2 list                                      # expect x402 online on 8402
# restore the route
sudo cp -a /root/retire-x402-*/cloudflared/. /etc/cloudflared/ && sudo systemctl reload cloudflared
# restore DNS only if it was changed (re-create the exact record from before-state.txt)
```
Ordering rule: **x402 is retired only after the HomeGuardian VPN path passes section J's gates**, so
the instance is never left having lost one thing and not yet gained the other.

---

## C. `homeguardian.berhampore.in` - DNS-only endpoint

* Record: **A** `homeguardian.berhampore.in` -> `161.33.146.238`, **DNS-only (grey cloud)**, TTL 300.
* **Why DNS-only:** WireGuard is UDP. Cloudflare's proxy forwards HTTP(S) only, and a proxied record
  or a Cloudflare Tunnel cannot carry UDP - a proxied record would silently break the VPN. Spectrum
  (which can carry UDP) is a paid enterprise product and is **not** part of this plan.
* Its role: the **WireGuard endpoint** for nodes and for the hub
  (`Endpoint = homeguardian.berhampore.in:51820`), i.e. a name that survives an IP change.
* It is **not** a HomeGuardian TLS endpoint. The TLS endpoint the phone dials remains the hub's
  **VPN address `10.66.0.2:8443`**, reachable only through the tunnel, so the certificate SAN never
  depends on the VPS's address.
* Optional (only if the owner wants a directly-verifiable name): add
  `DNS:homeguardian.berhampore.in` to the hub's server certificate SAN via
  `tools/provision_ca.sh server --server-names "..."` (CA and device certificates untouched).
* Change requires approval; no DNS change happens before the window.

---

## D. WireGuard routing between the VPS, the Android nodes and the Ubuntu hub

| role | VPN address | public endpoint | AllowedIPs |
|---|---|---|---|
| VPS (relay) | `10.66.0.1/24` | `homeguardian.berhampore.in:51820/udp` | per peer (below) |
| Ubuntu hub | `10.66.0.2/24` | - (dials out; home NAT) | `10.66.0.0/24` **only** |
| Android nodes | `10.66.0.10`, `.11`, `.12` ... (`/32` each) | - (dials out) | `10.66.0.2/32` **only** |

* Hub: `PersistentKeepalive = 25` so the home NAT mapping survives; **never** `0.0.0.0/0` (that would
  route the hub's whole internet through Tokyo).
* Node: AllowedIPs `10.66.0.2/32` means a node can reach the hub and nothing else - not the relay's
  services, not another node.
* VPS: `net.ipv4.ip_forward=1`; peer entries exactly `10.66.0.2/32` (hub) and `10.66.0.x/32` (nodes).
* **Forward-chain isolation (mandatory):** allow `wg0 -> 10.66.0.2:8443`; **drop** `wg0 -> mail
  ports (25/465/587/993/995)`, `wg0 -> 22`, and `wg0 -> any other wg peer`. A compromised node must
  not become a path to the mail server or to SSH.
* MTU 1420 (lower for a PPPoE uplink); mail traffic never traverses `wg0`.
* No default-route change on the VPS and no change to the hub's default route.

---

## E. End-to-end mTLS through the VPN, with strict SAN verification

* **The relay never terminates TLS.** The phone's TLS session is with the hub; the VPS forwards
  ciphertext inside WireGuard. It holds no CA key, no server key, no device key, no registry, no
  replay state, and it runs no second HomeGuardian instance.
* The phone's endpoint becomes `10.66.0.2:8443`; the device client verifies the **dialled address**
  (`X509_VERIFY_PARAM_set1_ip`) with **no way to disable it** (D-03.1). Therefore the hub's server
  certificate must carry `IP:10.66.0.2`:
  `bash tools/provision_ca.sh server --server-names "DNS:localhost,IP:127.0.0.1,IP:192.168.1.6,IP:10.66.0.2"`
* Unchanged: the registry binding (certificate CN = device id **and** SHA-256 fingerprint),
  `identity_mismatch` refusal, delivery-time authorization refresh, and the two-phase durable replay
  watermark (on the hub, so it survives a relay restart).
* Acceptance additions: extend `scripts/verify_tls_identity.sh` with the VPN address (one case that
  must match, one that must not). **Forbidden:** `--insecure`, trust-all, skipping verification, or
  accepting a CN match in place of the dialled address.

---

## F. Android 8.1 / API 27 compatibility and power

* **WireGuard's official Android app supports API 21+**, so API 27 is fine. Installing it on the
  owner's phone is an **owner decision** (it shows a persistent VPN key icon and requires the system
  VPN consent dialog) - this plan does not assume it.
* **Always-on VPN** exists from API 24 (`Always-on VPN` + `Block connections without VPN`): good for
  an unattended node, with the caveat that it can block other traffic on that phone.
* Rejected alternatives: a rooted device (`wg-quick`), or a bespoke userspace tunnel - the first is
  forbidden by the project's constraints, the second is out of scope and unreviewed.
* **Power/RAM reality of this node**: the phone reports ~962 MB RAM on an MT6580. Recommend a battery
  optimisation exemption for **both** the VPN app and the HomeGuardian app, or Android may kill them.
* **Known H-03 limitation that constrains the pilot:** capture is activity-scoped today (no foreground
  service), so an unattended node with the screen off is not yet supported. The pilot must therefore
  either keep the app in the foreground or accept capture only while it is - and the foreground-service
  work remains the open H-03 follow-up.
* Data: use the mobile profile (observation interval 5000 ms -> estimated ~20-35 MB/day, to be
  measured) rather than the 1 s default (~90-170 MB/day estimated).
* Measure: battery drain (%/hour) with the app + tunnel active on mobile data, over the bounded pilot.

---

## G. Resource budget - explicitly NOT assuming the two RSS values are additive

* Snapshot: ~954 MiB RAM + 1 GiB swap, ~443 MiB available.
* **The naive sum is wrong.** The PM2 app (~52.9 MB) and daemon (~50 MB) figures are RSS: they include
  pages shared with other processes (libc, Node's shared libraries), and Node's allocator does not
  return every freed arena to the OS. Part of what is released becomes page cache - reclaimable, but
  not "free". **Budget 50-80 MB, not 103 MB**, and confirm it by measurement:
  `free -m`, `grep MemAvailable /proc/meminfo`, and `sudo smem -tk` if available, before/after.
* New cost on the VPS: `wireguard-tools` (a few MB of userland, used only at setup; the data path is
  in-kernel), a few KB of wg0 state per peer, conntrack/slab entries per node (negligible at 2-5).
  `cloudflared`, nginx, OpenSMTPD, Dovecot are unchanged in size.
* Swap is the real risk on a 954 MiB box: after the change `vmstat 1 10` must show ~0 `si/so`, and
  `dmesg | grep -i oom` must stay empty during the pilot.
* Disk: ~4.5 GB of 45 GB used; the new material is config + keys (KB) - no risk.
* CPU: relaying 1-5 observations/s of small frames is negligible; **mail stays the dominant CPU/IO
  consumer** and is measured before/after with `vmstat`/`sar`.
* **Pilot assumption: 2 nodes** (two phones, one on Wi-Fi and one on mobile data) for the first
  pilot, then scale to 5. The pilot exists to prove stability, mail isolation, reconnect behaviour and
  phone battery - not to discover memory pressure, which per-node is negligible.

---

## H. Oracle security-list/NSG and host-firewall proposals (APPROVAL REQUIRED)

| # | layer | propose | rationale |
|---|---|---|---|
| 1 | VCN security list / NSG **and** instance firewall | **add** ingress **UDP 51820** from `0.0.0.0/0` | WireGuard must accept roaming clients from arbitrary networks; the control is the per-peer key, not the source address |
| 2 | VCN + instance | **tighten** ingress **TCP 22** to the owner's addresses | currently likely open; a tightening, proposed not applied |
| 3 | instance firewall `filter` (FORWARD) | **add** allow `wg0 -> 10.66.0.2 tcp/8443`; **drop** `wg0 -> 22,25,465,587,993,995` and `wg0 -> wg0` (except to .2) | peer isolation (section D) |
| 4 | anything else | **unchanged** | mail ports, 80/443, DNS, NTP |

* **No 8443 inbound, ever.** The hub's ingress stays off the public internet; only the VPN is exposed.
* Ubuntu 24.04 note: the VCN rules are enforced **outside** the instance, and Oracle's Ubuntu images
  also ship an instance-level ruleset (often with a default REJECT). **Both layers must be changed
  together and reverted together** - an ingress rule alone will not open a port.
* **Revert is targeted**: delete exactly the added rules (listed verbatim in the change request).
  Never `iptables -F`, never flush a chain, never touch an existing mail rule.

---

## I. Cloudflare tunnel assessment (before stopping anything)

1. **Enumerate the tunnel's routes, read-only:** `/etc/cloudflared/config.yml` (self-managed) or
   `cloudflared tunnel list` + `cloudflared tunnel info <name>` (dashboard-managed), and the DNS
   records that CNAME to `<tunnel-id>.cfargotunnel.com`.
2. **If `x402.berhampore.in` is the tunnel's ONLY route**, the tunnel and the cloudflared service can
   be retired together with x402 - after backing up `/etc/cloudflared`.
3. **If anything else uses it**, remove **only** the x402 ingress line and reload; leave the tunnel
   alone. Do not assume it is single-purpose.
4. **Never route WireGuard through Cloudflare.** The proxy is HTTP(S)-only and cannot forward
   arbitrary UDP; a Tunnel or a proxied record would appear to work and silently drop the VPN.
   `homeguardian.berhampore.in` must be **DNS-only**. (Cloudflare Spectrum could carry UDP but is a
   paid enterprise product and is not in this plan.)
5. nginx: the inventory says port 80 serves only the default site - **confirm** (`nginx -T | grep -n
   8402`, `ls /etc/nginx/sites-enabled`) and then leave nginx untouched.

---

## J. Minimal-risk sequence, acceptance gates and rollback

### J.0 Pre-flight (read-only, no changes)

Capture into one directory: section A's before-set, section B.1's asset table, `free -m` /
`/proc/meminfo` / `vmstat 1 10` / `df -h`, `ss -lntup`, `iptables -S`, the tunnel config, the DNS
records, `pm2 list`. **This is the read-only verification of the owner-supplied inventory** - every
number in section P is checked here, and the plan is adjusted if anything differs.

### J.1 Phases (each one: exact commands + revert command, presented for GO/NO-GO)

| phase | action | acceptance gate | revert |
|---|---|---|---|
| 1 | DNS A record `homeguardian.berhampore.in` (DNS-only) | `dig +short` returns 161.33.146.238; nothing else changed | delete the record |
| 2 | firewall: UDP 51820 (both layers) + forward isolation rules | `ss -lntp` diff vs pre-flight is **empty** (mail unaffected) | remove exactly those rules |
| 3 | WireGuard on the VPS (wg0; hub peer) | `wg show` handshake with the hub, transfer counters moving | `wg-quick down wg0`, disable the unit |
| 4 | WireGuard on the hub (client, keepalive, AllowedIPs 10.66.0.0/24) | hub pings 10.66.0.1; the VPS can route to 10.66.0.2 | `wg-quick down wg0` on the hub |
| 5 | re-issue the hub server cert with `IP:10.66.0.2` (+ optional DNS) | `verify_tls_identity.sh` incl. the new VPN-address case; the LAN path still works | re-issue without the VPN SAN (CA/device certs untouched) |
| 6 | node #1 over WireGuard on **Wi-Fi** | authenticated observations accepted, watermark advances | disconnect the tunnel |
| 7 | node #1 on **mobile data, Wi-Fi OFF** | observations still accepted (the real goal) | as above |
| 8 | mail verification (section A) | after-set identical to before-set | n/a - mail was never changed; a difference stops everything |
| 9 | bounded pilot, 24-72 h, then node #2 (2-5 node assumption) | no OOM, ~0 swap, mail latency normal, reconnect (Wi-Fi<->mobile) works, replay refused after a relay restart | disconnect nodes |
| 10 | **retire x402** (section B) | HomeGuardian path still works, mail still works, and the x402 backup restores it in a drill | B.4 |

### J.2 Rollback (per phase, precise)

* **10:** restore the app dir + `pm2 resurrect` (+ enable the unit), restore `/etc/cloudflared` +
  reload, re-create the DNS record from `before-state.txt`.
* **6-9:** disconnect/disable the tunnel on the phone; the hub is unaffected.
* **5:** `provision_ca.sh server --server-names "<original list>"` - CA and all device certificates
  are untouched, so no phone needs re-provisioning.
* **3-4:** `wg-quick down wg0` (+ disable the unit) on whichever side was configured.
* **2:** delete exactly the added rules; verify with `iptables -S` diff.
* **1:** delete the A record.
* **Mail:** never in scope of a rollback, because it is never changed. Any mail anomaly is treated as
  a stop-everything event, not as a rollback step.

---

## K. Migration window and approval requirements

* **No change happens outside an explicitly approved window.** The owner names the date/time; the
  sequence is proposed minutes-to-hours ahead, step by step.
* Suggested shape: a **low-mail-traffic window** of >= 2 h, with checkpoints after each phase, and a
  stated abort criterion (any mail regression, any unexplained resource move, any acceptance failure).
* **Per-step approval:** each phase is presented with its exact commands *and* its revert command;
  the owner may stop after any phase and roll back that phase alone.
* **Hard requirements before phase 10 (x402 retirement):** every gate in the owner's cutover
  acceptance list must pass - VPN reachable from the hub; a compatible Android node connecting from a
  genuinely different network; end-to-end mTLS with no trust-all workaround; real observations
  accepted with replay protection effective; mail delivery/submission/IMAP functional; SSH
  accessible; firewall denying unintended peer access to mail/management; memory/swap/CPU stable
  through the bounded pilot; and rollback instructions exercised as far as is safe.
* **H-03 physical acquisition must be working first.** That is the owner's own precondition, and it is
  still PENDING - so the H-04 cutover is gated on H-03, not merely on this plan.
* Restrictions that continue to apply until the owner approves the cutover: do not stop/delete/replace
  x402; do not remove PM2; do not stop or change OpenSMTPD/Dovecot; do not alter mail DNS,
  certificates or accounts; do not modify the Oracle firewall or security lists; do not change DNS or
  remove the Cloudflare tunnel; do not reboot the VPS; do not expose 8443 publicly; do not commit VPN
  private keys, SSH credentials or cloud secrets.
