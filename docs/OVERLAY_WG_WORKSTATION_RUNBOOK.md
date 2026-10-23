# HG-WG-LIVE-PREFLIGHT-13 — operator runbook (privileged steps are the owner's, not the agent's)

Status: **BLOCKED at the privilege boundary.** Read-only reconciliation is complete; the live leg needs
an interactive privileged operator. This agent has no passwordless `sudo` and must not create one.

## Reconciliation result (read-only, verified)

| item | result |
|---|---|
| existing authorised peer key | **present** — `~/.wireguard/hg-net-02a/devpeer.key` (0600) |
| peer public key vs server peer `.2` | **MATCH** — `+8jilIO/W4JAVDY45EOEWiaaKoslngHMGcuAOHNuiCQ=` |
| peer config | `~/.wireguard/hg-net-02a/wg0.conf` (0600): Address `192.168.77.2/32`, MTU 1420, Endpoint `161.33.146.238:51820`, AllowedIPs `192.168.77.1/32`, Keepalive 25 |
| least privilege already | yes — overlay-only, no `0.0.0.0/0`, no `DNS =`, no PostUp/PostDown |
| `wireguard-tools` | **not installed**; available as `1.0.20210914-1ubuntu4` |
| keys tracked in the branch | 0 |

Verified by deriving the public key from the private key in memory (`x25519`). **No private key was
printed, copied, committed or transmitted.**

## Step 1 — install the tooling (interactive, owner-controlled)

```
sudo -S -p '' apt-get update
sudo -S -p '' apt-get install --no-install-recommends wireguard-tools
```

No `sudoers` change, no passwordless grant, no new group. Revert with:

```
sudo -S -p '' apt-get remove --purge wireguard-tools && sudo -S -p '' apt-get autoremove
```

## Step 2 — prove the preflight is clean

```
bash scripts/overlay/wg_preflight.sh preflight          # must print RESULT: preflight clean, exit 0
```

## Step 3 — ONE bounded live leg (privileged), then reverse it

```
sudo -S -p '' bash scripts/overlay/wg_preflight.sh up   --execute     # arms the dead-man, then brings wg0 up
sudo -S -p '' bash scripts/overlay/wg_preflight.sh verify             # least privilege + handshake
sudo -S -p '' bash scripts/overlay/wg_preflight.sh down --execute     # tears down + proves restoration
```

Evidence to capture: `wg show wg0 latest-handshakes` **with a non-zero timestamp** (a UDP `nc` probe is
**not** handshake evidence), the `verify` output, the `down` restoration proof, and UTC timestamps.

The runbook refuses to create an interface it cannot restore, refuses to delete an interface it did not
create, arms a dead-man teardown **before** creating anything, and never touches the default route,
`resolv.conf`, NAT or `ip_forward`.

## STOP condition

If the operator privilege or the peer config is unavailable, **stop here and report BLOCKED**. Do not
infer live success from code-level tests, and do not fabricate keys or alter the VPS.
