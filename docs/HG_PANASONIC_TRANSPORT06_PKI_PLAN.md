# HG-PANASONIC-SERVICE-TRANSPORT-06 — Stage 5: mTLS provisioning plan

**Documentation only.** No certificate, key or passphrase was generated, copied, installed or
committed by this milestone. Provisioning requires explicit owner approval and a named custodian.

## 1. The identity model being reused (do not invent a second one)

Integration contract §2.1: a node's identity IS its device certificate, issued by HomeGuardian's
private CA. Identity is derived from the **authenticated peer**, never from a payload field.

Two bindings must BOTH hold (`net/TlsTransport.h`): the certificate chains to the private CA, **and**
its fingerprint is the one bound to that device in the registry. Neither alone is sufficient — the
pair is what stops one phone acting as another.

The node verifies the HUB in the other direction by **SAN** match against the configured endpoint,
with **no CN fallback**.

## 2. What the app expects, and where it must live

The app looks in exactly one place (`NodeSession`), all app-private (`filesDir`), never public
storage, never logged, never transmitted:

| file | purpose |
|---|---|
| `device.p12` | node certificate + private key (PKCS#12) |
| `device.p12.pass` | passphrase for the above |
| `ca.p12` | private CA bundle used as the trust store |
| `ca.p12.pass` | passphrase for the above |

The one-time import path is `Android/data/org.homeguardian.harness/files/hg_provisioning/`
(`NodeSession.IMPORT_DIR`); the "Import credentials" button copies the four files into private
storage and deletes the source copies. Files larger than 64 KiB are refused rather than copied.

**Currently absent on the device**, so the transport correctly reports BLOCKED rather than connecting
unverified. That is the designed behaviour, not a defect to work around.

## 3. Custody rules (proposed, for owner decision)

1. **The CA private key never leaves its custodian**, is never placed on either handset, and is never
   in the repository. Only the CA *certificate* (trust anchor) ships.
2. **The node's private key is generated where it will live** if that is practical, so it is never
   transported. If generation off-device is unavoidable, the transfer is out-of-band and the file is
   deleted from the source.
3. **Passphrases are supplied separately** from the `.p12` files, never in the same directory, never
   in a commit, never in a chat message, never in a log.
4. **The harness package is not debuggable** (`run-as` refuses, verified), so app-private storage is
   not reachable over ADB. Keep it that way — `android:debuggable` must not be enabled to make
   provisioning easier, because that would expose the credential store to anything holding ADB.
5. **One certificate per device.** Never share a node certificate between the Panasonic and the Mi A3;
   sharing collapses two identities into one and destroys the registry binding.

## 4. Endpoint and SAN requirements

The node dials `host:port` and requires the hub certificate to carry a **SAN** entry matching that
endpoint exactly — DNS name (SAN type 2) for a name, IP address (SAN type 7) for a literal. There is
no CN fallback and no flag that disables either check.

Consequence for provisioning: **whatever the operator types into the hub-host field must appear in
the hub certificate's SAN.** If the endpoint is a bare LAN IP, the certificate needs an IP SAN for
that exact address. Getting this wrong fails the handshake closed, which is the intended direction of
failure but a confusing one to debug if the requirement is not known up front.

## 5. Revocation

- Revoking a node's certificate must terminate that node's session without touching others
  (contract §2.1). The transport already carries `device_revoked` and `device_unknown` as permanent
  refusals, and holds its queue rather than retrying through them.
- A revoked node must NOT be able to re-establish by reconnect. Bounded reconnect is fine; reconnect
  that ignores a permanent refusal is not.
- The hub's registry is the revocation authority. Revocation is a hub-side act; the node cannot
  un-revoke itself.

## 6. Separation from MCP authorization (Stage 2, item 9)

Transport authorization and MCP authorization are **separate decisions** and must stay separate:
a node admitted to the observation transport is not thereby authorized to call MCP tools, and the
MCP bearer-token path does not admit a device to the media/observation transport. Conflating them
would make one compromise grant the other.

## 7. Approval required before anything is generated

1. Who is the CA custodian, and where does the CA private key live?
2. Who generates the node keypair, and where?
3. How are the four files and the passphrases transferred, out-of-band?
4. What endpoint and SAN will the hub certificate carry (this fixes what the operator may type)?
5. Revocation authority and procedure.

**Until all five are answered, this plan stays a plan.** No credential material has been created by
this milestone, and none should be until the owner appoints the custodian.
