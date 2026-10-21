# LAN acceptance readiness — prepared for independent validation once an endpoint exists

Prepared while **holding** per HG-NET-LAN-VERIFY-18 (no scanning until a real deployment change).
Nothing here changes the network, the container, PKI or Oracle.

## Instrument readiness — proven before it is needed

An instrument first used at the moment of judgement is not trustworthy; a PASS would be a first-ever
run. Both are now pre-validated:

```
python3 scripts/mtls_fixtures/verify_lan_exposure.py --self-test
  PASS: the gate classifies a compliant layout and all three non-compliant ones correctly
  S4 receiver bound to loopback only -> FAIL/SKIP/SKIP/PASS
  MCP reachable on the LAN          -> "REACHABLE - loopback-only violated"
```

So the gate is known to (a) pass a compliant layout, (b) catch a receiver that accepts any client,
(c) catch an MCP listener bound to all interfaces, and (d) treat a loopback-only receiver as
`SKIP` for the gates that cannot apply — **and a `SKIP` is never a pass**.

Evidence capture is also pre-validated against a **negative control**, to prove it cannot launder a
failure into a success:

```
python3 scripts/mtls_fixtures/capture_acceptance_evidence.py --host 192.168.1.9 --observation-port 8443
  gate_exit_code=1   verdict=FAIL   counts PASS=1 FAIL=1 INFO=3 skipped=3
  raw retained + sha256 recorded
```

## Acceptance procedure (run once a working private-LAN endpoint exists)

```
python3 scripts/mtls_fixtures/capture_acceptance_evidence.py --host <private-lan-ip> --observation-port 8443
```

| gate | requirement | failure reads as |
|---|---|---|
| 1 | TCP accept on the **designated private interface** | refused/timeout |
| 2 | no client certificate -> **refused** | handshake succeeded (fail-open) |
| 3 | authorized **fingerprint** mTLS **+ application ACK** | handshake fine, no ACK — under TLS 1.3 the client handshake finishes before the server validates the cert, so a bare handshake proves nothing |
| 4 | **MCP unreachable from the LAN** | reachable = credential-free leak |

**Decision rules.** PASS requires the gate to exit **0** with all four required gates satisfied.
A gate that could not be exercised is `SKIP`/`INFO` and is **not** counted as passing; the raw output
and its `sha256` are the evidence, not a summary. `PORT_CLOSED` is reported distinctly from
`HOST_UNREACHABLE` so "nothing is listening" can never be read as "the network is broken" or as
success.

## Preconditions that remain BLOCKED

- **No endpoint exists.** `docker-ingress-1` is `state=running health=healthy` while `networks={}`,
  `ports={}`, no IP. Attaching it and publishing the port is **#26's ownership**, not mine.
- The identity used for gate 3 is the **dev fixture**; no production credential will be issued
  (PKI issuance is out of scope) — a real gate 3 needs the owner's decision on which device
  certificate is authorised.
- Physical handset activation remains separately gated.

## Boundaries observed during this preparation

Read-only. No Docker attach or port publish, no receiver bind change, no network configuration, no
Oracle/NSG/DNS change, no PKI issuance, no listener started on any LAN interface, no key material
printed. The negative control targeted a closed port on an existing host address; it started no
listener of its own.
