# Portal deployment (HG-WEB-PORTAL-DEPLOY-49)

Artifacts for running the **existing** Node portal on a small shared VPS without exposing it. Nothing here
has been applied to any host: these are files plus a check-only preflight, a dry-run installer and a rollback.

## What this deploys, and what it deliberately does not

| deploys | does not |
|---|---|
| `web_mcp/server.js` under systemd, bound to **127.0.0.1:8800** (the service binds loopback itself) | open any public port — no `0.0.0.0` bind anywhere in these files |
| the child `hg_mcp_server` it spawns on **127.0.0.1:18990** | publish 18990 or 8790 — both stay private |
| a Cloudflare Tunnel ingress in front, as a template | touch mail (25/465/587/993/995), SSH (22), firewall/NSG, DNS, Docker or credentials |
| a 180 MiB / 25% CPU envelope for a host that also runs OpenSMTPD + Dovecot | create the tunnel, the DNS record or the TLS certificate — those are owner actions |

## Files

| file | purpose |
|---|---|
| `hg-portal.service` | systemd unit: loopback, `MemoryMax=180M`, `CPUQuota=25%`, hardening, `Restart=on-failure` |
| `cloudflared-ingress.yml` | tunnel ingress template → `http://127.0.0.1:8800`, everything else 404 |
| `preflight.sh` | **check-only** report: sources, node, port, mail/SSH ports asserted as untouched, memory, cloudflared |
| `install.sh` | **dry-run by default**; `--apply` (root) installs the unit, enables and starts it, then healthchecks |
| `rollback.sh` | **dry-run by default**; `--apply` stops/disables, restores the previous unit, proves the port is free |
| `healthcheck.sh` | read-only: systemd state, listener, dashboard 200, **unauthenticated API is not 200**, memory |

## Order of operations

```bash
# 1. on the host, with the app tree in place and dependencies installed
npm install --omit=dev
bash deploy/portal/preflight.sh                 # checks only; fix FAILs before going further
# 2. install and start (root)
sudo bash deploy/portal/install.sh --user hgportal --apply
bash deploy/portal/healthcheck.sh               # must pass
# 3. the tunnel (owner): create it once, then run it
cloudflared tunnel create <TUNNEL_NAME>         # writes <uuid>.json — mode 0600, outside this repo
#   copy cloudflared-ingress.yml to /etc/cloudflared/config.yml, substitute the three placeholders,
#   add the DNS record for <PORTAL_HOSTNAME>, then:
sudo cloudflared service install                # or run it under systemd yourself
# 4. verify from outside, then and only then is it "deployed"
curl -sS -o /dev/null -w '%{http_code}\n' https://<PORTAL_HOSTNAME>/
# 5. if anything is wrong:
sudo bash deploy/portal/rollback.sh --apply
```

## Security posture of this deployment

* **No public bind.** The portal already binds `127.0.0.1`; the unit adds no port. HTTPS ingress is the
  tunnel's, over an **outbound** connection — no inbound port is opened and no firewall rule changes.
* **The private MCP endpoint is not published.** The tunnel ingress has exactly one hostname route plus a
  404 fallback; nothing points at 18990 or 8790.
* **Mail and SSH are asserted, never modified.** `preflight.sh` reports their listeners so an operator can
  see they are untouched; no script here writes to mail or SSH configuration.
* **Bounded resources.** 180 MiB / 25% CPU / 64 tasks, chosen so a 954 MiB host keeps > 700 MiB for the OS
  and mail services.
* **No credential in any file here.** The tunnel credentials JSON lives outside the repository at 0600, and
  the portal's own tokens come from its environment.

## Owner prerequisites (cannot be done by an agent)

1. A **Cloudflare** account with the zone, permission to create a tunnel and a DNS record for the hostname.
2. **Host access** (SSH) to run `--apply` and `cloudflared tunnel create` — root or sudo for the unit.
3. A decision on **Cloudflare Access** in front of the hostname, or reliance on the portal's own operator
   login. Access is recommended for anything reachable from the internet.
4. Confirmation that the host is the intended one, and that backing out (step 5 above) is acceptable.

## Status

**Not deployed anywhere.** No host has been touched, no tunnel created, no DNS record added, no unit
installed, and the portal has never received an external request. The local evidence for this artifact set is
that the scripts parse, the preflight runs read-only, and the installer/rollback print their plan without
`--apply`.
