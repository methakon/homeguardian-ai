# H-04 - Oracle Cloud VPS relay: architecture and deployment plan

**STATUS: PLAN ONLY. Nothing on the Oracle VPS has been created, started, stopped, reconfigured or
deleted. No Oracle security list, firewall rule, cloud resource or port exposure has been changed.**
This document exists so the owner can review the architecture and the keep/remove decision before any
action is taken. Every step below is written to be executed later, one gated step at a time.

Owners of record: the VPS configuration is supplied by the owner (inventory checklist in section 2).
The tool that matters for certificates already exists (`tools/provision_ca.sh`, subcommand `server`),
so no new certificate tooling is required - see section 7.

---

## 1. Target architecture and why the relay is dumb

    [ Android node A ]  --\                     /--> [ Ubuntu hub : ingress 8443 ]
    [ Android node B ]  ----> ( WireGuard ) -> VPS  ( route/forward only )
    [ Android node C ]  --/                     \--> (no TLS termination, no keys, no CA)

The relay exists for exactly one reason: the Ubuntu hub sits behind a home NAT with no public
address, so remote Android nodes cannot reach it directly. The relay provides a stable, reachable
meeting point.

Three properties are non-negotiable, and they are what make this design small:

1. **The relay never terminates TLS.** It forwards packets. HomeGuardian's existing end-to-end
   mutual TLS stays exactly as it is between the phone and the hub; the relay sees WireGuard
   ciphertext wrapping TLS ciphertext. It holds no CA certificate, no server key, no device key,
   no registry and no replay state.
2. **The relay is not a second HomeGuardian instance.** No Docker, no SQLite, no ingress, no hub.
   It runs WireGuard and a firewall. This keeps the footprint inside the small VPS the owner already
   has (an H-01-era measurement of the VM the owner described: ~952 MB RAM / 2 cores - a pure relay
   is comfortable there; running a second hub would not be).
3. **The phone dials the hub's stable VPN address, not the VPS's public address.**
   `10.66.0.2:8443` (the hub's address inside the VPN) never changes when the VPS's public IP or DNS
   name changes, so the hub's certificate SAN does not depend on the relay and the phone's saved
   endpoint does not need re-issuing when the VPS moves.

Both the hub and the phone are WireGuard **clients** that dial OUT to the VPS (section 6). The VPS
is the only peer with a stable public address; the hub's outbound tunnel plus a persistent keepalive
is what keeps the return path open through its home NAT.

## 2. Oracle VPS inventory and compatibility (what I need from you)

Nothing here is assumed. Please supply the output of these read-only commands (or the equivalent
from the Oracle console), and I will fill in this section before any change is proposed:

| # | item to capture | command (run by you, on the VPS or in the console) |
|---|---|---|
| 1 | OS and kernel | `cat /etc/os-release; uname -a` |
| 2 | CPU / RAM / disk | `nproc; free -m; df -h /` |
| 3 | Shape and region | `curl -s -H "Authorization: Bearer Oracle" http://169.254.169.254/opc/v2/instance/` (read-only metadata) |
| 4 | Public address(es) | `ip -brief addr; curl -s ifconfig.me` |
| 5 | WireGuard availability | `wg --version 2>/dev/null \|\| echo "needs install"; lsmod \| grep -c wireguard` |
| 6 | Listening sockets | `ss -tulpn` |
| 7 | Containers | `docker ps -a --format '{{.Names}} {{.Image}} {{.Status}}'; podman ps -a 2>/dev/null` |
| 8 | systemd units (enabled) | `systemctl list-unit-files --state=enabled \| head -40` |
| 9 | Crontabs | `crontab -l; ls /etc/cron.*/` |
| 10 | Host firewall | `sudo iptables -S; sudo nft list ruleset 2>/dev/null` |
| 11 | VCN security lists / NSGs | Oracle console (egress + ingress rules, per subnet) |
| 12 | Existing users / keys | `ls -la ~/.ssh; getent passwd \| awk -F: '$3>=1000'` |
| 13 | Inbound reachability | (from outside) `nc -vz <public-ip> 22` and a scan of the ports you expect |
| 14 | Anything the owner wants preserved | your own list - workloads, DNS names, TLS certs for other services |

Compatibility notes to confirm against that inventory:

* **WireGuard**: needs a kernel module or `wireguard-go`. Debian/Ubuntu 22.04+ and Oracle Linux 8+
  ship one; a minimal install may need `apt install wireguard-tools`. If the kernel has no module,
  userspace `wireguard-go` is the fallback and the plan must say which is in use.
* **Kernel forwarding**: `net.ipv4.ip_forward=1` is required for the relay to route between peers.
* **Oracle firewall is TWO layers**: the VCN security list / NSG *and* the instance's own
  iptables/nftables. On Oracle Linux images the host firewall commonly rejects everything except 22
  by default, and an ingress rule alone does not open a port. Both layers must be stated explicitly
  in the change plan, and both must be reverted in a rollback.
* **RAM**: a relay with WireGuard + sshd + journald fits comfortably in 1 GB. Do not plan to run
  Docker, a database or the hub there.

## 3. Existing services, containers and firewall rules - and the keep/remove review

The review is a table the owner signs off, with three verdicts: **KEEP**, **REMOVE**, **UNSURE**.
Nothing is removed on a guess, and nothing is removed before the replacement path is proven.

Proposed review form (filled from section 2, before any change):

| service / container / rule | what it does | last activity | proposed verdict | evidence needed to remove |
|---|---|---|---|---|
| sshd (22) | administration | in use | KEEP (tighten: keys only, no passwords) | - |
| (from `docker ps -a`) | ... | ... | KEEP / REMOVE / UNSURE | owner confirmation |
| (from `ss -tulpn`) | ... | ... | ... | ... |
| VCN ingress rules | ... | ... | REMOVE only unneeded ones | a scan proving nothing uses them |
| cron jobs | ... | ... | ... | ... |

Rules for the review:

* **A service is not removed because it looks unused.** Last-activity evidence first (logs,
  connection counts, the owner's own knowledge).
* **Nothing is deleted.** "Remove" means stop + disable, keep the data, and record exactly how to
  start it again. Deletion is a separate, later decision with the owner.
* **The relay's own additions stay reversible**: one interface (`wg0`), one unit
  (`wg-quick@wg0`), and a small set of firewall rules that are listed verbatim in the rollback
  section.
* **DNS/HTTP services unrelated to HomeGuardian are out of scope** unless the owner lists them as
  removable - a HomeGuardian relay has no business touching them.

## 4. Public IP and private VPN addressing

| role | address | notes |
|---|---|---|
| VPS public | `<from inventory>` (or a DNS name the owner controls) | the only public address in this design |
| VPS inside VPN | `10.66.0.1/24` | relay endpoint, reachable by peers |
| Ubuntu hub inside VPN | `10.66.0.2/24` | **the address phones dial** (`10.66.0.2:8443`) |
| Android nodes | `10.66.0.10`, `.11`, `.12`, ... | one /32 per device, statically assigned |
| VPN interface | `wg0`, UDP 51820 | one interface, one port, no fallback port |

Why a routed VPN address for the phone->hub leg instead of forwarding the VPS's public 8443:

* the hub's certificate SAN becomes a stable VPN address (`IP:10.66.0.2`) that does not change when
  the VPS's public IP changes, so the phone's saved endpoint survives a VPS rebuild;
* the relay needs no DNAT, no conntrack rule and no proxy - only forwarding;
* `8443` is never exposed publicly on the relay (only UDP 51820 is), so a port scan of the VPS
  cannot even see that a TLS service exists behind it.

The client key material is generated per device and never leaves it (the private key is generated on
the phone or transferred once by the operator, and never stored on the relay).

## 5. Android API 27 compatibility (the current node)

The existing node is Android 8.1 / API 27 / armeabi-v7a (MediaTek MT6580). Constraints this imposes
on the relay plan:

* **The official WireGuard Android app requires Android 5.0+** (API 21), so API 27 is supported.
  The owner's consent to install a VPN app is a prerequisite: an Android VPN service shows the
  system VPN key icon and requires the user to approve the tunnel. **This is an owner decision and
  must be requested explicitly - this plan does not assume it.**
* **Alternative without a VPN app: `wg-quick` on a rooted device or a userspace tunnel** - neither is
  acceptable here (rooting the phone is forbidden by the existing constraints; a userspace tunnel
  would have to be written and reviewed, which is far out of scope).
* **If the VPN app is not acceptable**, the fallback is a relay that forwards the phone's TLS
  connection directly. That requires either a public listener on the VPS (rejected: it would put the
  device boundary on a shared host) or a TCP forwarder whose only job is byte-forwarding. Both are
  inferior: the first exposes the boundary, the second adds a moving part with no security benefit.
  **Recommendation: the WireGuard app, if the owner accepts a tunnel on the phone.**
* The app itself needs no code change for the relay: it already takes a host + port plus its
  PKCS#12 credentials, and the endpoint it must dial (`10.66.0.2`) is simply a different host.
* `observation_interval_ms` (clamped 100-60000 ms) remains the single lever for mobile-data volume
  (section 10).

## 6. Ubuntu-to-Oracle outbound VPN connectivity (the home NAT problem)

The hub has no public address, so it cannot be dialled. Therefore:

* the hub is a WireGuard **client** of the VPS: `AllowedIPs = 10.66.0.0/24` (only the VPN subnet -
  never `0.0.0.0/0`, which would try to route the hub's whole internet through the VPS),
  `PersistentKeepalive = 25` so the home NAT's mapping never expires;
* the hub's wg private key is generated on the hub and never copied elsewhere; the VPS stores only
  the corresponding public key;
* on the VPS, the hub's peer entry is `AllowedIPs = 10.66.0.2/32` so the relay will only ever route
  the hub's own address to it;
* the return path is the established tunnel (the relay answers on the session the hub opened), so
  no inbound rule on the home router is needed - **the home router and its firewall are unchanged by
  this milestone**;
* the hub keeps serving its LAN endpoint (`192.168.1.6:8443`) unchanged, so the existing local Wi-Fi
  route remains available and is the rollback path.

## 7. Certificate SAN and endpoint validation (no new tooling needed)

`tools/provision_ca.sh` already has the subcommand this needs:

    bash tools/provision_ca.sh server --server-names "DNS:localhost,IP:127.0.0.1,IP:192.168.1.6,IP:10.66.0.2"

* It re-issues **only the hub's server certificate**; the CA and every device certificate are
  untouched, so nothing has to be re-issued on the phones (they trust the CA, not the server cert).
* The device client verifies the address it dialled via `X509_VERIFY_PARAM_set1_ip/host`
  (D-03.1) with **no way to disable it**, so the new SAN entry is what makes `10.66.0.2` acceptable -
  and a certificate without it will fail loudly rather than falling back to an unverified connection.
* `scripts/verify_tls_identity.sh` covers the SAN matrix; H-04 adds the relay address as one more
  case.
* The relay itself presents nothing and verifies nothing at the TLS layer: it is not a party to the
  TLS session, which is the property that keeps the mutual-TLS guarantee end-to-end.

## 8. Multiple authenticated Android nodes

Three independent identities per node, none of which the relay can forge:

| layer | what identifies the node | where it lives |
|---|---|---|
| WireGuard | one key pair per node, `/32` address | node + one peer block on the relay |
| device certificate | CN = node id (e.g. `hg-443a03b54537c21f`) | node (PKCS#12, app-private storage) |
| registry binding | SHA-256 fingerprint of that certificate | hub's credential directory |

* The relay routes **only** `10.66.0.x/32 <-> 10.66.0.2:8443`. A node cannot reach another node, and
  cannot reach the relay's own services.
* Adding a node is: (1) a WireGuard peer block, (2) `provision_ca.sh issue <id>`, (3) consent +
  pairing in the registry. Removing one is: revoke in the registry (the ingress refuses it at
  delivery time within `authorization_refresh_ms`) and remove the peer block.
* The existing identity-mismatch protection (a valid certificate claiming another device's id is
  refused `identity_mismatch`) is unchanged by the relay, and is re-proven in H-04 acceptance.

## 9. Network switching and reconnection

| event | what handles it |
|---|---|
| phone moves Wi-Fi -> mobile data (IP changes) | WireGuard roaming: the tunnel rekeys and follows the new source address |
| NAT mapping expiry (idle node) | `PersistentKeepalive = 25` both ways |
| TLS session breaks mid-delivery | the app's existing transport retry with `welcome.last_sequence` resume; an unacknowledged observation is retried and answered idempotently (D-03.2) |
| hub's home IP changes | the hub's tunnel re-establishes outbound; the VPS sees the new source (no DNS update needed on the relay side for the phone->hub leg, which uses the VPN address) |
| VPS public IP changes | only the two clients' `Endpoint=` needs updating; the phone's dialled address (`10.66.0.2`) and the certificate SAN do not change |
| relay reboot | peers re-handshake automatically; no durable state to restore (the relay is stateless by design) |

Acceptance must include an actual network switch with the tunnel live (section 12, test 5).

## 10. Bounded traffic and resource usage

Estimates, labelled as estimates because H-03 has not yet measured bytes on the wire - H-04
acceptance measures them:

* one observation frame carries compact numeric features (no pixels, no PCM), bounded by the D-03
  frame cap of 4 KiB; real frames are expected in the low hundreds of bytes;
* at the default 1000 ms interval with camera motion plus three audio scalars (~4 observations/s),
  a rough 250-500 B/frame gives ~1-2 KB/s, i.e. **~3.6-7 MB/h / ~90-170 MB/day**;
* a conservative mobile profile (interval raised to 5000 ms, audio features every interval) gives
  **~0.2-0.4 KB/s / ~20-35 MB/day** - this is the recommended default for a phone on mobile data;
* WireGuard keepalive at 25 s is ~80 B per keepalive per peer: negligible (<1 MB/day);
* hard bound: the app's `observation_interval_ms` (100-60000 ms) and the transport's bounded queue
  (drop-with-count, never unbounded growth). The plan must state the byte budget the owner accepts
  **before** a node is enabled on mobile data, and the app's counters (frames written, dropped,
  refused) are the on-device evidence that it holds;
* relay resources: WireGuard forwarding is kernel-side; expect a few MB of RSS for
  `wg-quick`/sshd, negligible CPU at 1-5 obs/s, and no growth over time (no state). The VPS must
  **not** host the hub, a database, or Docker for this milestone.

## 11. Minimal Oracle firewall exposure

Intended end state (to be confirmed against the inventory, then applied as one reviewed change):

| direction | protocol/port | source | purpose |
|---|---|---|---|
| inbound | UDP 51820 | anywhere (or the owner's known address ranges if they prefer) | WireGuard |
| inbound | TCP 22 | the owner's addresses only (not 0.0.0.0/0) | administration |
| **inbound** | **TCP 8443** | **never** | the device boundary stays behind the tunnel |
| outbound | UDP to the hub's home public IP:51820-equivalent | relay | not needed: the hub dials out, so the relay only answers |
| outbound | DNS/NTP/updates | relay | as the OS needs |

Explicitly **not** done: no public 8443, no port range, no "allow all from the VPN subnet to
everything", no Oracle-managed WAF bicycles, no additional public services to administer.

## 12. Security and acceptance tests

Security invariants for H-04 (each one gets a test, not a promise):

1. the relay holds **no** HomeGuardian certificate, CA, device key, registry or replay state
   (file-level assertion on the VPS);
2. `8443` is not reachable from the public internet (scan from outside: only 51820/22 answer);
3. a node cannot reach another node's `/32`, nor the relay's own services;
4. a spoofed device identity is still refused (`identity_mismatch`) over the relay;
5. the mutual-TLS session is end-to-end: the relay's interface counters show forwarded bytes but
   the relay can produce no plaintext (no key material exists there);
6. replay protection still holds after a relay reboot (the watermark lives on the hub, not the
   relay);
7. traffic stays inside the stated byte budget (measured over a fixed window with the app's own
   counters and the relay's interface counters).

Acceptance procedure (later, gated):

| # | test | evidence |
|---|---|---|
| 1 | hub->relay tunnel established from behind the home NAT | `wg show` on both ends; handshake age |
| 2 | phone on Wi-Fi, over the relay | observations accepted, watermark advances |
| 3 | **phone on mobile data with Wi-Fi OFF** | observations accepted; this is the milestone's real goal |
| 4 | network switch mid-session (Wi-Fi <-> mobile) | sequence resumes, no duplicate processing, no data loss beyond the counted drops |
| 5 | second and third node added | each authenticated separately; per-node watermarks |
| 6 | revoke one node on the hub | refused within `authorization_refresh_ms`, without a restart |
| 7 | public scan of the VPS | only the intended ports answer |
| 8 | relay reboot with the phone connected | re-handshake and resume; no replay admitted |
| 9 | byte budget over a 30-minute window | measured MB vs the accepted budget |
| 10 | full rollback drill (section 13) | local Wi-Fi route restored and proven |

## 13. Rollback strategy

Everything added is additive and reversible, and the existing local path is never touched:

* **Certificates**: re-issue the server certificate without the relay SAN
  (`provision_ca.sh server --server-names "<original list>"`). The CA and device certificates are
  unaffected, so no phone needs re-provisioning.
* **Hub**: `wg-quick down wg0` (and disable the unit) restores the exact previous state; the hub's
  LAN listener on `192.168.1.6:8443` was never changed and remains the local route.
* **Relay**: `wg-quick down wg0`, disable `wg-quick@wg0`, then remove the UDP 51820 rule from both
  firewall layers (VCN list/NSG **and** the instance's own rules) in the same reviewed change that
  added it. Nothing else was installed except `wireguard-tools`.
* **Phone**: disable/remove the VPN tunnel; the app's saved endpoint can be set back to the LAN
  address for local operation.
* **Trigger for rollback**: any acceptance test failing in a way that cannot be explained within the
  session, or any indication that the relay can see plaintext.
* **Not part of rollback**: nothing on the VPS is deleted, and unrelated services are never touched
  by a HomeGuardian rollback.

## 14. Sequence of gated steps (for later; each one needs the owner's explicit approval)

| step | action | gate |
|---|---|---|
| H-04-P1 | owner supplies section 2 inventory + section 3 keep/remove review | **WAITING ON OWNER** |
| H-04-P2 | agree addressing (section 4) and the mobile byte budget (section 10) | owner sign-off |
| H-04-P3 | install/verify WireGuard on the relay (UDP 51820 only) | owner approves the exact commands |
| H-04-P4 | hub outbound tunnel; prove handshake, no phone yet | hub-side only, reversible |
| H-04-P5 | re-issue the server cert with the relay SAN; verify the identity matrix | no device impact |
| H-04-P6 | first phone over the relay on Wi-Fi, then on mobile data | acceptance tests 1-3 |
| H-04-P7 | second/third node, revoke test, rollback drill | acceptance tests 4-6, 10 |

Until P1 is answered, no action is possible or proposed. **H-03 remains the active milestone, and
its physical acquisition and standalone Wi-Fi items stay PENDING until independently demonstrated.**
