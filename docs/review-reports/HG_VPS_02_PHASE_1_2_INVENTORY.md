# HG-VPS-02 — PHASES 1-2: READ-ONLY INVENTORY + REMOVAL MANIFEST

**Task scope:** read-only investigation of the Oracle Cloud instance `x402-endpoint` and an explicit
SAFE / SHARED-PRESERVE / UNKNOWN-STOP manifest for retiring the x402 payment service, **without**
disturbing the live production mail server (OpenSMTPD + Dovecot + DKIM).

**Execution mode:** READ-ONLY. No file created, modified or deleted on the target host; no service
stopped/started/restarted/disabled; no firewall, DNS, NSG, nginx, cloudflared or certificate change;
no package install/remove/upgrade; no reboot. All probes were piped to the host over `ssh … 'bash -s'`
(stdin), so nothing was written on the host or in its home directory. No `/proc/*/environ`, no private
keys, no mail content, and no `.env` / credential file contents were read.
**Three incidental, benign filesystem touches did occur and are disclosed in full in §6.**

**Date of inventory:** 2026-10-17 ~21:25–21:35 IST.

---

## 0. CONNECTIVITY OUTCOME (this was part of the task)

**REACHED — no blocker.** The host was reached over SSH with the pre-existing per-instance key.

| Item | Value |
|---|---|
| Route used | `ssh -i ~/.ssh/oci-vm-id_ed25519 ubuntu@<TARGET_IP>` — TARGET_IP = the instance's public address `161.33.x.x` (mode 0600 key, present) |
| Host key handling | `StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null` (nothing persisted) |
| Auth result | Success, first attempt, no password prompt, no banner stall |
| Also confirmed | TCP :22 reachable; `oci` CLI 3.92.0 works against ap-tokyo-1 |

**Why the earlier assessment could not reach it:** it was a *parsing/identity* failure, not an access
failure. Two concrete causes found and fixed:

1. `oci compute instance list --output json` does **not** return a bare list here — it returns a
   `{"data": [...]}` **wrapper**. Iterating the parsed object directly yields dict keys and fails with
   `string indices must be integers, not 'str'`. Correct parse: `json.load(f)["data"]`.
2. The instance **is** in the tenancy ROOT compartment
   (`ocid1.tenancy.oc1..aaaaaaaawzxb266aqhdsiczpg2llsqjvr6cdrm43a3xgskuqywol64bc7gma`) — the same
   compartment-id used by the project launch script. `oci iam compartment list` returns **zero child
   compartments** for this tenancy (empty output, exit 0), so there is no child compartment to search:
   the root compartment is the correct and only place to look.
   `oci compute instance list --compartment-id <tenancy-root> --all` returns **2 rows** —
   `Dhargent` (out of scope, untouched) and `x402-endpoint`. Filtering on the display name returns
   exactly one row.
3. The project deploy scripts (`launch-x402-vm.sh`, `start-x402-vm.sh`) record the authorizing
   `--display-name x402-endpoint`, shape, image and what `start-x402-vm.sh` does — but **no IP address**
   and no SSH invocation, which is why grepping them for an address found nothing. The address had to
   come from the OCI API (`compute instance list-vnics` → `public-ip`).

No other host was contacted. No key was requested or pasted.

---

## 1. PHASE 1 — INVENTORY (with evidence)

### 1.1 Positive identification of the target ✔

| Field | Observed value | Evidence |
|---|---|---|
| Display name | `x402-endpoint` | `oci compute instance list --compartment-id <tenancy-root> --all` |
| Hostname / `hostnamectl --static` | `x402-endpoint` | SSH probe (matches display name) |
| OCID (API row) | `ocid1.instance.oc1.ap-tokyo-1.anxhiljroegggcicp2sbqrxkp2p77nbcaho3qalprm5fhbguyqcrap2s5p2q` | OCI API |
| OCID (reported by the box itself) | **identical** | OCI IMDS `http://169.254.169.254/opc/v2/instance/id` (read-only, link-local) |
| IMDS display name / shape / region | `x402-endpoint` / `VM.Standard.E2.1.Micro` / `ap-tokyo-1` | IMDS |
| OS | Ubuntu 24.04.5 LTS, kernel `6.17.0-1020-oracle`, x86_64 | `/etc/os-release`, `uname` |
| Shape | `VM.Standard.E2.1.Micro` — 1 OCPU / 1 GB RAM | OCI API + IMDS |
| Availability domain | `jKmJ:AP-TOKYO-1-AD-1` | OCI API |
| Compartment | tenancy **root** | OCI API |
| Lifecycle | RUNNING, created 2026-09-30T15:46:59Z | OCI API |
| Uptime | 17 days (boot 2026-09-30 21:17) | `uptime -s` |
| Address (masked) | `161.33.x.x` (public, ephemeral-class), private `10.0.0.x` on `ens3` | `compute instance list-vnics` |
| Other instance present | `Dhargent` (RUNNING) — **OUT OF SCOPE, not contacted** | OCI API |

**Identification is unambiguous → proceeding was correct.**

### 1.2 The x402 node / PM2 processes (fresh PIDs, as required)

PIDs reported in the brief (node 6853 / pm2 3675) are **still current** — verified fresh:

| PID | comm | exe | cwd | uid | RSS (kB) | PPID | cgroup |
|---|---|---|---|---|---|---|---|
| 3675 | `PM2 v7.0.4: God` | `/usr/bin/node` | `/home/ubuntu/x402` | 1001 (ubuntu) | **47 724** | 1 (systemd) | `user.slice/user-1001.slice/session-5.scope` |
| 6853 | `node /home/ubun…` | `/usr/bin/node` | `/home/ubuntu/x402` | 1001 (ubuntu) | **51 632** | **3675** | `user.slice/user-1001.slice/session-5.scope` |

- PPID chain: `6853 → 3675 → 1 (systemd)`. **The only child of the PM2 God daemon is pid 6853**
  (verified by scanning every `/proc/*/status` for `PPid: 3675`) → killing PM2 can only affect x402.
- Real executable for both: `/usr/bin/node` (single binary; no vendored/embedded node).
- Real application directory for both: **`/home/ubuntu/x402`**.
- The x402 worker's fds point at `/home/ubuntu/.pm2/logs/x402-endpoint-{out,error}-0.log` (2 sockets,
  rest pipes/eventfds) — consistent with a pm2-managed single worker.
- No other node/npm/pm2-titled process exists on the host.

### 1.3 PM2-managed applications (read-only listing: `pm2 jlist`, filtered — no mutating pm2 command)

```
pm2 app count: 1
name=x402-endpoint  pm_id=0  pid=6853  status=online  mode=cluster_mode  instances=1
  script=/home/ubuntu/x402/src/server.js   cwd=/home/ubuntu/x402   node_version=22.23.3
  restart_time=0  unstable_restarts=0  version=0.1.0
  mem=52 535 296 B (≈50 MiB)   cpu=0.2%
```
`pm2` reports exactly **one** application, and it belongs exclusively to x402. `pm2` binary resolves to
`/usr/bin/pm2`; the daemon's dump (`~/.pm2/dump.pm2`, metadata only, no env read) lists the same single
entry (`name=x402-endpoint  cwd=/home/ubuntu/x402`).

### 1.4 Startup mechanism — and how x402 comes back after a reboot

**Finding: x402 does NOT come back after a reboot.** There is no boot wiring at all:

| Candidate mechanism | Result |
|---|---|
| `systemctl list-unit-files \| grep -Ei 'pm2\|node\|npm\|x402'` | **nothing** (only unrelated `kmod-static-nodes`, `systemd-tpm2-setup*`) |
| pm2 systemd unit (`pm2-ubuntu.service`, `pm2-root.service`) | **does not exist** (`find` over `/etc/systemd`, `/usr/lib/systemd`, `~/.config/systemd` → no pm2 unit) |
| any unit `ExecStart=` → node/npm/pm2/npx/node_modules | **none** (grep of all of `/etc/systemd/system` + `/usr/lib/systemd/system`) |
| any unit referencing `/home/ubuntu/x402`, `8402`, `node_modules` | **none** |
| `/etc/crontab`, `/etc/cron.d/*`, root crontab, ubuntu crontab | no `@reboot`, no pm2/x402 entry. Root crontab holds only the mail job `17 * * * * /opt/mail.berhampore.in/cleanup.sh`; ubuntu has **no crontab** |
| `/etc/rc.local`, `/etc/profile.d`, `~/.bashrc`, `~/.profile` | absent / no pm2·x402 reference |
| `.wants` symlinks for pm2/x402 | none |

How it is actually running: the PM2 God daemon (3675) lives in **`session-5.scope`** — an interactive
SSH login session opened 2026-09-30 21:22:03 from `106.219.x.x` (state `closing`, still Active) — i.e.
x402 was started **by hand** through `deploy/start-x402-vm.sh` inside that login, and the session scope
has lingered for 17 days. **User-level** `systemctl --user` shows one enabled unit,
`cloudflared-x402.service` ("Cloudflare Tunnel for x402.berhampore.in"), active/enabled via
`~/.config/systemd/user/default.target.wants/`, running under `user@1001.service` (logind linger **is**
enabled for `ubuntu`: `/var/lib/systemd/linger/ubuntu`, entry dated 2026-09-30 23:38).

**Net reboot behaviour:** after a reboot the Cloudflare tunnel would start (linger + enabled user unit),
but **nothing would listen on 8402** — the tunnel would answer connection-refused. x402 is effectively a
one-shot manual deployment. (This also means: there is *no* systemd unit or cron entry to remove for x402
— only forward-action items are needed.)

### 1.5 Listeners and their owning PIDs (`ss -lntup`, read-only)

| Port | Owner (comm, pid) | Service | Classification |
|---|---|---|---|
| **8402** | **`PM2 v7.0.4: God`, pid 3675** | x402 HTTP endpoint | **x402 — note: bound by the *PM2 daemon*, not by node 6853 (cluster mode: PM2 holds the socket and passes it to the worker). `pm2 kill` is therefore what actually releases 8402.** |
| 25 | `smtpd`, pid 212332 | OpenSMTPD | MAIL — preserve |
| 465 | `smtpd`, pid 212332 | OpenSMTPD (smtps) | MAIL — preserve |
| 587 | `smtpd`, pid 212332 | OpenSMTPD (submission) | MAIL — preserve |
| 993, 995 | `dovecot`, pid 212363 | Dovecot IMAPS/POP3S | MAIL — preserve |
| 127.0.0.1:24 | `dovecot`, pid 212363 | Dovecot LMTP (smtpd `action "dovecot" lmtp 127.0.0.1:24`) | MAIL — preserve |
| 80 | `nginx`, pids 212345/212346/212347 | nginx (default site) | **SHARED — mail TLS renewal depends on it (§1.7)** |
| 22 | `sshd`, pid 213700 | SSH | preserve |
| 127.0.0.1:20241 + 4 UDP | `cloudflared`, pid 4770 | Cloudflare Tunnel (x402) | x402 |
| 53 (127.0.0.53/54) | `systemd-resolved`, pid 207890 | DNS | preserve |
| 68 | `systemd-networkd`, pid 207631 | DHCP | preserve |

**Nothing else listens that x402 owns.** No second x402-owned listener exists anywhere (all 8xxx ports
scanned: only 8402).

### 1.6 THE DECISIVE QUESTION — does any RETAINED service depend on Node.js or PM2?

**Answer: NO. Nothing retained depends on Node.js or PM2.**

| Retained service | Executable / implementation | Node? | PM2? | Evidence |
|---|---|---|---|---|
| `opensmtpd.service` | `ExecStart=/usr/sbin/smtpd` — ELF 64-bit binary, dpkg `opensmtpd` | **No** | **No** | `file /usr/sbin/smtpd`; `dpkg -S`; `/etc/smtpd.conf` grepped → **zero** node/npm/pm2 refs |
| DKIM signing (mail) | `filter dkim proc-exec "filter-dkimsign …"` → `/usr/libexec/opensmtpd/filter-dkimsign` (compiled binary, pkg `opensmtpd-filter-dkimsign`); key tool `/usr/bin/dkimsign` = **Python** script (pkg `python3-dkim`) | **No** | **No** | `/etc/smtpd.conf` filter line; `dpkg -L`; `file /usr/bin/dkimsign` |
| `dovecot.service` | `ExecStart=/usr/sbin/dovecot -F` — ELF binary, dpkg `dovecot-core` | **No** | **No** | `file`; `systemctl cat`; grep of `/etc/dovecot/**` → no node/npm/pm2/8402 |
| `nginx.service` | nginx, `sites-enabled/` = `default` only (`listen 80 default_server; root /var/www/html; server_name _;`) | **No** | **No** | **No `proxy_pass` anywhere in nginx config**; grep for `8402`/node in nginx config → none |
| `fail2ban.service` | Python; jails `[sshd]` + `[dovecot]` (logpath `/var/log/mail.log`) | **No** | **No** | `/etc/fail2ban/jail.local` + `jail.d/defaults-debian.conf` — no node/x402 jail |
| `certbot.timer` | Python certbot; `authenticator = webroot` | **No** | **No** | `/etc/letsencrypt/renewal/mail.berhampore.in.conf` |
| `snap.oracle-cloud-agent*` | Snap-packaged OCI agent (own runtime) | **No** | **No** | snap units; not in node process list |
| `cron`, `rsyslog`, `ssh`, `systemd-*`, `udisks2`, `iscsid`, `ModemManager`, `unattended-upgrades` | system binaries | **No** | **No** | no ExecStart references node/npm/pm2/npx/node_modules anywhere |

Global Node inventory: the **only** globally-installed npm packages are `corepack`, `npm`, `pm2`
(`npm ls -g --depth=0` → `/usr/lib/node_modules`). `nodejs 22.23.3-1nodesource1` comes from a dedicated
third-party apt repo (`/etc/apt/sources.list.d/nodesource.sources`) — it is **not** a dependency of any
other installed package. `/usr/local/bin` contains only `cloudflared` (a static Go binary, no deb owner).

**Conclusion: Node.js and PM2 exist on this host solely to run x402.**

### 1.7 Shared dependencies — what x402 and the mail server SHARE

| Shared thing | Detail | Why it matters for removal |
|---|---|---|
| **nginx :80 + `/var/www/html`** | `nginx` was installed by the x402 cloud-init (dpkg log 2026-09-30) but is now a **mail dependency**: certbot renews `mail.berhampore.in` with `authenticator = webroot`, `webroot_path = /var/www/html`, and `/var/www/html/.well-known/acme-challenge/` exists (created 2026-10-07 during mail setup). `certbot.timer` is enabled+active (next run 2026-10-18 04:53). | **nginx, its default vhost, and `/var/www/html` MUST NOT be removed or stopped** — doing so breaks mail TLS renewal in 79 days. |
| **Let's Encrypt state** | `/etc/letsencrypt/{renewal,live}` holds **only** the mail cert (`mail.berhampore.in`, valid to **2027-01-05**, account `e8e8e7b…`). Dovecot `ssl_cert = /etc/letsencrypt/live/mail.berhampore.in/fullchain.pem`. | There is **no** x402 cert. Nothing cert-related is x402-owned → touch nothing under `/etc/letsencrypt`. |
| **OCI security list `fyers-sl-public`** | Ingress: 22, 80, 25, 465, 587, 993, 995 from `0.0.0.0/0`; 3306 from `10.0.0.0/24` and from the workstation `106.219.x.x/32`. Used by the shared VCN `berhampore-fyers-vcn` (shared with the other OCI instance). **There is NO ingress rule for 8402.** | Shared list used by another instance → **must not be edited**. Good news: no 8402 rule exists, so retiring x402 needs **no NSG change** — never expose 8402 externally. |
| **Host firewall (netfilter-persistent)** | iptables INPUT: SSH rate-limiting + recent-list rules for 22/587/465/25, plain ACCEPT for 25/465/587/993/995, RELATED/ESTABLISHED, icmp, lo, REJECT default. **`grep -rn '8402' /etc/` returns nothing at all.** | No x402 firewall rule exists to remove. **Do not touch iptables** — the mail ACCEPT rules and the SSH/mail rate-limit rules live in the same persisted ruleset. |
| **Cloudflare zone `berhampore.in`** | `x402.berhampore.in` → Cloudflare-proxied IPv6 (tunnel route); `mail.berhampore.in` → **direct A record** to `161.33.x.x`. The same zone holds the mail MX / SPF / DKIM / DMARC records. One Cloudflare tunnel process only, and its `config.yml` ingress is `x402.berhampore.in → http://127.0.0.1:8402` then `http_status:404` — i.e. **no mail hostname is routed through the tunnel**. | The **zone must be preserved**; only the x402 hostname's route/hostname may ever be deleted, and only after dashboard verification (see UNKNOWN-1). |
| **`/opt/mail.berhampore.in`** | `cleanup.sh` (`#!/bin/bash`, 154 B, root cron hourly) + `mailboxes/` (vmail `mailuser`) | Pure shell + mail data — **no node/npm/pm2**. Preserve entirely; do not go near `mailboxes/`. |
| **logrotate / rsyslog** | No pm2/x402 logrotate or rsyslog config; PM2 writes its own `~/.pm2/*.log` (12 KB total, unrotated) | Removing x402 also removes the only unrotated log producer — no shared config to change. |

### 1.8 Baseline snapshot (sanitized) — before any change

**Resources**
- `uptime`: `up 17 days`, load average `0.02–0.24` (essentially idle).
- RAM: MemTotal **976 904 kB** (954 MB); MemAvailable **447 928 kB** (437 MB); `free -m` → used 516, free 77, buff/cache 538.
- Swap: `/swapfile` 1024 MB, **124.5 MB used** (≈12 %).
- PSI memory: `some avg10=0.00`, `full avg10=0.00` → no memory pressure at snapshot time.
- `vmstat 1 3`: `id` 99–100 %, `si/so` 0 → idle.
- Disk `/`: 45 G total, **4.5 G used (11 %)**, 40 G avail. Inodes: **3 %** used (166 979 / 5 980 160).

**RSS of every identified process**

| PID | comm | RSS (kB) |
|---|---|---|
| 6853 | `node` (x402 worker) | 51 632 |
| 3675 | `PM2 v7.0.4: God` | 47 724 |
| 212414 | `fail2ban-server` | 41 692 |
| 212658 | `fwupd` | 37 700 |
| 207657 | `systemd-journal` | 33 912 |
| 212305 | `multipathd` | 27 296 |
| 4770 | `cloudflared` (x402 tunnel) | 26 232–27 748 |
| 212332 | `smtpd` (mail, base) | 10 148 (4 forked children ~8.2–10.1 MB each) |
| 212363 | `dovecot` | 4 360 |
| 212345/6/7 | `nginx` (master + 2 workers) | 1 600 / 3 868 / 3 868 |

**Mail health (sanitized, health only — no mail content read)**
| Service | active | enabled |
|---|---|---|
| `opensmtpd` | active | enabled |
| `dovecot` | active | enabled |
| `nginx` | active | enabled |
| `fail2ban` | active | enabled |
| `certbot.timer` | active | enabled |
| `cloudflared-x402.service` (user unit) | active | enabled |

Mail listeners present and correct: 25, 465, 587 (smtpd), 993, 995 (dovecot). `/var/log/mail.log` is
**689 944 B, last written 2026-10-17 20:54** → mail is in active production use. Mail cert
`mail.berhampore.in` valid until 2027-01-05 (79 days).

**x402 footprint (for sizing)**
| Path | Size |
|---|---|
| `/home/ubuntu/x402` | **148 M** (all of it `node_modules`: 102 top-level packages; source ≈ 40 KB) |
| `/usr/lib/node_modules` | **41 M** (of which `pm2` = 25 M) |
| `nodejs` deb installed-size | **230 280 kB ≈ 225 MiB** (dpkg; includes `/usr/lib/node_modules/npm`, corepack) |
| `/home/ubuntu/.npm` cache | **18 M** |
| `/home/ubuntu/.pm2` | 72 K (logs 12 K) |
| `/home/ubuntu/.cloudflared` | 12 K (`config.yml` + tunnel creds json) |
| `/usr/local/bin/cloudflared` | 38 M (static Go binary) |
| x402 app dir perms | `/home/ubuntu/x402` `ubuntu:ubuntu 775`; `.env` `ubuntu:ubuntu 600` (contents **not** read); `.git/` present |
| x402 unit / crontab entries | **none** (no systemd unit, no crontab — see §1.4); only the *user* unit `~/.config/systemd/user/cloudflared-x402.service` |

---

## 2. PHASE 2 — REMOVAL MANIFEST

**Legend:** SAFE = confirmed exclusively x402 → may be removed by the next operation.
SHARED - PRESERVE = used by mail or another retained component → must not be touched.
UNKNOWN - STOP = ownership/dependency not established → do not touch until resolved.

> The commands below are **proposals only**. Nothing in this document has been executed.

| # | Item | Package / directory | Startup mechanism | Exclusively x402? | Proposed removal operation | Dependencies & mail risk | Backup / rollback | Expected saving |
|---|---|---|---|---|---|---|---|---|
| 1 | PM2 app `x402-endpoint` (pm_id 0, pid 6853) | runtime process of `/home/ubuntu/x402` | none (manual `pm2 start` from SSI session; no unit/cron) | **YES** | `pm2 delete x402-endpoint` | None. Verified its only purpose is 8402. Mail untouched. | `pm2 jlist`/`dump.pm2` archived (below) | frees ~50 MiB RSS |
| 2 | PM2 God daemon (pid 3675 — **the actual holder of :8402**) | runtime process; `~/.pm2` (72 K) | none | **YES** | `pm2 kill` **after** #1, then confirm `ss -lntp` shows 8402 gone | None. Only child is pid 6853. **Never `killall node`** — use `pm2 kill` only | — | frees ~47 MiB RSS, releases :8402 |
| 3 | x402 application directory | `/home/ubuntu/x402` (**148 M**) | none | **YES** | `rm -rf /home/ubuntu/x402` **only** (never `/home/ubuntu`) | `.env`, `.git`, `deploy/keystore.py`, `scripts/*.mjs` are x402-only; nothing else references the path | **Archive first (mandatory):** `tar czf /home/ubuntu/backups/x402-app-2026-10-17.tgz -C /home/ubuntu x402` then pull off-host and `sha256sum` both copies. Rollback: `tar xzf … -C /home/ubuntu` + #9 + #1 | 148 M disk |
| 4 | PM2 daemon state | `/home/ubuntu/.pm2` (72 K) | none | **YES** | `rm -rf /home/ubuntu/.pm2` after #2 | none | `dump.pm2` included in the archive (#3 backup command should add `-C /home/ubuntu .pm2/dump.pm2`) | 72 K |
| 5 | npm cache | `/home/ubuntu/.npm` (18 M) | none | **YES** (created 2026-09-30 21:22 during x402 install) | `rm -rf /home/ubuntu/.npm` | none | not required | 18 M |
| 6 | `pm2` global package | `/usr/lib/node_modules/pm2` (25 M), `/usr/bin/pm2` → symlink | none | **YES** — only global pkgs are `corepack`, `npm`, `pm2` | `npm rm -g pm2` (or as part of #7) — do **not** run `pm2` before this | none | covered by #7 apt rollback | 25 M |
| 7 | **Node.js runtime + npm/npx/corepack** | deb `nodejs 22.23.3-1nodesource1` (**225 MiB installed**) + `/usr/lib/node_modules` (41 M) + `/usr/bin/{node,npm,npx}` | none | **YES — nothing retained depends on Node** (§1.6) | `apt-get remove --no-auto-remove nodejs` (then verify `apt-get autoremove --dry-run` output line-by-line before ever considering it) | No retained service uses node (§1.6). Mail/nginx/fail2ban/certbot are C or Python. | Reinstall via `apt-get install nodejs` while `/etc/apt/sources.list.d/nodesource.sources` is still in place — so **remove #8 last** | ≈225 MiB + 41 M |
| 8 | NodeSource apt repo + pins | `/etc/apt/sources.list.d/nodesource.sources`; `/etc/apt/preferences.d/nodejs`, `/etc/apt/preferences.d/nsolid` | none | **YES** (exist only to provide nodejs) | `rm /etc/apt/sources.list.d/nodesource.sources /etc/apt/preferences.d/nodejs /etc/apt/preferences.d/nsolid` — **only after #7 is confirmed removed** | none | trivial to re-add (re-run the NodeSource setup for v22) | — |
| 9 | x402 Cloudflare Tunnel — user unit | `~/.config/systemd/user/cloudflared-x402.service` + `default.target.wants` symlink | systemd **user** unit (linger), enabled | **YES** (ingress = `x402.berhampore.in → 127.0.0.1:8402`, else 404; no mail hostname) | `systemctl --user disable --now cloudflared-x402.service` then `rm ~/.config/systemd/user/cloudflared-x402.service && systemctl --user daemon-reload` | None for mail — mail uses direct A record + ports 25/465/587/993/995, **not** this tunnel. **Do not delete the Cloudflare tunnel itself until UNKNOWN-1 is resolved.** | archive the unit file + `~/.cloudflared/config.yml` (tunnel UUID `<tunnel-uuid-redacted>` recorded) | frees ~27 MiB RSS |
| 10 | cloudflared tunnel config + creds | `/home/ubuntu/.cloudflared/` (12 K: `config.yml`, `<uuid>.json` 0600) | as #9 | **YES** | archive then `rm -rf /home/ubuntu/.cloudflared` | Only x402 tunnel. Keep the creds json in the off-host archive in case the tunnel is ever reused | archive both files off-host; rollback = restore dir + re-enable #9 | 12 K |
| 11 | cloudflared binary | `/usr/local/bin/cloudflared` (38 M, no deb owner) | as #9 | **YES** today (only tunnel is x402's) | `rm /usr/local/bin/cloudflared` — **optional**; only if the tunnel is permanently retired | If a mail-facing tunnel is ever wanted, this binary would be needed again. **Defer if unsure.** | re-download via Cloudflare's apt/`curl` install | 38 M |
| 12 | `loginctl enable-linger ubuntu` | `/var/lib/systemd/linger/ubuntu` | logind | **only enabler of #9** | `loginctl disable-linger ubuntu` — **optional; recommend DEFER** (no RAM win, non-zero surprise risk) | Nothing retained depends on linger (mail is all system units). But it is host login-policy state, so treat as deferrable | `loginctl enable-linger ubuntu` | 0 |
| 13 | — | — | — | — | **NO ACTION NEEDED** | There is **no** pm2 systemd unit, **no** `@reboot` cron, **no** rc.local, **no** 8402 iptables/NSG rule, **no** nginx vhost, **no** x402 cert, **no** x402 logrotate/rsyslog entry, **no** fail2ban jail | — | — |
| 14 | nginx + `/var/www/html` + certbot + Let's Encrypt | pkg `nginx`; `/var/www/html` (incl. `.well-known/acme-challenge/`); `/etc/letsencrypt` | `nginx.service` (system unit, enabled); `certbot.timer` | **NO** | **DO NOT TOUCH** | **Mail TLS renewal (webroot HTTP-01) depends on nginx :80 + `/var/www/html`.** Mail cert expires 2027-01-05 | — | — |
| 15 | OpenSMTPD + Dovecot + DKIM + mail certs + `/etc/mail` + `/opt/mail.berhampore.in` + `/var/log/mail.log` | system packages, system units | `opensmtpd.service`, `dovecot.service` (enabled) | **NO** | **DO NOT TOUCH** | LIVE PRODUCTION MAIL | — | — |
| 16 | fail2ban | pkg + `jail.local` (sshd, dovecot) | `fail2ban.service` | **NO** | **DO NOT TOUCH** | Protects SSH + mail | — | — |
| 17 | iptables/netfilter-persistent ruleset | `/etc/iptables/rules.v4` via `netfilter-persistent` | `iptables.service` symlink | **NO** (contains all mail ACCEPT + rate-limit rules) | **DO NOT TOUCH** — no x402 rule exists in it | Mail reachability on 25/465/587/993/995 | — | — |
| 18 | OCI security list `fyers-sl-public` | shared VCN `berhampore-fyers-vcn` | OCI | **NO** (shared with the other instance; has 3306 for the MySQL VPS) | **DO NOT TOUCH** | Would break the other VPS and mail ingress | — | — |
| 19 | Cloudflare zone `berhampore.in` (DNS records, incl. mail MX/SPF/DKIM/DMARC) | Cloudflare, external | — | **NO** (shared zone) | **DO NOT TOUCH** the zone; at most the `x402` hostname/route, gated on UNKNOWN-1 | Deleting the zone or wrong records breaks mail deliverability | — | — |
| 20 | OCI instance `x402-endpoint` itself | compute instance | — | n/a | **OUT OF SCOPE** — the instance is retained (it hosts production mail). Only app-level retirement is proposed | Terminating it destroys the mail server | — | — |
| 21 | Other instance `Dhargent`, my-job-agent MySQL VPS | — | — | n/a | **NOT IN SCOPE — never contacted, never touch** | — | — | — |

### 2.1 Can PM2 be removed?

**YES — PM2 can be removed.** Evidence:
- Exactly one pm2 application exists (`x402-endpoint`), which is exclusively x402 (§1.3).
- The PM2 daemon's only child process is the x402 node worker (verified by full `/proc` PPID scan).
- The PM2 daemon binds **only** port 8402 (the x402 listener) and holds no other socket.
- No systemd unit, cron entry, mail component, nginx config, fail2ban jail, certbot component or shell
  rc file references PM2 or its sockets; `pm2 jlist` count is 0-referenced elsewhere.

### 2.2 Can Node.js / npm be removed?

**YES — Node.js and npm can be removed**, with the ordering caveat that #7 must precede #8 (keep the
NodeSource source file until nodejs is confirmed gone, so rollback is a one-liner). Evidence:
- The only OS processes running `/usr/bin/node` are the PM2 daemon and the x402 worker (§1.2).
- No unit file anywhere in `/etc/systemd/system` or `/usr/lib/systemd/system` has an `ExecStart=`
  referencing `node`, `npm`, `npm`, `npx` or a `node_modules` path (§1.6).
- The only globally-installed npm packages are `corepack`, `npm`, `pm2` — none used by a retained service.
- Mail = C (smtpd, dovecot, filter-dkimsign) + Python (`dkimsign`, fail2ban, certbot); nginx = C;
  cloudflared = static Go; OCI agent = snap. None touches Node.
- `nodejs` comes from a dedicated third-party apt repo; no installed package depends on it.

### 2.3 Concrete backup method (archive OUTSIDE every path being removed)

```bash
# On the target host, BEFORE anything is removed (single dated archive, off the removal paths):
mkdir -p /home/ubuntu/backups
tar czf /home/ubuntu/backups/x402-retire-2026-10-17.tgz \
    -C /home/ubuntu x402 .pm2/dump.pm2 .cloudflared \
    -C /etc/apt/sources.list.d nodesource.sources
systemctl --user cat cloudflared-x402.service > /home/ubuntu/backups/cloudflared-x402.service.txt
ls -l  /home/ubuntu/backups/ ; sha256sum /home/ubuntu/backups/x402-retire-2026-10-17.tgz

# Then pull a second copy off-host (workstation) and verify the digest matches:
mkdir -p ~/backups/x402-endpoint
scp -i ~/.ssh/oci-vm-id_ed25519 ubuntu@<TARGET_IP>:/home/ubuntu/backups/x402-retire-2026-10-17.tgz ~/backups/x402-endpoint/   # <TARGET_IP> = 161.33.x.x
sha256sum ~/backups/x402-endpoint/x402-retire-2026-10-17.tgz
```
`/home/ubuntu/backups/` is deliberately **outside** `/home/ubuntu/x402`, `/home/ubuntu/.pm2`,
`/home/ubuntu/.npm` and `/home/ubuntu/.cloudflared`, so it survives the removal. Record the tunnel UUID
`<tunnel-uuid-redacted>` and the archived `cloudflared` creds json off-host.

### 2.4 Exact rollback steps

```bash
# 1. Restore files
tar xzf /home/ubuntu/backups/x402-retire-2026-10-17.tgz -C /home/ubuntu
# 2. Reinstall Node (source file must still exist; if #8 already ran, re-add NodeSource v22 first)
apt-get install -y nodejs && npm i -g pm2
# 3. Recreate the app environment (deps are in package-lock.json)
cd /home/ubuntu/x402 && npm ci --omit=dev
# 4. Re-create the pm2 app (this is exactly what deploy/start-x402-vm.sh does; it needs PAY_TO)
set -a; . /home/ubuntu/x402/.env; set +a
pm2 start src/server.js --name x402-endpoint --instances 1 --cwd /home/ubuntu/x402 && pm2 save
# 5. Restore + re-enable the tunnel user unit
mkdir -p ~/.config/systemd/user
cp /home/ubuntu/backups/cloudflared-x402.service.txt ~/.config/systemd/user/cloudflared-x402.service
systemctl --user daemon-reload && systemctl --user enable --now cloudflared-x402.service
loginctl enable-linger ubuntu   # required for the user unit to run headless
# 6. Verify
ss -lntp | grep 8402 ; curl -sS -m 10 http://127.0.0.1:8402/healthz
# 7. MAIL SANITY (must be unchanged throughout)
systemctl is-active opensmtpd dovecot nginx fail2ban; ss -lntp | grep -E ':(25|465|587|993|995)$'
```

---

## 3. UNKNOWN — STOP (do not touch until resolved)

| # | Item | Why it is unknown | What to check before acting |
|---|---|---|---|
| **UNKNOWN-1** | **Cloudflare-side state of tunnel `<tunnel-uuid-redacted>` and hostname `x402.berhampore.in`** | Local `config.yml` shows a single-ingress x402 tunnel, but Cloudflare **published application routes configured in the dashboard do not appear in the local config**. The `berhampore.in` zone is shared with mail (MX/SPF/DKIM/DMARC + `mail` A record). I cannot audit Cloudflare without credentials (and must not invent them). | In the Cloudflare dashboard: confirm the tunnel serves **only** `x402.berhampore.in` and that deleting that hostname/route does not disturb the zone's mail records. **Do not delete the zone or the tunnel until this is verified.** |
| **UNKNOWN-2** | Whether the tunnel's `.json` credentials are needed for anything else | The creds file is only referenced by the x402 tunnel, but if the tunnel is left alive (deferred) the json must stay. | Decide #9/#10/#11 ordering; if the tunnel is retired now, archive the json off-host first. |
| **UNKNOWN-3** | Contents/handling of `/home/ubuntu/x402/.env`, `.git/`, `deploy/keystore.py`, `scripts/*.mjs` (and the workstation's `x402-*-ACCOUNT.json`) | **Not read by design** (may hold key material / PAY_TO addresses). Ownership is clearly x402-only, but the *handling sensitivity* is unverified. | Decide the archive-destination sensitivity and whether the local workstation's `x402-*-ACCOUNT.json` files (mode 0600) should also be retired. Archive, never print. |
| **UNKNOWN-4** | Any x402 revenue cutover / DNS dependency from outside this host | `x402.berhampore.in` is publicly resolvable (proxied). External clients/records (e.g. x402 directory listings, an agent registry) may reference it. | Confirm no external system depends on the endpoint being live before the tunnel/DNS record goes away. |

Items **not** in the UNKNOWN list: the host's own ownership picture is fully established — every unit,
cron entry, listener, firewall rule and config file on the box was enumerated, and none of them belongs to
x402 except the items classified SAFE.

---

## 4. WHAT THE NEXT (WRITE) OPERATION MUST NOT DO

- **Never** `pm2 delete all`, `killall node`, `pkill -f node`, or any wildcard deletion.
- **Never** `apt-get autoremove`, `apt purge`, or a blanket `apt` upgrade as part of this retirement
  (an `--no-auto-remove` single-package removal is the only apt action proposed, and only after review).
- **Never** `rm -rf /home/ubuntu`, `/etc`, `/var/lib`, `/usr` wholesale — only the exact paths in the
  SAFE rows above.
- **Never** touch: nginx (config, `/var/www/html`, service), certbot/`/etc/letsencrypt`, OpenSMTPD,
  Dovecot, DKIM filter/keys, fail2ban, iptables ruleset, the OCI security list, the Cloudflare zone, or
  the instance itself.
- **Never** read or print `/proc/*/environ`, `.env`, `~/.cloudflared/*.json`, `/etc/mail/*`,
  `/etc/letsencrypt/**/privkey*`, or mailboxes.
- **Never** access `Dhargent` or the my-job-agent MySQL VPS.

## 5. EXPECTED RESULT

| Resource | Before | After (SAFE rows only; cloudflared items optional) |
|---|---|---|
| RAM | MemAvailable 437 MB; PM2 46.6 MiB + node 50.4 MiB + cloudflared 27.1 MiB RSS | **≈ +97 MiB RSS freed** (PM2 + node); **≈ +124 MiB** with the tunnel. Swap in use (124.5 MB) should fall materially |
| Disk `/` | 4.5 G used of 45 G (11 %) | **≈ −390 MiB** (148 M app + 18 M npm cache + 225 MiB nodejs deb) **+38 M** if cloudflared goes |
| Port 8402 | held by PM2 God | released; no firewall/NSG change required |
| Mail | active | **identical — zero mail-facing artefact in the SAFE set** |

---

## 6. READ-ONLY COMPLIANCE DISCLOSURE (honest side-effects)

No package, service, unit, firewall rule, certificate, DNS record, config file or mail artefact was
created, modified or deleted. All probes were piped over `ssh … 'bash -s'` (stdin) so no script landed on
disk. `pm2 jlist` was the only pm2 command invoked (a read-only RPC listing); no `pm2 start/stop/restart/
delete/save/startup` was ever run. Nevertheless, three *incidental, benign* filesystem touches happened
and are disclosed rather than hidden:

| Touch | Evidence | Assessment |
|---|---|---|
| `/home/ubuntu/.npm/_update-notifier-last-checked` (0 B, 21:27) and two `/home/ubuntu/.npm/_logs/2026-10-17T15_57_50_*.log` (600 B / 1 040 B, 21:27) | Created by the read-only `npm ls -g --depth=0` inventory call (npm's built-in update-notifier + its own debug log) | Harmless: npm's own cache metadata, inside the directory already classified SAFE TO REMOVE (#5). No config, service or state change |
| `/etc/letsencrypt` and `/var/lib/letsencrypt` directory mtimes bumped to 21:28 | The read-only `certbot certificates` query takes a transient lock (`/var/lib/letsencrypt/` is empty again; no file left behind) | No cert, key, account, renewal config or hook was created or altered: `renewal/mail.berhampore.in.conf` still `mtime 2026-10-07 14:28:32, 592 B`, `live/` untouched, cert notAfter still 2027-01-05. The `certbot.timer` next run (04:53) is unaffected |
| `/home/ubuntu/.bash_history` (643 B, 19 lines, `0600`, appeared during the session window) | Content scan by marker (counts only, contents never printed): matches for `jlist`, `IMDS`, `probe`, `ss -lntup`, `pm2` = **0** | **Not attributable to these probes** (no probe command text is present). Most likely the OCI agent's run-command plugin or a pre-existing shell flushing history. Flagged as unattributed rather than claimed as mine; no content was read |

Daemon-driven writes observed but not caused by this task (normal runtime churn): `/var/lib/fail2ban/
fail2ban.sqlite3`, `/var/lib/landscape/landscape-sysinfo.cache`, `/var/lib/systemd/timesync/clock`,
`/var/lib/update-notifier/fsck-at-reboot`. **No file under `/etc` other than the two certbot directory
mtimes above, and nothing under `/etc/systemd`, `~/.config/systemd`, `/usr/lib/node_modules` or a
service definition was modified.**

**Post-inventory state confirmation (21:33 IST, unchanged from the pre-inventory baseline):**
`opensmtpd=active dovecot=active nginx=active fail2ban=active certbot.timer=active`; mail listeners
25/465/587/993/995 all present; `pm2` app count = 1, `x402-endpoint` = `online`, pid 6853; **8402 still
held by the PM2 God daemon**; `/home/ubuntu/backups` does not exist (no backup was taken — that is the
next operation's first step).

