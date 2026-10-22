# MCP owner-run local endpoint

A read-only MCP endpoint you can start yourself, point at the alert store your application writes, and test
independently — no build agent, no coordinator, no code changes required.

## The one command

```bash
cd <repo>
bash scripts/hg_mcp_owner_endpoint.sh --store /path/to/alerts.db
```

It starts `hg_mcp_server` on **127.0.0.1:8790** (loopback only), prints its startup line, runs a real
`initialize` → `tools/list` → `tools/call` exchange so you can see it working, prints a copy-paste example,
and leaves it running. Stop it with:

```bash
bash scripts/hg_mcp_owner_endpoint.sh --stop
```

## Which store?

`--store` must name **the file the application writes** — the `audit_db_path` in the ingress config. The
endpoint refuses to guess, and refuses to create one.

```bash
# the compose deployment: find it from the running ingress
docker compose -f deploy/docker/docker-compose.yml exec ingress \
  sh -c 'grep audit_db_path /etc/homeguardian/certs/ingress.json'
```

That file lives in the `hg_data` volume, shared by the hub and the ingress on purpose: an alert produced
from an admitted observation is written into the same SQLite file the dashboard reads. The MCP process is a
**reader** of it, never a second writer.

## What "read-only" means here, and why it is enforced twice

| layer | guarantee |
|---|---|
| the volume/mount | the compose service mounts the store `:ro` |
| the connection | `SQLITE_OPEN_READONLY` plus `PRAGMA query_only` — SQLite itself refuses every write on that connection, and a repository built with `ensure_schema=false` cannot even run `CREATE TABLE IF NOT EXISTS` |
| the capability | the tools cannot enable capture, grant consent, change configuration or read media; authorization is the Alexa contract's, not a second opinion |

The credential store is stricter still: it requires a **0600 file owned by the effective uid** and refuses
anything else. That check is a feature — it is why an unprovisioned mount fails loudly instead of serving
unauthenticated. The script creates one at `~/.homeguardian/mcp_credentials.json` (mode 0600) if you do not
have one, and **never prints the token**: the commands it prints read the file themselves.

## The distinction this endpoint exists to preserve

```
store is a valid, empty HomeGuardian store  -> {"alert_count":0,...}   SUCCESS: zero alerts is a fact
store is missing / unreadable / foreign     -> isError "the request could not be authorized"
```

It will never answer "no alerts" for "no storage". If you see the error, point `--store` at the right file;
do not create one to make it quiet.

## Running it as a service instead

`deploy/docker/docker-compose.yml` has an `mcp` service that does the same thing in the deployment topology
(host networking so its loopback bind is the host's loopback; the store mounted `:ro`):

```bash
export HG_MCP_CREDS_FILE=$HOME/.homeguardian/mcp_credentials.json   # 0600, owned by HG_MCP_UID_GID
export HG_MCP_UID_GID=$(id -u):$(id -g)                            # must own that file
docker compose -f deploy/docker/docker-compose.yml up -d mcp
```

Both `HG_MCP_CREDS_FILE` and (when unset) `HG_MCP_ALERTS_DB` are named in the compose file's error text, so a
misconfiguration says what to set rather than half-starting.

## What this does NOT do

No writes to the store, no second writer, no LAN exposure, no capture/consent/configuration control, no raw
media, and no physical-device claim: every observation behind these tools is synthetic and labelled as such.
