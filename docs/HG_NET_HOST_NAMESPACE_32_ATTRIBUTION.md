# HG-NET-HOST-NAMESPACE-32 — per-container bind attribution without privileged namespace access

Method per the instruction: attribute the **expected** bind from each container's own launch config
(readable via `docker inspect`, no privilege), then compare against the read-only host listener table.
`/proc/<pid>/fd` is root-owned and unreadable to this uid, which is why the namespace view is
unavailable — the config is the way in.

## Per-container evidence

| container | same binary? | bind source | declared bind | host listener | verdict |
|---|---|---|---|---|---|
| `hg_e2e_ingress` | `hg_device_ingress --config /state/ingress.json` | `~/.hermes/cache/scratch/hg_e2e/ingress.json` | **`bind_address = 127.0.0.1`**, port 18443 | none on 18443 | **NOT EXPOSED** — declares loopback; no non-loopback listener |
| `hg_e2e_ingress_s2` | `hg_device_ingress --config /state/ingress.json` | `~/.hermes/cache/scratch/hg_e2e_s2/ingress.json` | **`bind_address = 127.0.0.1`**, port 18444 | none on 18444 | **NOT EXPOSED** |
| `hgvec27` | `hg_device_ingress --config /tmp/hg_vec27/ingress.json` | `/tmp/hg_vec27/ingress.json` | **`bind_address = 127.0.0.1`**, port 18445 | none | **NOT EXPOSED** |

All three are the **same image and binary** (`homeguardian-ai:d032test`,
`/src/build/hg_device_ingress`), each pointed at a config whose `bind_address` is `127.0.0.1`. No
`--bind`/`--host` flag exists in their argv or environment, so the config is the only bind source and
the attribution is complete.

**`network_mode=host` was therefore not evidence of exposure in any of the three cases** — the listen
addresses are loopback by configuration. The remaining risk is structural and conditional: because they
share the host's network namespace, a future config change to `0.0.0.0` would expose the port with no
Docker publish to review, which is why the boundary check keeps them at WARN rather than PASS.

**Host listener table (read-only, compared as instructed):** the only LAN-facing listeners are nginx on
`0.0.0.0:80` and `[::]:80`. No listener exists on 18443, 18444 or 18445 on any interface, consistent with
loopback-declared binds.

**Still UNVERIFIED, and stated as such:** the *runtime* socket attribution. The config says what the
process intends; this review cannot prove from the process's own fds that no listener was opened elsewhere. The
evidence above is config-declared intent plus the host listener table, and it is not dressed up as
socket-level proof.

## Owner-gated minimal remediation plan

1. **Remove the idle stale test containers** — `hgvec27`, `hg_e2e_ingress_s2`, `hg_e2e_ingress`. All
   three are e2e/vector test containers with scratch or `/tmp` state, and each is host-networked. This
   is a change to **separate workstreams**, so it is a request, not an action: no container outside
   this review's scope was stopped or removed.
2. **If any must stay host-networked**, keep its config `bind_address` at `127.0.0.1` and treat a change
   to `0.0.0.0` as a review gate — with host networking there is no `ports=` mapping to catch it.
3. **No privileged escalation is required for any of this**, and none was used.

## Historical-snapshot discipline

The earlier MCP `401 / 401 / 403` + LAN-closed acceptance remains a **snapshot of the endpoint that was
running at that time**; the endpoint process has since ended (foreground, owner-run, ended by the
reboot). It is not restated as current state. A further acceptance needs a fresh owner start.
