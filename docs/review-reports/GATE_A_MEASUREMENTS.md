# HG-VPN-01 — Gate A, Phase 1: Identity Confirmation & Read-Only Resource Audit

**Target:** Oracle Cloud instance `x402-endpoint` (only), in scope for a future secure
remote-connectivity gateway.
**Mode:** READ-ONLY. No installs, no config edits, no service restarts, no firewall or
security-list changes, no WireGuard activation, no writes of any kind to the host.
**Audit date:** 2026-10-17, ~20:30–20:35 IST (host clock).
**Audit pattern followed:** `~/.hermes/skills/devops/oracle-cloud-infra/references/mail-vm-audit.md`
**Measurements are from a live, idle baseline** — no synthetic benchmark was run.

---

## STEP 0 — Gate: Target Proven

Exactly **one** instance matches in the tenancy's root compartment when filtered on
display-name containing `x402` — no ambiguity, no STOP condition triggered.

| Field | Value |
|---|---|
| Display name | `x402-endpoint` |
| Shape | `VM.Standard.E2.1.Micro` |
| Region | `ap-tokyo-1` |
| Availability domain | `jKmJ:AP-TOKYO-1-AD-1` (single AD) |
| Lifecycle state | `RUNNING` |
| Instance OCID | `ocid1.instance.oc1.ap-tokyo-1.anxhiljro…p2s5p2q` (truncated) |
| Private IP | `10.0.0.89/24` on `ens3` |
| Public IP | `161.33.146.x` (last octet masked) |
| VNIC MAC | `02:00:17:06:74:79` |
| Hostname (`/proc/sys/kernel/hostname`, `hostname -f`) | `x402-endpoint` |
| OS identity | Ubuntu 24.04.5 LTS (Noble), kernel `6.17.0-1020-oracle`, `x86_64` |
| DMI chassis asset tag | `OracleCloud.com` (confirms OCI guest) |
| Login user | `ubuntu` (`opc` refused with "login as the user ubuntu") |
| Uptime | 16 d 23 h, since 2026-09-30 |

**Two independent identity confirmations, both matching the expected OCID exactly:**

1. **OCI CLI** (`~/.oci/config` present, region `ap-tokyo-1`, OCI CLI 3.92.0 at
   `/home/swarna-sekhar-dhar/bin/oci`, auth OK):
   `compute instance list` → 1 result, display-name `x402-endpoint`, shape
   `VM.Standard.E2.1.Micro`, state `RUNNING`.
2. **Instance's own IMDS** (read-only `HTTP GET` to the link-local metadata service,
   authenticated with the instance's own token — no write):
   - `…/opc/v2/instance/id` → `ocid1.instance.oc1.ap-tokyo-1.anxhiljro…p2s5p2q`
   - `…/displayName` → `x402-endpoint`
   - `…/shape` → `VM.Standard.E2.1.Micro`
   - `…/canonicalRegionName` → `ap-tokyo-1`

The CLI-reported `public-ip` equals the address SSH'd to, and the `vnic-id` is the primary
VNIC (`is-primary: true`). Identity is therefore pinned three ways (API + IMDS + SSH host
identity), all agreeing.

**Corroborating project evidence (matches the launch artifacts):**
- `projects/x402/deploy/launch-x402-vm.sh` L63 `--display-name x402-endpoint`, L60
  `--shape VM.Standard.E2.1.Micro`, L42 key `~/.ssh/oci-vm-id_ed25519` (present, mode 0600).
- `projects/x402/deploy/start-x402-vm.sh` runs the app under **pm2** as `x402-endpoint` on
  `PORT=8402` — and on the host, pm2 id 0 **is** `x402-endpoint` on :8402, uptime 16 days.

**Isolation compliance.** Only this instance was inspected or connected to. I did **not**
enumerate other instances, other VNICs, or other compartments; the instance query was
filtered to `x402` and returned a single row, so no other VPS came into view. The unrelated
Oracle VPS and the my-job-agent MySQL VPS were neither contacted nor examined.

---

## STEP 1 — RAM / CPU / Disk Headroom (idle baseline)

### Memory

| Metric | Value |
|---|---|
| `MemTotal` | 976,904 kB = **954 MiB usable** |
| `MemAvailable` (steady, 4 samples over ~3 min) | **461–472 MiB** |
| `MemFree` | 92–108 MiB |
| `Cached` | 456–463 MiB |
| Buffers | ~25 MiB |
| `SwapTotal` | 1,048,572 kB = **1023 MiB** (`/swapfile`, prio −2) |
| `SwapFree` | 920,872 kB → **swap in use ≈ 124.7 MiB (persistent, not churning)** |
| `SwapCached` | 75,920 kB (so ~76 MiB of the swapped-out set is cached/shared pages) |
| `Dirty` / `Writeback` | < 3 MB / 0 |
| Sum of all RSS (overcounts shared pages) | 632 MiB across 252 threads |

**Memory pressure — free and stable:**
- `/proc/pressure/memory`: `some avg10=0.00 avg60=0.00–0.01 avg300=0.00`, **`full` all zero**
  → no process is stalling on memory reclaim, at any window.
- CPU pressure: `some avg10 ≤ 0.43 avg300 0.11`, `full = 0.00`.
- IO pressure: `some`/`full avg10 ≈ 0.62` (idle housekeeping: certbot, sysstat — not workload).

Interpretation: ~450–470 MiB is genuinely available at idle. The persistent 125 MiB of swap
predates this audit (SwapCached = 76 MiB, zero swap-in/swap-out across all `vmstat` samples)
and is cold-page parking, **not** active thrashing. Still, ~125 MiB of swap already consumed
on a 1 GiB VM is the single most important signal here: the box *has* felt memory pressure at
some point in its 17-day life.

### CPU

- **2 vCPU** — `AMD EPYC 7551 32-Core Processor`, 1 socket × 1 core × 2 threads = 2 logical.
- Virtualization: **AMD-V exposed, hypervisor vendor KVM, type `full`** (OCI standard).
- Idle load average `0.02 / 0.02 / 0.00`; `vmstat 1 5` shows **99–100 % idle** every sample,
  `si`/`so` = 0, context switches ~350–770/s, interrupts ~170–520/s.
- No runnable-process backlog (`r = 0`, `b = 0` in all samples).

### Disk

| Mount | Size | Used | Avail | Use% | Inodes used |
|---|---|---|---|---|---|
| `/` (`/dev/sda1`) | 45 G | 4.5 G | **40 G** | 11 % | 166,976 / 5,980,160 (3 %) |
| `/boot` (`sda16`) | 881 M | 158 M | 662 M | 20 % | 604 / 58,496 (2 %) |
| `/boot/efi` (`sda15`) | 105 M | 6.2 M | 99 M | 6 % | n/a |
| `/run`, `/dev/shm` (tmpfs) | 96–478 M | ≤1 % | — | ~0 % | 1 % |

Disk is **not** a constraint: ~40 GB free and 5.8 M free inodes on `/`. Journald holds 89 MB.
Block device `sda` = 46.6 GB with 3 snap loop mounts (`core18`, `oracle-cloud-agent`, `snapd`).

### WireGuard-relevant budget

| Item | Footprint |
|---|---|
| Kernel WireGuard module (in-tree, not loaded) | ~1–5 MB kernel memory, **0 userspace** |
| `wireguard-tools` (`wg` + `wg-quick` shell script) | a few hundred KB on disk; `wg` is used only for config |
| A `wg-quick` systemd unit | effectively 0 (interface lives in-kernel) |
| Available RAM for this | **461–472 MiB idle** |

**Verdict on the 150–200 MB planning target:** satisfied with ~2.3× margin. WireGuard's
kernel datapath is the *cheapest possible* addition to this host — it adds no resident
userspace process. The real memory risk is **not** WireGuard; it is the pre-existing swap
usage and the ~105 MB of idle-but-resident daemons (`fail2ban` 40.9 MB, `fwupd` 37.4 MB,
`multipathd` 27.3 MB) that are not needed on a micro VM but whose removal would be a *change*
and is therefore out of scope.

---

## STEP 2 — Existing Service Footprint & Safety Assessment

### `x402-endpoint` (the production workload — must be preserved)

| | |
|---|---|
| Process | pm2 id 0, name `x402-endpoint`, cluster mode, **PID 6853**, user `ubuntu` |
| RSS | **51.6 MB** (pm2 self-reports 50.1 mb) |
| CPU | **0.1 %** (long-run), 0 % at sample time |
| Memory % of total | 5.2 % |
| Uptime / restarts | **16 days / 0 restarts** |
| Listen | `*:8402` (TCP, dual-stack), plus nginx `:80` in front |
| pm2 God daemon (PID 3675) | 45.1 MB — part of the same app's real cost (total ≈ 96.7 MB) |
| node path | `/home/ubuntu/x402` (contents not read — out of scope) |

### Mail stack (explicitly NOT to be disturbed)

| Service | Footprint |
|---|---|
| OpenSMTPD (`smtpd`) | 56.2 MB across **8 processes** (user `opensmtpd`, 50.8 MB / 6 procs + root-managed) |
| Dovecot | 4.3 MB daemon + 6.6 MB (2 procs, user `dovecot`) |
| DKIM signer | 8.3 MB (2 procs, `_dkimsign`) |
| **Mail aggregate** | **≈ 65–70 MB** |
| Listeners | `0.0.0.0:25`, `:465`, `:587`, `:993`, `:995`; LMTP `127.0.0.1:24` (loopback only) |

Mail TLS ports 993/995/465/587/25 are all bound and reachable — this is a **live production
mail host**. No mail service was restarted, reconfigured, or had its contents inspected.

### Other application / infrastructure overhead

| Component | RSS | Notes |
|---|---|---|
| `cloudflared` (PID 4770, user `ubuntu`) | 26.2 MB | running outside systemd; **not touched** |
| `nginx` | 9.3 MB / 3 procs (7.6 MB as `www-data`) | fronting :80, bound `0.0.0.0:80` + `[::]:80` |
| `fail2ban-server` | 40.9 MB | active, recent-limit rules on 22/25/465/587 |
| `fwupd` | 37.4 MB | firmware daemon — idle cost on a VM |
| `systemd-journald` | 33.0 MB | 89 MB on disk |
| `multipathd` | 27.3 MB | idle cost on a VM |
| `snapd` | 22.3 MB | |
| Oracle Cloud Agent snap stack | **≈ 83 MB** (`oci-wlp` 19.4, `agent` 18.5, `gomon` 16.9, `runcommand` 16.7, `updater` 11.5) | plus `ocarun` 16.3 MB |
| `iscsid` | 16.3 MB (2 procs) | |
| `systemd` (1 + user@1001) | 22.8 MB | |
| `udisksd`, `systemd-resolved`, `ModemManager` | 10.9 / 10.1 / 9.7 MB | |
| `rsyslog`, `polkitd`, `dbus-daemon` | 5.3 / 6.9 / 5.1 MB | |

Aggregated by user: `root` 280.5 MB/116 procs · `ubuntu` 153.5 MB/10 (x402 + cloudflared +
PM2 God + login shells) · `snap_daemon` 64.7 MB/4 · `opensmtpd` 50.8 MB/6 · `systemd+` 24.1 MB/3
· `ocarun` 16.3 MB · `_dkimsign` 8.3 MB · `www-data` 7.6 MB.

**OS / container overhead:** no Docker/Podman/containerd — there are no containers running.
The only "container-ish" overhead is snap (4 loop mounts, snapd 22.3 MB, Oracle agent ~83 MB).

**Short-term peaks observed:** the only CPU activity during the ~5-minute window was the
certbot and sysstat timers (IO pressure `some avg10 0.62`), and my own SSH session
(`sshd` 0.3 % CPU). No application peak occurred; load stayed ≤ 0.12. Established TCP
connections: 9 total (8 established per `ss -s`), i.e. an idle box.

### Headroom for WireGuard

- Idle `MemAvailable` **461–472 MiB**; the entire `x402-endpoint` app costs **≈ 97 MB**
  (node + PM2 God). A kernel-datapath VPN would add **single-digit MB**, leaving
  ≈ **450 MiB** available even in the worst case.
- Startup headroom is comfortable, but this VM's **1 GiB RAM is objectively tight** for
  MAIL + x402 + PM2 + cloudflared + snap/OCI-agent + fail2ban in aggregate.
- **Pre-existing signals that must be carried into the gate decision:**
  1. **124.7 MiB of swap already in use** (cold, but real).
  2. One **failed** transient unit: `test-smtpd-srv.service` (leftover test `smtpd -d -f
     /tmp/test_domain.conf`, failed 2026-10-07, "no rules, nothing to do"). Harmless but
     indicates the host has been hand-worked. Its cleanup would be a *change* — not performed.
- **Conclusion:** 150–200 MB available-RAM planning target is met with large margin, and the
  measured memory **pressure is zero**, so WireGuard itself is safe to add from a capacity
  standpoint. Any future plan should still include swap-usage and MemAvailable monitoring,
  because the margin on this VM comes from cache (reclaimable), not from truly free memory.

---

## STEP 3 — Networking & WireGuard Prerequisites

### Kernel / WireGuard support

| Check | Result |
|---|---|
| Kernel | `6.17.0-1020-oracle` (Ubuntu 24.04.5 noble), `x86_64`, built 2026-07-25 |
| **In-tree WireGuard module** | **PRESENT** — `/lib/modules/6.17.0-1020-oracle/kernel/drivers/net/wireguard/wireguard.ko.zst` |
| Module metadata | `version: 1.0.0`, author Jason A. Donenfeld, "WireGuard secure network tunnel", license GPL v2 |
| Module currently loaded? | **No** (`lsmod` has no `wireguard`; no `tun`/`tap` either) |
| `wireguard-tools` (`wg`, `wg-quick`) | **NOT INSTALLED** (`command -v wg` empty); apt candidate `1.0.20210914-1ubuntu4` available |
| Existing VPN interface | **NONE** (`ip -d link show type wireguard` → empty) |
| DKMS / out-of-tree build needed? | **No** — module ships in the distro kernel, so no compilation, no headers package needed |

**Kernel WireGuard is fully supported and will need no kernel work.** Only userland tooling
(`wireguard-tools`) is missing — installing it is a **change** and is therefore proposed, not
performed.

### Interface / routing structure

```
lo       127.0.0.1/8, ::1/128
ens3     UP   10.0.0.89/24  fe80::17ff:fe06:7479/64   mtu 9000   mac 02:00:17:06:74:79
```

```
default via 10.0.0.1 dev ens3 proto dhcp src 10.0.0.89 metric 100
10.0.0.0/24 dev ens3 proto kernel scope link src 10.0.0.89 metric 100
10.0.0.1     dev ens3 proto dhcp  scope link src 10.0.0.89 metric 100
169.254.0.0/16    dev ens3 proto dhcp scope link src 10.0.0.89 metric 100   # OCI services
169.254.169.254   dev ens3 proto dhcp scope link src 10.0.0.89 metric 100   # IMDS
```

- IP rules: only the **default three** (`0: local`, `32766: main`, `32767: default`). **No
  policy routing in use**, and **no VRF configured**.
- IPv6: only link-local on `ens3`. **No global IPv6 address** (`net.ipv6.conf.all.disable_ipv6
  = 0`, but no global prefix delegated) → a WireGuard tunnel here would be **IPv4-only**;
  dual-stack is not possible as-is.
- `net.ipv4.ip_forward = 0` and `net.ipv6.conf.all.forwarding = 0`.
- `net.ipv4.conf.{all,default}.rp_filter = 2` (**loose** mode) — tolerant of asymmetric paths,
  which is favourable for tunnelled traffic.
- `net.ipv4.conf.all.route_localnet = 0`.
- VCN interface MTU is **9000** (OCI jumbo frames inside the VCN); the internet path is 1500,
  so a WireGuard interface will need an explicit MTU (≈1420, or ≈1380 if carried inside the
  Cloudflare tunnel) — a *config* consideration, not a blocker.
- Networking is managed by **cloud-init + systemd-networkd** (`/etc/netplan/50-cloud-init.yaml`),
  **no NetworkManager**. A new `wg*` interface does not disturb `ens3` — but note the
  `cloudflared` binary in use implies an existing tunnel path on this host.

**Routing-change risk — explicit warning.** A `wg-quick` config with
`AllowedIPs = 0.0.0.0/0` would install policy rules and **replace the default route**,
breaking the box's own outbound path (certbot, apt, mail relay, cloudflared, OCI IMDS access).
On a production mail + payments host that is a hard no. Any future tunnel must be configured
**split-tunnel** (dial-in / specific peer routes only), and `Table`/`RouteTable` settings must
be chosen deliberately rather than by `wg-quick` default. This is the top networking risk
found on this host.

### UDP port feasibility

Existing UDP listeners (all non-colliding):

```
127.0.0.54:53      (systemd-resolved stub)
127.0.0.53%lo:53   (systemd-resolved stub)
10.0.0.89%ens3:68  (DHCP client — OCI DHCPv4)
*:37570  *:43585  *:46670  *:59103   (ephemeral, high-numbered)
```

| Candidate | Status |
|---|---|
| **UDP/51820** | **FREE** — not bound by any listener (`ss -lunH | grep -w 51820` → no match) |
| UDP/51821 | free (alternative) |

`51820` is a **candidate only — not approved and not reserved**. Nothing was bound or opened.

### Two firewalls stand in front of any inbound UDP/51820 (both currently block it)

**(a) Host firewall — `iptables-nft` (iptables 1.8.10-3, `iptables-persistent` enabled):**

`INPUT` policy is `ACCEPT` but the **final rule rejects everything**:
`-A INPUT -j REJECT --reject-with icmp-host-prohibited` → effective **deny-by-default**.
Explicit accepts: TCP `22` (rate-limited 10 hits/60 s), `25` (60/60 s), `465` (12/60 s),
`587` (12/60 s), `993`, `995`, `RELATED,ESTABLISHED`, `icmp`, `-i lo`.
**There is no UDP accept rule of any kind** → inbound UDP/51820 would be **rejected at the
host**. `FORWARD` chain contains a blanket REJECT rule (relevant if transit forwarding is ever
needed). No UFW installed; nftables is driven by `iptables-nft` ("table ip filter is managed
by iptables-nft, do not touch!") — so a hand-written nftables rule would be overwritten/misplaced.

**(b) OCI security list `fyers-sl-public` on subnet `fyers-subnet-public` (10.0.0.0/24):**

| Direction | Rules |
|---|---|
| **Ingress** | TCP 22 ← 0.0.0.0/0 · TCP 80 ← 0.0.0.0/0 · TCP 25 ← 0.0.0.0/0 · TCP 465 ← 0.0.0.0/0 · TCP 587 ← 0.0.0.0/0 · TCP 993 ← 0.0.0.0/0 ("IMAP over TLS mail.berhampore.in") · TCP 995 ← 0.0.0.0/0 · TCP 3306 ← 10.0.0.0/24 · TCP 3306 ← 106.219.73.40/32 |
| **Egress** | all protocols → 0.0.0.0/0 |
| **NSGs attached to the VNIC** | **none** (`nsg-ids: []`) — one security list only |
| Inbound UDP rules | **NONE** |

So an inbound UDP/51820 also requires a **new OCI ingress rule (protocol 17/UDP,
dst-port-range 51820)** — which is explicitly prohibited in this phase. Nothing was created.

**⚠ Shared-security-list finding (network blast radius).** The security list is named
`fyers-sl-public`, the subnet `fyers-subnet-public`, and it carries a MySQL rule
(`TCP 3306 ← 10.0.0.0/24`) plus a home-IP rule (`TCP 3306 ← 106.219.73.40/32`). This is a
**shared, pre-existing network boundary**, i.e. sibling VMs in the same 10.0.0.0/24 subnet are
governed by the *same* security list. Therefore:

> **Adding an ingress rule to `fyers-sl-public` would open that port on EVERY VM attached to
> it — not just on `x402-endpoint`.** The correct future approach is a **dedicated NSG**
> created and attached to *this VNIC only* (or a dedicated security list), with the source
> limited to the specific client CIDR — never `0.0.0.0/0`.

I deliberately **did not** enumerate the other VNICs/instances on that subnet, to respect the
"never inspect any other instance" rule. I note the shared boundary as a *finding to
re-verify* at the change-approval gate, not as a verified peer inventory.

**Summary of what a future inbound UDP/51820 would need (all currently PROHIBITED — proposals only):**
1. An OCI ingress rule allowing UDP/51820, ideally via a **new dedicated NSG** attached to this
   VNIC only (never the shared `fyers-sl-public`), source = the specific client CIDR only.
2. A host `iptables` ACCEPT for `udp --dport 51820` inserted **before** the trailing REJECT,
   plus `iptables-save`/`netfilter-persistent` so it survives reboot.
3. `wireguard-tools` installed (kernel module already present — no DKMS).
4. WireGuard configured **split-tunnel** (never `AllowedIPs = 0.0.0.0/0`), with an explicit
   reduced MTU for a 9000-MTU VCN / 1500 internet path.
5. Decision on `net.ipv4.ip_forward` — **leave 0** for a pure client dial-in; set 1 only if the
   host must route between peers (a change requiring review of the `FORWARD` REJECT rule).

---

## Not checked, and why (explicit gaps)

| Not checked | Why |
|---|---|
| Anything about any other OCI instance or VPS | Hard prohibition. Query was filtered to `x402` only; no other instance was listed, inspected or contacted. |
| Full instance inventory of the compartment / total instance count | Would require enumerating other instances; deliberately avoided. Identity was proven by exact-match on one row instead. |
| Other VNICs sharing subnet `fyers-subnet-public` | Same reason — the shared security list is flagged as a risk to re-verify at approval time rather than enumerated now. |
| `wireguard-tools` install, module load, key generation, tunnel up | Prohibited (changes). Verified only that the module *exists and is loadable in principle*. |
| Any inbound rule / NSG creation / security-list edit | Prohibited. Documentation only. |
| `/home/ubuntu/x402` app config and `.env` | Out of scope and could contain an address/secret — not read. |
| nginx vhost contents, cloudflared config/credentials, mail spool contents, mail service state files | Out of scope and/or privacy-sensitive. Only listener presence and process RSS were observed. |
| System package upgrade state details, apt history | Not required for Gate A; `wireguard-tools` candidate version noted only. |
| A real WireGuard handshake / throughput test | Would require keys + activation (prohibited), and synthetic load is excluded by the task. |
| OCI console (web) access, support-ticket-side controls | Not available from this environment; the OCI CLI is the only authorized read path and it was used read-only. |

## Change proposals raised (NONE executed)

1. `apt install wireguard-tools` (kernel module already in-tree).
2. Host `iptables` accept for UDP/51820 before the trailing REJECT, persisted.
3. **Dedicated NSG** (or dedicated security list) for UDP/51820 ingress — *not* the shared
   `fyers-sl-public`.
4. `wg-quick` config in **split-tunnel** mode with explicit reduced MTU.
5. (Conditional) `net.ipv4.ip_forward=1` + `FORWARD` chain review, only if inter-peer routing
   is required.
6. (Housekeeping, unrelated) retire the failed transient unit `test-smtpd-srv.service`.

## Gate A verdict

- **Identity: CONFIRMED** (API + IMDS + SSH host, one candidate only).
- **Capacity for WireGuard: ADEQUATE** — 461–472 MiB idle MemAvailable vs a ~single-digit-MB
  kernel-datapath addition; zero memory pressure; disk and inode headroom not a concern.
- **Existing services: SAFE to coexist** — x402 (≈97 MB incl. PM2) and mail (≈65–70 MB) were
  untouched and are idle; the flagged residual risk is the **pre-existing 124.7 MiB swap** and
  tight 1 GiB total RAM, which warrants monitoring rather than blocking.
- **Kernel/WireGuard: READY** — in-tree module 1.0.0 present, no DKMS needed; only
  `wireguard-tools` userland is absent.
- **Networking: FEASIBLE BUT GATED** — UDP/51820 free; **two** independent firewalls (host
  iptables trailing REJECT + OCI security list with no UDP rule) must both be opened, and the
  security list is **shared**, so a dedicated NSG/security list is required. `wg-quick`'s
  default full-tunnel behaviour is a **real hazard** on this production host.
- **Nothing was changed on the host or in OCI. No write occurred.**
