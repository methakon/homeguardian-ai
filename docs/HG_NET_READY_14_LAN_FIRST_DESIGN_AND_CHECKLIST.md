# HG-NET-READY-14 — LAN-first camera path, least privilege, operator checklist

Design and evidence only. **No Oracle/VPS/NSG/router/firewall change, no tunnel started, no key
printed, no privileged package installed.**

## 1. Audit findings: overlay runbook + script

| area | finding | least-privilege alternative | observable success criterion |
|---|---|---|---|
| single-source NSG | ingress admits only `106.219.74.225/32`, so a roaming handset is dropped upstream of the host | keep the pin for the workstation test; for handsets admit the **overlay subnet only** after the tunnel exists, never a bare `0.0.0.0/0` on 51820 | `wg show wg0 latest-handshakes` non-zero from the intended peer, with the counter on the pinned source unchanged |
| DNS absence | `homeguardian.berhampore.in` is absent from the authoritative zone | clients dial the **literal address** `161.33.146.238:51820`; a DNS record is a convenience, not a prerequisite | handshake completes with `Endpoint` set to the literal IP |
| handset endpoint | peer `.3` has never recorded an endpoint, so the phone is not dialling | apply the Mi A3 config with `AllowedIPs 192.168.77.0/24` (must cover the cameras, not just the VPS `/32`) and `PersistentKeepalive 25` | a non-zero handshake for `.3` |
| rollback | `down` deleted `wg0` unconditionally; a `[BLOCKED]` label did not always block | ownership marker, baseline refusal, verified dead-man cancellation, label matches effect | `down` proves routes/NAT/`ip_forward`/`resolv.conf` equal the pre-test snapshot |

Exact permissions: host = the ability to create/delete **one** interface and one route, nothing more;
OCI = **no** change for the workstation test. No `sudoers` edit is proposed.

## 2. LAN-first camera-to-hub path (design)

Camera → hub over the private LAN (never through Oracle for local observations). Identity is the
device certificate **fingerprint**, never CN. Refusals use the vocabulary observed in
`src/net/DeviceProtocol.cpp` @ `02ab07c` and `ObservationTransportPolicyTest.java` @ `cfb96ef`:
`accepted`, `device_revoked`, `device_unpaired`, `device_unknown`, `identity_mismatch`,
`consent_missing`, `consent_withdrawn`. **No invented response codes.**

Dependency checklist (owner / not-owner):
- **Android:** present the client certificate; surface `consent_missing`/`consent_withdrawn`; keep MCP loopback-only.
- **C++ receiver:** enforce device identity on ingress; emit the same tokens.
- **Network:** the hub's receiver port must be reachable on the LAN while the MCP listener stays bound to loopback — proven by `verify_lan_exposure.py` (4/4).
- **Not owned by network:** the consent UI and any physical activation.
- **Gap:** the harness cannot currently *refuse* with `consent_missing`/`consent_withdrawn` — reported as a GAP, not a pass.

## 3. Operator live test checklist (BLOCKED dependencies noted)

```
[blocked] sudo apt-get install --no-install-recommends wireguard-tools
[blocked] an interactive privileged operator (no passwordless sudo on this host)
[ready]   bash scripts/overlay/wg_preflight.sh preflight        -> RESULT: preflight clean
[ready]   sudo bash scripts/overlay/wg_preflight.sh up   --execute
[ready]   sudo bash scripts/overlay/wg_preflight.sh verify
[ready]   sudo bash scripts/overlay/wg_preflight.sh down --execute
evidence  non-zero latest-handshakes timestamp + bidirectional probe + the down restoration proof
```
A UDP `nc` probe is **not** handshake evidence. `test_wg_preflight_mocked.py` is simulated with mocked
tools and is not a substitute for the live leg.
