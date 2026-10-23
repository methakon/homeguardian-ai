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

## 8. Two vector modes, because one timestamp cannot serve both purposes

A pinned timestamp makes bytes reproducible; the ingress **bounds** the timestamp. `kMaxObservationAgeMs`
is 86400000 (24 h), so a fixed instant expires the day after it is chosen. The original pinned value
`1761100000000` = **2025-10-22T02:26:40Z** was exactly a year stale and the live ingress refuses it
`malformed_timestamp / "acquisition timestamp is too old"`. **Deterministic bytes cannot be used
unchanged in a live test**, and pretending otherwise yields either a test that fails forever or one that
gets quietly skipped.

```
pinned   byte-reproducible offline golden. NOT live-valid. sha256 736c4b88... (stable across runs)
live     fresh acquired_at_ms inside the window. sha256 0a1781fa... (differs every run)
         BOTH: acquisition_mode=synthetic, provenance_claim=synthetic, same framing, same vocabulary
```

The manifest records `mode`, `live_valid`, `acquired_at_utc` and the exact `ingress_timestamp_windows`,
so a consumer can tell which artefact it holds without inferring it.

```
-Dhg.vector.mode=pinned   (default)   -Dhg.vector.mode=live
```

`V10` asserts the pinned golden **is** outside the window — so a later change that quietly refreshed it
would break the build rather than silently stop being reproducible.

### The three time rejections, and their two different tokens

```
acquired_at_ms > now + 120s              -> malformed_timestamp  "in the future"
now - acquired_at_ms > 24h               -> malformed_timestamp  "too old"
acquired_at_ms < last_observation - 60s  -> stale_timestamp      "behind the device watermark"
```

The third is a distinct token and is about **ordering within a device**, not age. E1–E4 assert all of
these against the live ingress, including `E1`: the hub refuses the pinned golden's own timestamp.

## 9. Allowlisted vocabulary is NOT detector capability — the distinction that matters

`camera_availability` / `availability` are **wire-valid**: the ingress accepts, records and audits them.
That is all they are. From `main_ingress.cpp`:

> ingested sensor observations are recorded and audited but **cast NO vote** — so they can never raise an
> alert on their own. Listing the ingested ids (e.g. `sensor.motion`) is an explicit operator decision to
> let that evidence participate in fusion.

So:

- **allowlisted source_type** → the frame is accepted. Nothing more is implied.
- **detector-capable** → the id must also appear in `evidence_detector_ids` in the ingress config.

**This vector is wire-valid and detector-INERT by default.** It proves the transport contract; it does
**not** prove that an ingested observation raises an alert. Claiming otherwise would be the same class of
error as presenting synthetic evidence as physical. Producing rule-compliant detector evidence is a
separate artefact and requires the operator listing the id — an operator decision, not a code change.

## 10. Test-container lifecycle

`hg_e2e_ingress_s2` (this lane) on `127.0.0.1:18444` with its own state dir. The shared `hg_e2e_ingress`
is **not touched**.

**Never `rm -rf` a bind-mounted tree, and never restart a container independently of the material it was
started from.** A bind mount holds the **inode**: deleting the host directory leaves the running
container attached to the *unlinked* one, so `/state` reads empty and every connection fails
`certificate verify failed` — which presents as a trust problem and is a filesystem one. The ingress also
loads its CA at startup, so a regenerated CA cannot be verified by a running process.

**Clean start** (the remedy for that symptom):

```
HG_E2E_FRESH=1 bash scripts/run_real_ingress_e2e.sh
```

`FRESH=1` regenerates the material **and recreates this lane's container**; the script refuses
`FRESH=1` against the shared container name rather than recreating a container it does not own.

## 11. What remains owner-gated

1. **Credential issuance** for the designated device, and the registry entry.
2. **Publishing the ingress on a LAN interface** — #23's decision. Today `docker port` is empty and there
   is no address Android can reach.
3. **`apk_install`** if the Mi A3 is the subject; **`sensor_activation`** for any capture.
4. **The #26 trusted approval gate**, which is unresolved.
5. **A detector-capable evidence fixture**, if fusion evidence is wanted (§9).

Until then: **the wire contract is verified against the real receiver, and no physical device has
exchanged a frame with the C++ hub.**
