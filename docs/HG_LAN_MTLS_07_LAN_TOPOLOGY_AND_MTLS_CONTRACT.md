# HG-LAN-MTLS-07 — trusted-LAN topology and device mTLS contract

Dev-only design + validation artifacts. **No handset, router, Oracle or production identity was
modified for this work.** Everything below is either read-only observation or an isolated test.

Related: issue #14 (this mission) and issue #12 (Panasonic integration).

---

## A. Root cause of the asymmetric connectivity

Measured read-only over ADB on both handsets. Three independent results, all pointing the same way:

| Observation | Result |
|---|---|
| Wi-Fi networks in use | **two different SSIDs, two different subnets** — one device on `192.168.0.0/24`, the other on `192.168.1.0/24` |
| Route to the *other* phone | each routes it **via its own gateway**, not on-link → different L2 segments |
| ICMP A → B | 3/3 replies, **278–460 ms** (the "unusually high latency") |
| ICMP B → A | **3/3 sent, 0 received — 100% loss** |
| ICMP to the *other* subnet's gateway | A → `192.168.1.1` works (18 ms); B → `192.168.0.1` **100% loss** |

**Conclusion:** the two handsets are on different subnets with a NAT boundary between them; the
subnet holding the Mi A3 is reachable *from* the other side only outbound. Traffic leaving the
NATed side works (and pays the extra hop, hence the latency); traffic *entering* it cannot, which is
exactly the reported asymmetry.

Ruled out, with evidence rather than assumption:

- **client/guest isolation** — would break *both* directions, and does not explain one phone reaching
  the other subnet's gateway while the reverse fails;
- **VPN routing** — neither device has any VPN network; both report `NOT_VPN`;
- **private DNS** — disabled/unspecified on both;
- **cellular fallback** — neither has a cellular interface holding an address; both are on validated
  Wi-Fi transport.

**Second, independent blocker (application layer).** The Mi A3's edge service is running but binds
**`127.0.0.1` only** — there is no LAN listener at all. So even after the two devices share a subnet,
the initiator still could not connect. The two faults are separate and both must be fixed.

**Pre-existing observation, reported not changed:** one handset listens on **TCP 5555 on all
interfaces** (the ADB-over-TCP convention) — a LAN-reachable debug listener that predates this work.

---

## B. Trusted-LAN solution

Requirements: authorised private LAN; no Oracle involvement for local observations; no router port
forwarding; no internet-facing listener; the **initiator is the other handset**; the Mi A3's MCP
interface stays loopback-only; the observation receiver is separate from the MCP listener.

| Option | How | Verdict |
|---|---|---|
| 1 — shared SSID | put both handsets on the same authorised network | simplest; needs owner authorisation to change a handset's Wi-Fi |
| **2 — local hotspot** | the Mi A3 hosts a private hotspot; the other handset joins it | **recommended**: satisfies every requirement with no router change and no exposure — a strictly local L2, nothing reachable from the internet |
| 3 — Wi-Fi Direct | P2P group, no infrastructure AP | viable but needs app-level group negotiation |

**Recommended: option 2**, with option 1 as the fallback if the owner prefers to keep both devices on
the household network. In both cases the mandatory change is the same and belongs to the Android
service owner:

> the observation receiver must bind the **private-LAN interface** on its own port, while the MCP
> server **continues to bind `127.0.0.1` only**. One process reachable from the LAN, one not.

Validation procedure (after the change, each step independent):

1. initiator → receiver ICMP: expect replies both directions (proves a common L2 — step 1 alone
   proves nothing about the application).
2. initiator → receiver **TCP connect** on the observation port: expect accept (proves a LAN listener).
3. TLS handshake **with a valid client certificate**: expect success **and an application-level ACK**.
4. TLS handshake **without a client certificate**: expect refusal.
5. Confirm the MCP listener is still **unreachable** from the other handset (loopback-only preserved).

Rollback: stop the hotspot / rejoin the previous network and revert the service's bind address; no
router, DNS, Oracle or certificate state is involved, so rollback is confined to the two handsets.

---

## C. mTLS contract (aligned to the existing X-01 / X-02 model, not a new invention)

The project already implements this model (`tools/provision_ca.sh`, `src/net/TlsTransport.*`,
`src/net/DeviceRegistry.*`, `src/net/DeviceIngressServer.*`). The contract below restates it for the
device-to-device LAN path.

1. **CA model** — one **offline private CA**, 4096-bit RSA, `basicConstraints=critical,CA:TRUE`,
   `keyUsage=critical,keyCertSign,cRLSign`. Signs only; never runs on a device; its key is the only
   thing that can mint an identity.
2. **Device certificate generation and ownership** — **one certificate per device**, Common Name **is**
   the device id, `extendedKeyUsage=clientAuth`. Issued by the offline CA; the device's private key
   never leaves the device; the CA key never leaves the offline store.
3. **Required SAN identities** — the receiving side presents a `serverAuth` certificate whose **SAN
   contains the address the initiator dials** (IP SAN for the LAN address). **SANs are the only
   identity source: a CN is never substituted** (the shipped code already refuses CN fallback). The
   peer's identity comes from its certificate, never from message content.
4. **Expiry and rotation** — device certificates short-lived (e.g. 90 days); the server certificate
   no longer than the device certificates. Rotation provisions a *new* certificate while the old one
   is still valid (overlap), then removes the superseded `credential_id` from the registry. Device
   identity is re-issued, not re-registered.
5. **Mutual authentication** — both sides present certificates. The receiver requires a client
   certificate (`CERT_REQUIRED` / `VERIFY_FAIL_IF_NO_PEER_CERT`), verification depth 2, verify result
   checked explicitly, **TLS 1.2 minimum**, no custom cryptography.
6. **Revocation** — a `revoked` flag plus removal of the credential binding, **re-read per connection**
   (already the shipped behaviour), each refusal carrying a stable reason. No CRL/OCSP dependency for
   the LAN path.
7. **Private-key storage on both phones** — app-private storage, `0600` inside a `0700` tree; PKCS#12
   for the Android keystore path already used by the shipped transport. Never in a payload, never in a
   log, never world-readable.
8. **Fail-closed behaviour** — a missing, invalid, expired, revoked or mismatched credential is a
   refusal: no fallback, no plaintext, and no way to disable verification. **Additionally, a completed
   handshake must not be treated as authenticated** (see D).
9. **Private provisioning and deletion** — generate on the offline CA host, transfer privately, install
   into app storage, verify the fingerprint against the registry, then delete the transfer copy.
   Deletion: revoke in the registry, remove the credential binding, wipe the device key.
10. **Test identities kept strictly separate** — a dedicated **DEV CA** with a different subject,
    different files, never installed on a device and never referenced by a production registry. The
    fixture generator in `scripts/mtls_fixtures/` is dev-only and writes outside the repository.

---

## D. Finding: a completed handshake is not authentication (TLS 1.3)

The first revision of the validation harness reported a **false PASS on three security-critical
cases** (no client certificate, untrusted CA, expired certificate). The cause was in the harness, and
it is a trap the real device code can fall into:

> Under **TLS 1.3** the client completes its handshake *before* the server has validated the client
> certificate. A client that sends and does not read observes no error; the rejection surfaces only
> on the first application-level read.

Measured directly, and both versions fail closed once an application ACK is required:

| Case | TLS 1.2 | TLS 1.3 |
|---|---|---|
| no client certificate | `SSLV3_ALERT_HANDSHAKE_FAILURE` | `TLSV13_ALERT_CERTIFICATE_REQUIRED` |
| with application ACK required | refused | refused |

**Contract consequence:** the receiving side must gate on the first *authenticated application
frame*, not on handshake completion, and the initiator must not report success until it has received
an application-level response. This is why "the tunnel is up" is never evidence of anything.

---

## E. Test results

`scripts/mtls_fixtures/generate_dev_fixtures.sh` builds a dev-only CA plus a second *untrusted* CA and
the negative fixtures (no-SAN server, wrong-SAN server, untrusted-CA client, expired client) with
`credential_id` bindings taken from real certificate fingerprints.

`scripts/mtls_fixtures/test_mtls_validation.py` runs 14 cases against the shipped rules —
allow cases, five distinct refusing cases, each client-side negative exercised under **both TLS 1.2
and TLS 1.3**, plus the two registry-level cases (revoked device, credential mismatch):

    PASS: all 14 cases match the contract (fail-closed verified)
    harness exit code: 0

Reproduce: `bash scripts/mtls_fixtures/generate_dev_fixtures.sh && python3 scripts/mtls_fixtures/test_mtls_validation.py`

`scripts/mtls_fixtures/verify_lan_exposure.py` turns the procedure above into an **executable gate**.
Dialling the receiver on its **LAN** address it asserts: (1) the receiver is reachable there;
(2) a handshake **without** a client certificate is refused; (3) a valid client certificate completes
**and receives an application ACK**; (4) the MCP port is **not** reachable on the LAN address. When no
LAN listener exists, checks 2–3 report **SKIP, never PASS** — a refused connection is not evidence of
fail-closed behaviour, and counting it as one would be a pass for the wrong reason.

`--self-test` proves the gate itself: it builds mock receivers locally and confirms the gate
classifies a compliant layout and three non-compliant ones (receiver accepting any certificate, MCP
bound to all interfaces, receiver bound to loopback) correctly.

---

## F. Scope boundaries

No handset network configuration, router setting, Oracle DNS/NSG/firewall/forwarding/mail/certificate
or WireGuard peer was changed. No live device credential was generated or installed. The Mi A3 VPN
handshake problem is tracked separately from this LAN work and is unaffected by it. Successful ICMP is
**not** treated as evidence of authenticated application transport, and enrolment is **not** treated as
readiness.
