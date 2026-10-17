# HG-VPN-01 — Gate A findings and deployment blueprint

Read-only assessment. Nothing on any server was changed, and this is not authorization to execute
anything in the runbook. Sanitized: no addresses beyond a masked public IP, no keys, no credentials, no
mail details, no host inventory.

## Decision: **CONDITIONAL**

The VPS is suitable for the proposed lightweight control-plane role, but three named issues must be
resolved before Gate B: the shared security list, the default-route hazard, and the pre-existing swap on
a 1 GiB host. It is *not* suitable as a media relay (see Scenario C).

## Gate 0 — target identity: CONFIRMED (one candidate, no stop condition)

Triple-pinned, not assumed from a previous record:

- OCI CLI, filtered to the display name → exactly one instance, `RUNNING`.
- Instance metadata service on the host returned the **same OCID** as the CLI, plus display name, shape
  and region.
- SSH host identity: `hostname=x402-endpoint`, Ubuntu 24.04.5, kernel `6.17.0-1020-oracle`, DMI asset tag
  `OracleCloud.com`, and `opc` refused with `ubuntu` accepted.
- Corroborated by `/etc/os-release`, the project's own launch script (lines 42/60/63) and the running
  pm2 process named `x402-endpoint` on its service port.

Only the `x402` instance was queried — no other instance, VNIC or compartment was listed, inspected or
contacted. **Flag for the owner:** the measured public address falls in `161.33.146.x`, which matches a
host I had recorded earlier as "do not access". This brief explicitly authorizes *read-only* assessment of
the designated `x402-endpoint` machine, so I proceeded; if that identification is wrong, say so and I will
drop the host immediately. Gate B remains unauthorized either way.

## Step 1 — measured capacity (idle, four samples over ~3 minutes, no synthetic load)

    RAM            954 MiB total; MemAvailable 461-472 MiB (free 92-108, Cached ~460)
    Swap           1023 MiB, 124.7 MiB already used, SwapCached 76 MiB, zero si/so
    Pressure       ZERO - /proc/pressure/memory 'full' all-zero at every window
    CPU            2 vCPU (AMD EPYC 7551), KVM full virtualization, AMD-V exposed
                   load 0.02/0.02/0.00, vmstat 99-100% idle, r=b=0
    Disk           45 G total, 4.5 G used (40 G / 11% free), inodes 3% used, journald 89 MB

Available RAM sits **well above** the 150-200 MB planning target (about 2.3x margin), and kernel
WireGuard adds no resident userspace process. The residual risk is the pre-existing swap usage combined
with a tight 1 GiB total: monitor it, do not treat it as a blocker. Post-audit re-check confirmed every
service still active, the x402 PID unchanged, `wg` still absent and 51820 still unbound.

## Step 2 — existing footprint

    x402 application   ~97 MB (node 51.6 + PM2 god 45.1), 0.1% CPU, 0 restarts
    mail stack         ~65-70 MB (smtpd 8 procs 56.2, dovecot ~11, dkim 8.3), untouched,
                       listeners 25/465/587/993/995 intact
    other              cloudflared 26, nginx 9, fail2ban 41, fwupd 37, multipathd 27,
                       Oracle agent snap ~83 MB; no containers
    one transient unit  test-smtpd-srv.service failed (harmless, noted not investigated)

## Step 3 — kernel and networking prerequisites

Kernel WireGuard is **ready**: the in-tree `wireguard.ko.zst` v1.0.0 is present, so no DKMS is needed. The
module is not loaded, no WireGuard interface exists, and `wireguard-tools` is absent (an apt candidate was
noted, not installed).

Single interface `ens3` (mtu 9000), one default route, **no policy routing or VRF**, **no global IPv6**,
`ip_forward=0`, `rp_filter=2` (loose, favourable). **UDP 51820 is free** — a candidate only; nothing was
bound and no port is reserved or approved.

**Two independent firewalls block inbound UDP:** the host `iptables-nft` is deny-by-default with a
trailing REJECT and no UDP accept rule, and the OCI security list in use has no UDP ingress and no
network security group attached. Any future ingress therefore requires changing both layers, which is a
change and was not performed.

### Two findings to escalate

1. **The security list is shared.** It is named for another workload and contains a rule exposing MySQL
   to `10.0.0.0/24`, implying sibling VMs in this subnet. Adding a UDP rule to it would open **every**
   attached instance. A **dedicated NSG** must be created and attached instead — that is an owner
   decision and a real infrastructure task, not a tweak.
2. **A default-route hijack is one careless command away.** A `wg-quick` config with the usual
   `AllowedIPs = 0.0.0.0/0` would take over the default route on a production mail-and-payments host.
   **Split-tunnel only**, with an explicit permitted-peer/subnet policy and no default route.

## Step 4 — capacity model (paper, 14 marked assumptions, in the capacity document)

    Scenario A  control plane, 5/10/25 devices   ~262 bit/s per device
                0.43-2.13 GB/month               PLAUSIBLE
    Scenario B  occasional encrypted audio       128 kbit/s aggregate
                1.7-41.5 GB/month                PLAUSIBLE at low duty, CONDITIONAL otherwise
    Scenario C  CCTV media through the VPS       4-16 Mbit/s
                1.3-5.2 TB/month                 NOT VIABLE - requires a different media path

Scenario C fails on the ~50 Mbps ceiling, monthly transfer, disk and project policy. A kernel WireGuard hub
forwards encrypted packets without decoding them — a CPU saving, not a bandwidth saving — and a central
WireGuard server **does not by itself establish direct peer-to-peer media**; it provides roaming and
coordination only.

## Steps 5-8, 10 — the blueprint (written, not executed)

Five documents, 931 lines, at `~/.hermes/cache/scratch/hg-vpn-01/docs/`:

    HG_VPN_00_SITE_CONNECTOR_DESIGN.md          trusted per-site connector for cameras that cannot run a
                                               VPN client; outbound-only dial, durable {site_id,device_id}
                                               identity, explicit camera authorisation, per-camera consent,
                                               four-way availability reporting, separate revocable
                                               credentials, never exposing camera services publicly; states
                                               that VPN membership is NOT camera-level authentication
    HG_VPN_01_VPS_CAPACITY_AUDIT.md             the model above, with assumptions marked
    HG_VPN_02_MULTI_NETWORK_ARCHITECTURE.md     identity plane (reuses the existing CA, fingerprint
                                               registry and consent gate - no second pairing authority),
                                               coordination plane (outbound-only, endpoint rediscovery
                                               across ISP/DHCP/provider changes), media plane (direct
                                               authenticated transport, ICE/STUN, relays opt-in and
                                               owner-approved only); explains why per-camera VPN membership
                                               is unnecessary and a single camera subnet is ruled out
    HG_VPN_03_INSTALLATION_AND_ROLLBACK_PLAN.md the 14-step runbook, NOT to be executed, 16 steps marked
                                               [PROD], with a 7-row per-change reversal table that
                                               preserves mail data/config, x402, DNS and certificates;
                                               split-tunnel only; includes the local-only sandbox plan
    HG_VPN_04_SECURITY_AND_APPROVAL_GATES.md    the eleven threat areas each with a fail-able test, plus
                                               an 11-item approved-change list for Gate B

**Not done, deliberately:** no package installed, no module loaded, no key generated, no interface, no
rule, no NSG, no routing change, no service touched. Six proposals were documented rather than executed.

**Documentation branch:** the documents are written and sanitized but **not committed** — creating a
`docs/hg-vpn-01-oracle-feasibility` branch and committing them needs your approval, and I will not push
without it. The earlier assumption-versus-measurement gap is now closable: the capacity document assumed
150-200 MB available RAM and unknown disk, and the audit measured 461-472 MiB available and 40 G free, so
the "assumed, not measured" notes can be replaced with real figures once the branch is authorized.

## Risks and rollback

Principal risks: a route hijack from a careless configuration (mitigated by split-tunnel policy and the
runbook's explicit prohibition), shared-security-list collateral exposure (mitigated by a dedicated NSG),
and memory tightness on a 1 GiB host that already swaps (mitigated by monitoring and by WireGuard adding no
resident process). Every runbook step has a matching reversal and a post-change check that x402 and mail
remain operational.

## Next action

Owner decision required on: the dedicated NSG, the split-tunnel peer policy, the candidate UDP port, and
authorization to create the documentation branch. Gate B is not started and will not begin without
granular approval.
