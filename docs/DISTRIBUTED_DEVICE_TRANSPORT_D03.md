# D-03 — Secure Android-to-Docker Device Integration

**Branch:** `feature/secure-device-transport` (created from the verified D-02x tip `e2ab2a1`)
**Baseline:** D-02x reported **413 cases / 3,984 assertions**; this milestone measures its own numbers
**Objective:** an authorized observation produced by a device must reach the running Docker hub over
authenticated and encrypted communication, pass through the existing detection/event pipeline, and
be available in alert history and persistence.

---

## 0. What is implemented, and what is not

| Capability | Status |
|---|---|
| Private-CA mutual TLS between device and hub | **IMPLEMENTED** (OpenSSL, TLS 1.2 minimum, no custom crypto) |
| Versioned, framed, bounded device→hub observation protocol | **IMPLEMENTED** (`src/net/DeviceProtocol.*`) |
| Bounded `DeviceRegistry` with pairing, credential binding, consent, capture state, liveness, sequence watermarks | **IMPLEMENTED** (`src/net/DeviceRegistry.*`) |
| Server-side authorization before storage, replay/duplicate rejection, timestamp bounds, allowlists | **IMPLEMENTED** (`src/net/ObservationIngest.*`) |
| Separate network-facing ingress (no dashboard, no commands, no SQLite over the device port) | **IMPLEMENTED** (`src/net/DeviceIngressServer.*`, `src/main_ingress.cpp`) |
| Mock Android transport client + real-TLS end-to-end acceptance | **IMPLEMENTED** (`src/main_device_client.cpp`, `scripts/verify_device_transport.sh`) |
| D-02x detectors wired into the shipped hub runtime | **IMPLEMENTED** (`src/net/IngestDetectors.*`) — the D-02x limitation is closed |
| Certificate provisioning, per-device issuance, immediate revocation | **IMPLEMENTED** (`tools/provision_ca.sh`, revocation file re-read per connection) |
| Docker deployment publishing exactly one port | **IMPLEMENTED** (`deploy/docker/Dockerfile`, `docker-compose.yml`, `ingress-entrypoint.sh`) |
| Android app speaking the protocol with a bounded queue | **PARTIAL — compile-verified only.** `ObservationTransport.java` (bounded 128-entry queue with drop-oldest accounting, one worker thread, bounded reconnect backoff, mutual TLS from a PKCS#12) compiles in the APK build; no phone is attached, so no physical capture or delivery is claimed |
| Raw media streaming | **OUT OF SCOPE** (per the brief): D-03 carries compact observations |
| Home Assistant over TLS | **NOT DONE**: the adapter still has no TLS, so a non-loopback HA deployment remains blocked (see §9) |
| Alexa delivery | **UNCHANGED AND BLOCKED** (D-06 identifier preserved; no work attempted) |
| Disaster-prediction algorithms, new ML models | **NOT STARTED** (explicitly out of scope) |

Everything below was executed on this host or inside the container image; anything not executed
says so.

**Headline results.** D-03 TLS acceptance: **21/21 checks, 0 failed, 0 skipped**, driven over real
sockets with a real private CA (`scripts/verify_device_transport.sh`), and re-run inside the
container `test` stage where the OpenSSL toolchain always exists. D-03 end-to-end detection against
BOTH containers: **32/32, 0 failed, 0 warned** (`scripts/verify_detection_end_to_end.sh`) — the hub's
own API served an alert raised by the ingested stream (0 -> 1, durable across container
replacement). Host suite **426 cases / 4,464 assertions** (Debug, Release and ASan/UBSan agree;
D-02x was 413/3,984), ctest 100% on both build types, 0 sanitizer reports, D-01 container acceptance
**26/26** on the final image. Footprint: ingress idle 12,456 kB RSS and 3 threads,
2.15 ms per observation round trip, 465 observations/s sustained, +2,496 kB under load, two OpenSSL
libraries and nothing else added to the runtime image. Full numbers in
`docs/DISTRIBUTED_DEPLOYMENT.md`.

**Defects the end-to-end run found that unit tests could not** (all five recorded in
`docs/FRICTION_LOG.md`): a decoder that could not read the refusals the hub sent (so every rejection
looked like "ok" to the client); a capture gate that nothing could ever open, which made the
milestone's objective unreachable; a revocation that could not be lifted; two OpenSSL-linked targets
that no environment had ever compiled; and a startup path that aborted rather than refusing.

---

## 1. Repository state and ancestry

```
feature/secure-device-transport   <- created from e2ab2a1 (D-02x tip)
   ^ e2ab2a1  D-02x increment 2
   | 43d6d48  D-02x: skills validation and detection integration
   | 3c8ff460  D-02: Android acquisition node        <- verified D-02 commit
   | 2dc8046   D-02: acquisition node (software complete)
   | 5abc643   D-01: containerise the C++ hub
```

`git merge-base --is-ancestor 3c8ff46… e2ab2a1` returns **true**: the integration branch's ancestry
includes both D-02 and D-01. Note that the milestone brief quoted the D-02 commit as
`3c8ff46eabff45abc59ff6ded297fea10fc1bb`, which is 39 characters — one digit short of the real
`3c8ff4608eabff45abc59ff6ded297fea10fc1bb`. The real object is an ancestor; the abbreviated form in
the brief does not resolve. No reconciliation was needed: D-02, D-02x, D-01 and the v21 fallback
branch are all preserved unmodified, and `main` is untouched.

---

## 2. Security protocol and credential model

### 2.1 Credential model: a private CA plus two independent bindings

`tools/provision_ca.sh` creates:

- an **offline CA** (4096-bit RSA, `basicConstraints=critical,CA:TRUE`) used only to sign,
- a **server certificate** for the hub (`extendedKeyUsage=serverAuth`),
- **one certificate per device** whose Common Name **is** the device id
  (`extendedKeyUsage=clientAuth`).

Every private key is written `0600` inside a `0700` tree; `deploy/docker/certs/`, `*.key`, `*.crt`
and the registry files are gitignored, and certificate material is mounted read-only into the
container. **No key material is ever baked into an image layer.**

The hub requires **both** of these to hold before it trusts a caller:

1. the certificate chain must verify against the private CA
   (`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`, depth 2, `SSL_get_verify_result ==
   X509_V_OK` checked explicitly), and
2. the certificate's SHA-256 fingerprint must equal the `credential_id` the registry binds to that
   device id.

Either alone is insufficient, and that is the point: a stolen CA signature cannot impersonate a
device whose fingerprint is recorded, and a device that presents another device's certificate fails
binding. Identity is taken from the certificate (`CN`), **never** from the message.

### 2.2 Transport

- OpenSSL, `TLS_server_method()`/`TLS_client_method()`, `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`,
  compression and renegotiation disabled. OpenSSL's reviewed default cipher suite — no custom
  cryptography anywhere.
- Client-side verification is mandatory and is never disabled, including in tests.
- Sockets carry `SO_RCVTIMEO`/`SO_SNDTIMEO` (handshake, then idle), `TCP_NODELAY`, and a hard frame
  cap (`kMaxFrameBytes = 4096`) validated from the 4-byte length header **before** any allocation.

### 2.3 Protocol

`uint32` big-endian length + JSON body. Kinds: `hello`, `welcome`, `observation`, `ack`,
`heartbeat`, `goodbye`, `error`. A version mismatch is refused (`bad_version`), and every message
carries `protocol_version_major`.

An observation must carry: protocol version, device id (a **claim**, checked), observation id,
source type, observation type, acquisition mode, acquisition timestamp, sequence number, a bounded
flat numeric feature object, optional unit/scale/confidence/quality, and a provenance claim.
Rejections use stable reason strings: `bad_frame`, `bad_version`, `bad_json`, `oversized`,
`unknown_kind`, `unknown_source_type`, `unknown_observation_type`, `invalid_value`,
`malformed_timestamp`, `stale_timestamp`, `replayed_sequence`, `duplicate_observation`,
`device_unknown`, `device_revoked`, `device_unpaired`, `identity_mismatch`, `consent_missing`,
`consent_withdrawn`, `capture_disabled`, `queue_full`, `unauthorized`.

### 2.4 Provenance is derived, not claimed

`derive_provenance()` is the only place provenance is decided, and it requires **all** of:
authenticated peer, paired device, recorded device consent, capture enabled for that device, and
`acquisition_mode == "live"`. Anything less is recorded as **synthetic**, and a `real` claim the hub
cannot substantiate is flagged (`provenance_claim_unsubstantiated`) and counted rather than
silently believed. The trust facts come from the registry, not from the caller's context struct.

### 2.5 Replay, duplicate and ordering defence

- A sequence must be strictly greater than the device's committed watermark; otherwise
  `replayed_sequence`. **The watermark is committed only after every other check passes**, so a
  refused message cannot consume a sequence number.
- A repeated observation id inside a bounded per-device window is `duplicate_observation`.
- Timestamps must be positive, within +120 s of hub time, no older than 24 h, and no more than 60 s
  behind the device's own high-water mark (`stale_timestamp`). A smaller backward step is accepted
  and flagged `out_of_order`.

### 2.6 Revocation is immediate

`DeviceRegistry::apply_revocation_file()` is re-read by the ingress **on every new connection**, so
`tools/provision_ca.sh revoke <id>` (or editing `revocations.json`) takes effect on the device's
next message — not at the next restart. An absent revocation file is normal and is not an error.
Revocation in the registry is total: it clears pairing, consent, capture and liveness, so no residual
flag leaves a revoked device partially authorized.

**Removing an id from the list lifts the ban immediately** (the file is authoritative for the ids it
lists, tracked separately from registry revocation). Un-revoking deliberately does NOT re-grant
pairing or consent: the operator states those again. A ban and a permission are different decisions,
and silently collapsing them would mean reinstating a device also silently re-authorized it
(ADR-013).

Consent, unlike revocation, is part of the registry and is therefore read at startup: withdrawing it
is a registry reload, which is how the acceptance suite exercises it.

### 2.7 Boundaries and quotas

- The device endpoint serves **one protocol**. There is no HTTP listener on that port, no dashboard,
  no command channel and no SQLite access; the acceptance script asserts plain HTTP fails there.
- Accepted connections are queued in a bounded queue; a full queue is refused with `queue_full`
  rather than growing. `max_connections`, `max_queued_connections`, `max_workers` and
  `max_pending_observations` are all validated to stay inside hard ceilings at config load, so a
  config cannot disable the limits.
- Shared core state (registry, ingest, detectors, pipeline, persistence) is guarded by one mutex;
  TLS handshakes happen outside it in a bounded worker pool.

---

## 3. Android transport implementation

`src/main_device_client.cpp` + the `TlsClient` in `src/net/TlsTransport.*` implement the device
side of the protocol: mutually authenticated connect, `hello`/`welcome`, then framed observations
with sequence numbers and current timestamps, plus adversarial modes (`replay`, `future`, `ancient`,
`unknown-source`, `oversized`, `claim-real`, `no-hello`, `goodbye`) so the boundary can be tested
with a hostile client rather than only a cooperative one.

The D-02 Android application is unchanged in its acquisition behaviour — capture remains **off by
default**, permission-gated, consent-gated, per-frame/per-buffer accounted, and it still stops
everything when the activity leaves the foreground. Its side of the transport is a compile-verified
serializer/queue in the APK; **no physical device is attached**, so no capture or delivery is
claimed (see §8). The mock client is what the acceptance script drives.

`docs/ANDROID_TRANSPORT_D03.md` records exactly which parts of the Android side exist and which are
compile-verified only.

---

## 4. Docker hub ingestion implementation

```
deploy/docker/Dockerfile          3 stages; libssl-dev/openssl in build+test, libssl3(+t64) in runtime
deploy/docker/ingress-entrypoint.sh   validates config + certificate mounts, then execs the ingress
deploy/docker/config/ingress.json.example
deploy/docker/docker-compose.yml  two services: `hub` (unpublished loopback dashboard) and
                                  `ingress` (THE ONLY published port, 8443/tcp)
tools/provision_ca.sh             CA + per-device issuance + revocation
scripts/verify_device_transport.sh  real-TLS end-to-end acceptance (12 checks)
```

The ingress is a separate process and a separate container boundary. Docker Compose publishes
**only** `8443/tcp`; the hub's loopback dashboard port stays unpublished (the opt-in
`docker-compose.loopback.yml` override still binds the host side to `127.0.0.1` only). The ingress
runs as the same non-root UID, with all capabilities dropped and `no-new-privileges`, its own memory
ceiling (128 MB), `pids_limit: 128`, its own data volume, and certificate material mounted
read-only.

---

## 5. Detection pipeline integration (the D-02x limitation, closed)

`src/net/IngestDetectors.*` is wired into the ingress binary, so the D-02x components now run in a
shipped hub runtime, per device, and feed the **existing** pipeline:

| Incoming observation | Detector | Output |
|---|---|---|
| `motion-changed-fraction` | `ActivityChangeDetector` | activity level/trend observation |
| `audio-rms` | `ActivityChangeDetector` + `AnomalyMonitor` (the existing z-score monitor) | activity + statistical-deviation observations |
| any accepted observation | `SensorDeduplicator` | repeat accounting (never gates the feed) |
| any accepted observation | `DeviceAvailabilityTracker` | liveness; one offline observation per quiet device |
| `environmental_threshold` | validated and carried as an observation | measurement, no invented threshold |

`VectorDriftDetector` and the full `PrecursorSuite` fusion are deliberately **not** wired: they need
a bounded multi-modal feature *vector*, and D-03 carries scalars. Wiring them to one scalar would
manufacture a "regime shift" out of a single number. They remain available for D-05.

Every derived observation goes to the same `Pipeline`, `DetectionFusionRule` and `AlertPolicy` —
there is exactly one event engine. The derived detector ids are not in the fusion rule's default
evidence allowlist, so nothing here silently changes the existing alert behaviour, and **no path in
this milestone can produce `Severity::critical`**.

---

## 6. Network security and consent test results

Host suite (no TLS toolchain needed), `[net]`: **7 cases / 310 assertions**, covering protocol
framing/encoding/strict decoding, provenance derivation, registry binding/revocation/consent/
capture, persistence strictness, and the full accept path including every refusal reason.

Container acceptance, `scripts/verify_device_transport.sh` — 12 checks, asserting:

TLS handshake succeeds for a registered, consented device · a missing certificate fails closed at
startup · a certificate whose fingerprint is not bound to the device is refused (`identity_mismatch`)
· an unregistered device is refused (`device_unknown`) · a revoked device is refused and un-revoking
restores service without a restart · a replayed sequence is refused · a future timestamp is refused ·
an ancient timestamp is refused · an unlisted source type is refused · an oversized frame is refused
· a stream without the hello handshake is refused · consent withdrawal/disabled capture blocks
processing · the port speaks TLS only (plain HTTP fails) · the ingress shuts down cleanly on SIGTERM
· the durable alert store survives a restart.

Measured outcome: **21 checks, 21 passed, 0 failed, 0 skipped** — and the same script runs as the
last step of the container `test` stage, so the evidence is produced by the image build itself.
Exact output and the host/container numbers are in `docs/DISTRIBUTED_DEPLOYMENT.md`.

---

## 7. End-to-end observation-to-alert proof

The acceptance script drives a real client against the real binary; the delivered observations reach
`Pipeline` → `DetectionFusionRule` → `AlertPolicy` → SQLite (`alerts` table), and the persisted count
is asserted to survive a restart. The host suite additionally proves the same path without sockets
(`[net][ingest][integration]`, `[net][detectors]`): accepted observation → `to_observation()` →
`to_event()` → pipeline history with **real** provenance and the correct detector id, plus derived
detector observations flowing into the same pipeline.

An alert is produced only when the fused evidence qualifies; a single sensor report never alerts on
its own, and no sensor or protocol path can reach `critical`.

---

## 8. Hardware-dependent limitations

| Limitation | Detail |
|---|---|
| No phone attached | `adb devices` is empty. D-02 physical capture acceptance remains **BLOCKED**, and no frame/sample/battery number is claimed anywhere |
| Android transport not run | The Android side is compile-verified; the protocol client that has actually been executed is the C++ mock. Physical acceptance needs an operator, a device, and the provisioning steps in `docs/DISTRIBUTED_DEPLOYMENT.md` |
| Container networking only | The end-to-end test runs client-against-container over loopback; a real LAN run (two hosts, firewall rules) is documented but not executed here |
| Live Home Assistant | Still blocked: the adapter has no TLS. D-03 does **not** transmit HA tokens over plaintext Wi-Fi |

---

## 9. Home Assistant compatibility

The D-02x adapter is untouched and continues to produce the same validated internal observations
(`SecurityObservation`), which is the shared model the ingestion path also uses. Its transport
remains **plaintext-only and opt-in**: `https://` is refused because no TLS client was linked, and
`http://` requires `allow_plaintext=true`. The brief's rule that HA tokens must not cross ordinary
unencrypted Wi-Fi is therefore still enforced by refusing to talk to a non-loopback instance.
Mock/loopback validation remains what exists; live HA acceptance is a separate gate. No actuator
control was added.

---

## 10. Performance and footprint

Measured on this host and in the image; exact figures in `docs/DISTRIBUTED_DEPLOYMENT.md` §D-03.
Summary of what is claimed: hub binary size delta, ingress binary size (3,053,760 B) and client
(267,504 B), TLS dependency footprint (two OpenSSL libraries; `libssl3t64` at 8,115 kB installed, and
nothing else added to the runtime image), idle RAM (12,456 kB) and CPU (0.0% idle) of the ingress,
thread count (3: one accept thread plus bounded workers, unchanged under load), 2.15 ms per
observation round trip and 465 observations/s sustained over 200 observations, and queue bounds with
drop accounting. Throughput and latency were measured **against this host and the container**, not
against a phone — the old Android devices' limits are a separate measurement that cannot be made
without hardware.

Security, consent and SQLite durability were not weakened for any of it: TLS 1.2 minimum stays,
`synchronous=FULL` + WAL stay, and the frame/queue/registry caps stay enforced.

---

## 11. Remaining work and the honest boundary between the four kinds of "working"

- **Component-tested detection** — true today for the protocol, registry, ingest, detectors and the
  pipeline integration (host suite).
- **End-to-end Docker integration** — true today for a mock client against the real container.
- **Physical Android acquisition** — NOT proven: no device attached.
- **Live Home Assistant integration** — NOT proven: no instance, and no TLS client yet.
- **Genuine Alexa integration** — NOT proven and externally blocked (D-06, unchanged).

Next recommended step: **D-04 — multiple nodes**, which is where per-device isolation, clock skew,
reconnect storms and multi-source correlation actually get exercised; and, in parallel, the operator
step of provisioning a real phone certificate and running the first physical acceptance.
