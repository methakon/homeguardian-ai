# Android → C++ D-03 pairing preflight and operator runbook

Status: **PREPARED, NOT EXECUTED.** No credential was issued, no APK installed, no sensor activated,
no Docker or network publication attempted.

Every fact below is read from the implementations — `apk/src/org/homeguardian/harness/` and
`src/net/DeviceProtocol.{h,cpp}` / `DeviceIngressServer.cpp` @ `02ab07c` — not from a scripted peer.

## 1. What matches already (verified, not assumed)

```
framing        4-byte big-endian length prefix + UTF-8, max 4096
               both sides ALSO reject length == 0
version        protocol_version_major/minor = 1/0 on both sides
kinds          hello, welcome, observation, ack, heartbeat, goodbye, error
welcome        resume watermark travels as `last_sequence`
               (`resume_after_sequence` is the C++ STRUCT field name, not the wire name)
ack            observation_id, sequence, accepted, reason
refusals       22 tokens, identical on both sides
```

## 2. What the two sides require of each other — the real constraints

**Android requires of the hub** (`openSocket` → `enforceServerIdentity`):
- a TLS server certificate whose **SAN matches `config.host`**. With `host = 127.0.0.1` the SAN must
  be `IP:127.0.0.1`; with a DNS name it must be a matching **DNS** SAN.
  `endpointIdentificationAlgorithm=HTTPS` is set **before** the handshake, so a CA-signed certificate
  for a **different** host is rejected by the TLS stack rather than trusted because its chain is valid.
- the server chain must verify against the truststore the device holds (`ca.p12`).

**The C++ ingress requires of the device:**
- a client certificate that is **authenticated AND bound to a registry record**:
  `registry_.authenticate(peer.device_id, peer.credential_id)`. A valid certificate that is not in
  `devices.json` is **not** enough — authentication and authorization are separate steps.
- the registry's verdict maps to a wire refusal, so the device sees *why*:

```
ok                  -> none
unknown_device      -> device_unknown
revoked             -> device_revoked
unpaired            -> device_unpaired
credential_mismatch -> identity_mismatch
no_credential       -> unauthorized
```

- the `device_id` **claim** in the hello must equal the identity the hub derived from the certificate,
  or the frame is refused. The claim is never trusted over the certificate.

## 3. The four credential files, and where they go

Android reads them from app-private storage (`NodeSession.buildConfig`):

```
<filesDir>/device.p12        client certificate + key (PKCS12)
<filesDir>/device.p12.pass   its password, separate 0600 file
<filesDir>/ca.p12            the CA to trust (PKCS12)
<filesDir>/ca.p12.pass       its password
```

**They cannot be pushed directly.** The app is **not debuggable**, so `run-as` cannot reach private
storage and there is no root. The supported path is `NodeSession.importCredentials`:

1. the operator places the four files in
   `Android/data/org.homeguardian.harness/files/hg_provisioning/`
2. the app copies them into private storage, marks them readable **owner-only**, then **deletes the
   source files** — on API 27 another app holding `READ_EXTERNAL_STORAGE` could read that external
   directory, so the exposure window is deliberately short
3. the copy is **bounded** (`MAX_CREDENTIAL_BYTES`) and **nothing is logged, returned or displayed**

`credentialsPresent()` returns false until all four are present, and `SessionObservationLink.start()`
refuses to start without them — a missing credential fails closed rather than connecting unauthenticated.

## 4. SAN / CA decisions the owner must make — a checklist, no defaults chosen here

- [ ] **Which host string will the device dial?** This determines the SAN the server certificate needs.
      A literal `127.0.0.1` requires an **IP** SAN; a name requires a **DNS** SAN. Using one and
      issuing the other produces a certificate that fails endpoint identification with a message about
      the SAN, and the device will not connect.
- [ ] **Is the hub's certificate issued for the address the device will actually use?** A certificate
      valid for `hub.internal` does not authenticate to `10.x.y.z` unless that is also a SAN.
- [ ] **Which CA signs the client certificate, and is that CA the one the device trusts?** The device
      trusts only what is in `ca.p12`.
- [ ] **Is the device's certificate recorded in the hub's `devices.json`**, with the correct
      `device_id` and `credential_id`? Without the registry entry the certificate authenticates and the
      device is refused `device_unknown`.
- [ ] **Is the `device_id` ≤96 chars and the display name ≤64?** Now enforced client-side; an
      over-long value stops the transport locally instead of being refused remotely.
- [ ] **Are the phones' clocks within 2 minutes of the hub?** `acquired_at_ms` more than
      `kMaxFutureSkewMs` (120 s) ahead is refused as `malformed_timestamp`, which reads as a transport
      fault. Check with `adb shell date` against the hub before a capture run.
- [ ] **Credential issuance and file placement are owner actions.** They are `live_credential_issuance`
      in this worker's identity and are **not** performed here.

## 5. Non-sensor test-vector path (no camera, no microphone)

A handshake and observation can be exercised **without activating any sensor**, because the producer
takes observations as data:

- The **test-only** TLS material from `scripts/gen_test_tls_material.sh` already drives the real
  transport through hello → welcome → observation → ack over a real socket (19 cases).
- Substituting the **real** hub requires only (a) a reachable address and (b) credentials in the four
  files above. No code change is needed: `host`/`port` are configuration.
- Therefore the first genuine cross-implementation test can use a **synthetic** observation with
  `acquisition_mode: "synthetic"` and `provenance_claim: "synthetic"` — which is the non-claiming value,
  so it cannot be mistaken for physical evidence — and still prove the wire contract end to end.

## 6. Expected behaviour on a real connection

- **Hello → welcome.** The welcome carries `last_sequence`. The device **adopts** it and resumes above
  it. If it did not, a restarted node would begin at 1 and every line would be refused
  `replayed_sequence`. (The wire name is `last_sequence`, not the schema's `resume_after_sequence`.)
- **Observation → ack.** The ack echoes `observation_id` and `sequence` and sets `accepted`. The
  transport treats a non-matching echo as a failure rather than a success.
- **Refusal classes and what the device does:**
  - `queue_full`, `processing_failed`, `duplicate_observation` → **retry**, bounded at 4 attempts.
  - `bad_frame`/`bad_json`/…/`replayed_sequence` → **permanent**: dropped, never retried.
  - `consent_withdrawn`, `device_revoked`, `device_unpaired`, `device_unknown`, `identity_mismatch`,
    `unauthorized`, `capture_disabled`, `consent_missing` → **SUSPEND**: the transport stops and holds
    the queue rather than retrying. This is the difference between "stop and wait for the operator" and
    "keep pushing to a hub that has revoked this device".
  - an **unrecognised** reason → treated as permanent (fail closed).
- **Revocation.** Revoking the device in the registry makes the next exchange return `device_revoked`,
  which suspends the transport. It does **not** silently drop one observation and continue.

## 7. What remains owner-gated

1. **Credential issuance** for the designated device, and the registry entry.
2. **Publishing the ingress on a LAN interface** — a network-boundary change, #23's decision. Today
   `docker port` is empty and `127.0.0.1:8443` is refused, so there is no reachable endpoint.
3. **`apk_install`** if the Mi A3 is the subject; `sensor_activation` for any capture.
4. **The #26 trusted approval gate**, which is unresolved.

Until 1–4 are satisfied, the honest statement is: **the wire contract is verified against the real
receiver by inspection and against a real TLS peer by execution, and no physical device has yet
exchanged a frame with the C++ hub.**
