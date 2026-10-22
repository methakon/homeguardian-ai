# Private VPN and remote MCP readiness

## Execution update — 2026-10-23, 03:25 IST

The owner approved applying the prepared setup. The earlier read-only baseline
below is retained as history, not the current state of the first three blockers.

- **Phone enrolled and connected:** the supplied public key now has a dedicated
  /32 assignment, persisted on the VPS while preserving both existing peers.
  The phone profile was configured through WireGuard's UI without exporting or
  replacing its private key. A fresh server-side authenticated handshake, a live
  phone tunnel address and increasing transfer counters were observed.
- **Connectivity verified after handshake:** the phone sent two pings to the VPS
  tunnel address and received both replies, with zero packet loss.
- **Cloud ingress corrected narrowly:** one UDP 51820 rule was added for the current
  verified public /32 in the target's dedicated NSG; existing rules were preserved.
  The observed phone handshake endpoint confirmed this source also covers the
  phone's present egress. This is not a roaming-source or source-any allowance.
- **Coordinator deployed as a service:** a checksum-verified isolated Node.js
  v24.22.1 runtime and integrity-verified `ws` 8.22.0 were deployed without system
  package upgrades. `homeguardian-coordinator.service` is active and enabled,
  requires WireGuard, binds only the VPS tunnel address and has resource limits.
  At the checkpoint it had zero restarts and approximately 28 MB cgroup memory.
- **Application authentication remains enforced:** missing and incorrect client
  tokens returned HTTP 401. A legitimate deployment-readiness request returned
  HTTP 503 `EDGE_OFFLINE`, not a fabricated successful phone response. Eleven
  focused coordinator readiness checks passed; the broader native integration
  harness did not initialize its reused host C++ test artifact and is not reported
  as passing.
- **Exact private ingress restrictions installed:** a dedicated INPUT chain allows
  the phone /32 only into edge port 18994 and the separately reserved worker /32
  only into client port 18995, dropping other VPN sources to those ports. The worker
  address is not yet enrolled or activated. A validated firewall snapshot was copied
  off-host before changes; a timed rollback was armed, then cancelled only after
  verification. The resulting rules were persisted and read back.
- **Public and retained-service controls:** external TCP probes to 8790, 18994 and
  18995 did not connect. The existing public SMTP banner remained reachable, both
  retained mail daemons stayed active with unchanged process ids, and the original
  WireGuard peers were preserved. No DNS or camera/media changes were made.

**Remaining physical MCP bridge gate:** inspection of the installed phone APK
confirmed that it contains neither the outbound bridge class nor its start action.
The owner explicitly approved a same-signature in-place release update preserving
app data and MCP credentials. That update is being prepared in an isolated source
worktree; it has not yet been installed or connected at this checkpoint. Phone MCP
continues serving authenticated read-only requests locally. Remote phone MCP is
therefore **NOT YET VERIFIED**, despite the now-working VPN and deployed coordinator.

The phone's existing native libraries will be preserved byte-for-byte rather than
rebuilt during severe workstation disk pressure. Bridge enrollment credentials must
remain separate from the existing native MCP bearer; the latter must not be exported.
Real reboot/VPN restore and roaming-source recovery have not been tested.

## Earlier read-only baseline

Checkpoint: 2026-10-23, approximately 02:51–02:54 IST.

Verdict: **EXISTING VPN SERVER VERIFIED; PHONE TUNNEL AND REMOTE MCP BLOCKED**.
This was a read-only live-host assessment. Nothing below constitutes a deployment,
a fresh handshake, or permission to change cloud/firewall/app configuration.

## Verified facts

| Check | Observed result |
|---|---|
| Target identity | The owner-specified VPS matched both the OCI instance/VNIC inventory and its SSH hostname |
| Existing WireGuard | `wg0` is up with an assigned private address; UDP 51820 listens; `wg-quick@wg0` is active and enabled |
| Existing peers | Two distinct peer public keys and /32 assignments exist; neither matches the supplied phone public key |
| Handshakes | One peer last handshook on 2026-10-18 at 01:23:40 IST; the other has no recorded handshake. Neither proves a current phone connection |
| Phone profiles | WireGuard is installed; three profiles are present and all were off. The supplied public key matches one existing profile, whose detail view contains no address or peer configuration |
| Phone network | Wi-Fi connected, no active VPN network or assigned tunnel address observed |
| Worker network | No WireGuard interface or NetworkManager VPN/WireGuard profile observed; the `wg` utility is absent |
| Cloud UDP ingress | The attached NSG admits UDP 51820 from one existing public /32 only; the attached security list has no alternative allowance. The workstation's measured public IPv4 differs, so this worker cannot currently handshake through that rule |
| Endpoint DNS | The reference hostname resolves to Cloudflare proxy addresses, not the VPS. Do not assume ordinary WireGuard UDP is proxied by that hostname |
| Phone MCP | Live listener remains `127.0.0.1:8790`; authenticated on-device status probe succeeds with `Running`, `read_only=true`, `capture_enabled=false` |
| VPS MCP/coordinator | No TCP 8790, 18994 or 18995 listeners observed; the coordinator systemd unit is not installed |
| Existing services | Phone EdgeService remained running; retained VPS mail services were active. Neither was restarted |

Actual endpoint addresses and public keys are kept in the private preparation
bundle and the owner's session report, not this public document. No private key,
production bearer token or phone credential file was read or exported.

## Minimum connection to prepare, not a deployed configuration

1. Preserve the current WireGuard interface, private key and existing two peers.
   Add distinct phone and worker peers with conflict-checked /32 assignments. The
   supplied phone key identifies an existing local keypair, not an enrolled VPS peer.
2. Provision a separate worker keypair securely. Do not reuse the phone key or
   repurpose an unrelated existing peer. No keypair was generated in this assessment.
3. Correct the cloud UDP source allowance only with explicit approval. Verify phone
   and worker public egress independently; a roaming-source policy needs a separate
   decision. A DNS-only UDP endpoint is possible, but the literal verified VPS IP
   avoids changing DNS. No TCP application port needs public ingress.
4. Use split-tunnel routes only to the VPS WireGuard address for the recommended
   coordinator shape. No internet default route, DNS override, NAT or unrestricted
   peer-to-peer forwarding is necessary.
5. Require a fresh, matching WireGuard handshake and increasing authenticated
   transfer counters before any connectivity or MCP-over-VPN acceptance test.
   This gate is **NOT MET**; no ping or application request was offered as a substitute.

## Preserve loopback: outbound authenticated control bridge

Do not rebind the phone's MCP to `0.0.0.0`, publish TCP 8790, use DNAT to expose
loopback, or assume the phone's future VPN IP reaches a loopback socket.

The existing repository contains a narrower arrangement:

    phone foreground bridge -> private VPN -> coordinator edge listener :18994
    authorized worker       -> private VPN -> coordinator client listener :18995/mcp
    phone bridge            -> authenticated local MCP at 127.0.0.1:8790/mcp

The phone's own MCP bearer remains app-private. The bridge needs separately
provisioned device authorization; the remote client needs a separate coordinator
credential. Restrict operations to initialization, notification, ping, tool
listing, `get_homeguardian_status` and `get_alert_summary`. Never bridge media,
camera controls, arbitrary destinations or mutating tools.

Candidate source: `deploy/coordinator/service.js` and `web_mcp/outbound_bridge/`.
This source is **not** an observed deployment. The installed phone service reports
version 1.0.0, while current repository source is newer; source presence does not
prove that the installed APK supports the bridge. Do not replace the preserved
phone service or extract its credentials to bypass this compatibility gate.

The deployment package's example/default overlay differs from the live overlay.
Use verified live addressing, not copied defaults. Its permitted-subnet option
uses string-prefix matching, not an exact /32 authorization rule. Therefore exact
interface/source/destination/port restrictions must also be enforced:

- bind coordinator listeners only to the VPS WireGuard address;
- allow only the phone /32 into the edge listener and the worker /32 into client ingress;
- reject other VPN peers to those ports, not just public sources;
- apply these as VPS **INPUT** restrictions, because the coordinator is local to
  the VPS; do not substitute FORWARD rules;
- leave public TCP 8790, 18994 and 18995 closed; preserve mail and SSH restrictions;
- do not enable an unreviewed IPv6 path.

A live deployment still needs staged module-path/ownership/auth/resource review,
a validated firewall snapshot and rollback, secure enrollment, and explicit owner
approval. No installer, NSG change or service restart was executed in this assessment.

## Phone boot-service clarification

MCP already runs in an Android foreground service, independent of its activity.
The installed package registers `BOOT_COMPLETED` and has the corresponding
permission granted. Source starts it after the first unlock; it is not direct-boot
aware because credentials and audit state use credential-encrypted storage.
The service and authenticated health probe were observed live. Automatic startup
following a real reboot was **not re-tested**. No phone reboot or permission change
was performed.

## Remaining acceptance gates

- Approve and apply exact peer enrollment and the cloud UDP source policy.
- Securely provision worker, bridge and client credentials without exporting the
  existing phone MCP bearer.
- Verify a compatible phone bridge and stage the coordinator before deployment.
- Observe fresh phone and worker handshakes, then test VPN-only route confinement.
- Exercise authenticated remote MCP calls and missing/wrong-token, mutating-tool,
  other-peer and public-network rejection controls.
- Verify fail-closed tunnel loss and screen-off/reboot recovery separately.

Remote MCP access remains **BLOCKED**, not complete. The earlier physical
sensor → alert → MCP gate in
[the P1 acceptance report](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md) is also separate
and remains open. The objective here is a private control plane; no camera media
was streamed through Oracle.
