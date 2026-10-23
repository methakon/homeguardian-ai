# Backup portal — run, verify, expose and roll back

The backup operator portal is the existing native demo (`hg_alexa_bridge`) extended with uploaded-media
support: the same loopback HTTP server, the same dashboard source (`src/web/index.html`, embedded into the
binary at build time), and no Node, framework or new service.

**Status: not deployed anywhere.** Everything here has been exercised only on loopback from this
workstation. No tunnel exists, no DNS record was created, no systemd unit was installed, and the portal has
never received a request from outside the machine.

## 1. Prerequisites

| need | why | how |
|---|---|---|
| a built binary | the portal is the binary | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target hg_alexa_bridge -j4` |
| an application config | the portal drives the real C++ application | `config/homeguardian.demo.json` for a demo run |
| a credential table | read-only routes (`/api/status`) verify a token through the existing contract | a JSON map of token → `{principal, allow_status, allow_alert_summary, expires_at_ms, revoked}` |
| an **operator secret** file | every upload and delete needs a single-use CSRF session minted from it | a file containing the secret, mode 0600, outside the repository |
| a writable upload directory | uploaded media lives OUTSIDE any web root | defaults to `$XDG_STATE_HOME/homeguardian/portal-uploads`, then `$HOME/.local/state/...`; override with `--uploads-dir` |
| for anything beyond this host: a TLS front | the server is loopback-only by construction and must stay that way | the existing Cloudflare Tunnel pattern — **but see the Origin coupling in §7 first** |

`hg_alexa_bridge --http` prints its full option list; `--uploads-dir` is the only portal-specific one.

## 2. Start and stop

```bash
# start (foreground, loopback only)
./build/hg_alexa_bridge --http config/homeguardian.demo.json <creds.json> \
    --port 8823 \
    --secret-file /path/to/operator-secret \
    --uploads-dir  /path/to/uploads
# it announces readiness as: DEMO_READY 8823
```

```bash
# stop: send SIGTERM/SIGINT to that PID (the accept loop notices within one tick and exits cleanly)
kill <pid>
```

Under a supervisor, run exactly the command above. If you use systemd, do not add a public bind, a
different user's credentials, or extra capabilities: the unit is a plain loopback service and needs none.
Never pass the operator secret on the command line — use `--secret-file` (the binary itself warns about
this for `--token`).

## 3. Verify

```bash
# 1. the committed smoke test: starts its own portal, drives the whole surface, exits non-zero on failure
python3 scripts/portal_smoke.py

# 2. by hand
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8823/api/health   # 200
curl -sS http://127.0.0.1:8823/api/feed | head -c 200                       # label + live_camera:false
curl -sS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8823/            # 200 (the dashboard)

# 3. what a browser gets: the panel, its label, and syntactically valid JavaScript
curl -sS http://127.0.0.1:8823/ | grep -c 'Uploaded media — simulated camera feed'
```

The smoke covers: the served dashboard and its label; session minting with and without the operator
secret (200/401); upload and delete without CSRF (403); a valid PNG upload, its served-back bytes, and a
valid MP4 whose duration is read from the container; the feed listing both with a label and
`live_camera:false`; a script renamed `.png` (415); a non-allowlisted content type (415); an image over
the ceiling (413); path traversal and an unissued id (404); a malformed delete id (400); delete with CSRF
(200) and the media then gone (404); and a portal whose upload store is unusable still serving an empty,
labelled feed instead of erroring or inventing content.

**The oversized-upload defect is fixed.** Until recently an image above roughly 200 KiB was refused with
`400 malformed` where `413` was expected. The cause was in the transport, not the portal: its
`Content-Length` parser capped the value at **6 digits** (`value.size() > 6`), i.e. a maximum declared
length of 999,999 bytes. That guard was written when the only bodies anyone sent were `{}`, and it was
correct then; a route allowed to accept 25 MiB made a 7-digit length legal and the parser refused it
before the route's own ceiling was ever consulted. The bound is now 8 digits (99,999,999 bytes) and the
authoritative limit is still `body_cap`, so an image over 8 MiB now returns `413 too_large` as designed.
The smoke test's oversized-format check passes, and the size sweep is: 999,967 · 7-digit · 1 MiB · 4 MiB ·
7 MiB → all accepted; 9 MiB → `413` from the store's ceiling.

## 3a. Measured resource use

Taken on this workstation with the portal serving loopback traffic (120 uploads of 256 KiB, plus feed and
status polls interleaved):

| measure | value |
|---|---|
| RSS before / after the run | 6,648 KiB → 7,520 KiB (delta 872 KiB, bounded) |
| CPU per upload | ~1.8 ms (0.21 s of CPU for 120 uploads) |
| uploads accepted | 120/120 |

The wall time for that run is dominated by the transport's CSRF issuance limit (5 sessions per 10-second
window), because each upload needs a fresh single-use token — so the CPU figure is the meaningful one, and
the rate limit is a property of the transport rather than of the portal.

## 4. Logs

The process logs to stderr (and to journald under systemd): the resolved uploads directory at startup,
`DEMO_READY <port>` when listening, and — with `HG_DEBUG_BODY=1` — one line per request body stating sizes
only, never content. Request bodies, credentials and the operator secret are never logged.

## 5. Upload limits and validation (what will be refused)

| limit | value |
|---|---|
| image size | 8 MiB |
| video size | 25 MiB (also the transport's hard ceiling) |
| video duration | 30 s, read from the container; a video whose duration cannot be read is **refused** |
| formats | PNG, JPEG, WebP, MP4, WebM — extension, declared MIME type and file signature must all agree |
| filenames | generated server-side (`u_YYYYMMDD_HHMMSS_<8 hex>`); a client name is never used as a path |
| rate | at most 5 CSRF sessions per 10-second window, each single-use |

## 6. Rollback

```bash
kill <pid>                       # or: systemctl stop <unit>
```

That is the whole rollback: the portal holds no external state. Uploaded media is a directory you chose
(`--uploads-dir`) — delete it to discard uploads, or leave it and restart to keep them; the index is
`index.jsonl` inside that directory and is rebuilt from it on load. Nothing else on the host is touched:
no firewall rule, no DNS record, no mail or SSH configuration, no container.

## 7. Security posture, and the one integration that is NOT yet proven

* **Loopback only.** The transport binds `127.0.0.1` and refuses a non-loopback `Host` (DNS-rebinding
  defence) and a non-loopback `Origin`. It is explicitly not a public server.
* **Mutations are CSRF-gated.** Upload and delete need a single-use token minted from the operator secret,
  bound to an authorized session, with a bounded lifetime, an issuance rate limit and lockout after
  repeated failures.
* **Nothing is served from disk except an id-resolved upload.** The dashboard is embedded in the binary; a
  media id is resolved through the store's index, so no path is ever assembled from caller input. Responses
  carry `nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` and a same-origin CORP.
* **The private MCP surface is not exposed** by this portal.

**Unproven integration — read before exposing this with a tunnel.** The transport's `Origin` check requires
the request to come from *this* loopback host and port, which is a strong defence while the portal is used
locally, but a reverse proxy or tunnel presents a *different* scheme/host and will be refused. Putting a
tunnel in front of the portal therefore needs that check widened deliberately — for instance by accepting
a configured external origin while keeping the loopback bind and the CSRF session requirement — and that
change has not been made or tested here. Do not assume an HTTPS front will work until it is.

Other unverified items: the login/authorization experience is the operator-secret panel only (no per-user
accounts); and the dashboard's behaviour when the *application* is degraded (as opposed to the upload store
being unusable) is covered only by reading its JavaScript, not exercised.
