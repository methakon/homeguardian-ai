# MCP release evidence checklist — reproducible, with sources and limits

Task HG-NET-MCP-EVIDENCE-HARDEN-27. Every line states **who measured it** and **how to reproduce it**.
"Worker-reported" is marked as such; it is not independent verification.

| # | evidence | value | measured by | reproduce |
|---|---|---|---|---|
| 1 | bind is loopback-only | `127.0.0.1:8790` LISTEN, no `0.0.0.0` | network verification | `ss -ltn \| grep :8790` |
| 2 | live local listener exists | pid 272248 `hg_mcp_server` | network verification | `ps -p 272248 -o pid,comm,args` |
| 3 | LAN isolation | `192.168.1.9:8790` CLOSED | network verification | `verify_lan_exposure.py --host 192.168.1.9 --observation-port 8443 --mcp-port 8790` |
| 4 | gate check 4 (corrected) | **PASS** — the first non-vacuous one: requires a live local listener AND a closed LAN port | network verification | same as 3 |
| 5 | no-token refused | `POST /mcp` -> **401** | network verification | `curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:8790/mcp -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'` |
| 6 | wrong-token refused | `POST /mcp` -> **401** | network verification | as 5 with `-H 'Authorization: Bearer not-a-real-token'` |
| 7 | bad-Origin refused | `POST /mcp` -> **403** | network verification | as 5 with `-H 'Origin: http://evil.example'` |
| 8 | authenticated protocol | `initialize` + `get_alert_summary` OK | **separately reported** (#25 6335206612) | owner-run; not independently reproduced — needs a real token |
| 9 | cross-process MCP store | **14/14** | **separately reported** (receiver verification) | owner-run; not independently reproduced |
| 10 | raw capture + digest | sha256 `07a3fef0ad7980aea3e9f073b9036d690d1f5b3d5750d028795ccffaa1d72571`, gate exit 1 | network verification | `capture_acceptance_evidence.py --host 192.168.1.9 --observation-port 8443 --mcp-port 8790` |

**Reading the verdict:** loopback-only MCP acceptance **PASS** on isolation and authentication
enforcement. Gate exit 1 is correct here — check 1 (receiver LAN-reachable) FAILs and checks 2-3 are
SKIPPED because nothing is published to the LAN. That is **not** a private-LAN acceptance.

**Evidence rules carried into this checklist:** a completed connection is not authentication (rows 5-7
assert a *refusal*, measured by the endpoint's HTTP status); a skipped check is not a pass (row 4's PASS
requires a live local listener); and a count is read from the instrument that states it, never
re-derived (the capture tool now reads the gate's own NOTE line).

## Two material findings from the bounded static config review

Both are outside this review's implementation scope and are reported to the responsible workstreams.

**F1 — a container runs with `network_mode: host`.** `hg_e2e_ingress` has
`HostConfig.NetworkMode=host`, so it shares the host's network namespace: any listener it opens is on
the **host's interfaces**, including the LAN, and it bypasses Docker port publishing entirely — so
`ports={}` is not reassuring for this container. Precisely: this review observed **no
unexpected LAN listener** (only nginx `0.0.0.0:80`), so this is a **latent structural risk, not an
observed leak**. The exposure would appear the moment that container binds anything.

**F2 — certificate, device-key and secret directories are mounted WRITABLE.** Every one of these is
`ro=false`, so a compromised container can overwrite or plant credentials:

```
hg-s3-ingress-1   /home/…/hg_h03/deploy-runtime       -> /etc/homeguardian/certs        WRITABLE
hg-s3-ingress-1   /home/…/hg_h03/device-keys          -> /etc/homeguardian/device-keys  WRITABLE
hg-s3-hub-1       /home/…/hermes-v380-09/…/creds.json -> /run/secrets/hg_creds          WRITABLE
docker-ingress-1  /home/…/hg_h03/deploy-runtime       -> /etc/homeguardian/certs        WRITABLE
hg_e2e_ingress    ~/.hermes/cache/scratch/hg_e2e/tls  -> /pki                             WRITABLE
```

Read-only DB volumes are correct (`ro=true` on the data volumes), and no container is `privileged`.

**Response:** reported to #25 (receiver lane) and #26 (integration), no container mutated, no restart,
no firewall change, no LAN opened, no key handled.
