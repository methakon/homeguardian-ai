# HG-NET-HUB-ENDPOINT-17 — the real hub endpoint, LAN requirements, secure path

Read-only reconnaissance. **No Oracle/VPS/NSG/router/firewall change, no container modified, no
privileged install, no tunnel, no key printed.**

## 1. The real endpoint: there is none serving

| candidate | what is actually true | verdict |
|---|---|---|
| `192.168.1.6:8443` | host IS on the LAN (ICMP replies, ARP `REACHABLE` `36:bc:d3:fb:81:06`) but **22, 443 and 8443 are all closed** | **not an endpoint** |
| `docker-ingress-1` | `state=running health=healthy` **but `networks={}`, `ports={}`, `ip=` empty** | **not an endpoint — healthy and attached to nothing** |
| this host `192.168.1.9` | no listener on 8443 / 18880 / 18980; the only non-loopback listener is an unexplained **`0.0.0.0:80`** | **not an endpoint** |

The previously recorded claim that the ingress publishes `192.168.1.6:8443->8443` is **not
reproducible**: that address refuses the port, and the container has no network attachment.

**Two correction-worthy findings.** The container reports `healthy` while serving nothing — a
healthcheck that cannot detect an unattached container is a false-healthy signal (the same class as
"process online" ≠ "working"). And `0.0.0.0:80` is LAN-facing and **unattributed**: `ss -ltnp` cannot
name the process without privilege. It contradicts the loopback-only requirement for the demo server
and should be identified by whoever owns it before any LAN path is declared safe.

## 2. LAN connectivity requirements: NOT MET (measured, not asserted)

```
python3 scripts/mtls_fixtures/verify_lan_exposure.py --host 192.168.1.6 --observation-port 8443
python3 scripts/mtls_fixtures/verify_lan_exposure.py --host 192.168.1.9 --observation-port 8443
  FAIL 1 receiver reachable on the LAN address   (ICMP answers, nothing listens - a closed PORT,
                                                  not a network fault)
  INFO 2 fail-closed without a client certificate  (not applicable - no LAN listener to test)
  INFO 3 valid client certificate authenticates    (not applicable - no LAN listener to test)
  PASS 4 MCP port NOT reachable on the LAN address
  INFO 5 ICMP - informational only
  NOTE  2 checks SKIPPED (no LAN listener) - explicitly NOT counted as passing
  FAIL  1 required check did not hold
```

The gate distinguishes `PORT_CLOSED` from `HOST_UNREACHABLE`, which is why check 1 is a **FAIL** and
not a network fault: the link is fine, the service is absent. Required checks 2 and 3 are skipped, and
a skipped check is never a pass.

## 3. The secure path to the C++ receiver (prepared, not applied)

**Bind contract (from the reviewed topology):** the observation receiver binds the **private-LAN
interface** on its own port; the **MCP server continues to bind `127.0.0.1` only**. One process
reachable from the LAN, one not. Recommended observation port: **8443** (the gate's default).

**Identity:** mutual TLS with a client certificate that MUST verify against the dev/production CA;
identity is the certificate **fingerprint**, never CN; a handshake alone is not authentication — the
receiver must return an **application-level ACK**. Refusals use the vocabulary observed in
`DeviceProtocol.cpp @ 02ab07c`: `accepted`, `device_revoked`, `device_unpaired`, `device_unknown`,
`identity_mismatch`, `consent_missing`, `consent_withdrawn`.

**Dependency checklist (owner in brackets):**
- [network] attach the ingress container to a network and publish the port, **or** run the receiver on the host bound to the LAN interface — nothing can be validated until an endpoint exists
- [network] identify and justify `0.0.0.0:80`; it must not be an unauthenticated LAN service
- [C++ receiver] bind the LAN interface on the observation port; keep MCP on loopback; emit the canonical tokens
- [Android] present the client certificate; surface consent refusals
- [owner] decide LAN vs Wi-Fi Direct; physical activation is separately gated

**Acceptance (all four, in order):** 1 TCP connect on the observation port accepted; 2 no client
certificate -> refused; 3 valid client certificate -> success **and application ACK**; 4 MCP port not
reachable from the LAN. Run the gate after each change:
`verify_lan_exposure.py --host <hub> --observation-port 8443`.
