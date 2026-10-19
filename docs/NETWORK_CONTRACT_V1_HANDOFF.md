# Network observation contract v1 — handoff for the Ubuntu ARM64 hub

**Contract version:** wire protocol `1.0` (`kProtocolVersionMajor` = 1, `kProtocolVersionMinor` = 0).
**Status:** implemented and audited, not proposed. The transport, registry, ingest, replay and detector
layers already exist in this repository and are linked into the hub binary. **Nothing in Hermes 2's branch,
worktree or files was touched**, and no merge is proposed: this document is the interface.

The contract has **two layers**, and a producer only has to satisfy the first:

1. **The wire** (`src/net/DeviceProtocol.h`, `TlsTransport`, `DeviceIngressServer`) — framed JSON messages
   over mutually authenticated TLS. This is what a remote producer sends.
2. **The normalized observation** (`docs/SENSOR_EVENT_CONTRACT.md`, `Observation`) — what the engine
   fuses, produced by the hub from the wire message. A producer never writes this.

## 1. Canonical example (an observation message)

```json
{
  "kind": "observation",
  "protocol_version_major": 1,
  "protocol_version_minor": 0,
  "device_id": "hg-aabbccddeeff0011",
  "observation_id": "obs-000123",
  "sequence": 123,
  "source_type": "motion",
  "observation_type": "motion-changed-fraction",
  "acquisition_mode": "live",
  "acquired_at_ms": 1800000000123,
  "features": { "motion-changed-fraction": 0.25 },
  "unit": "fraction",
  "confidence": 0.8,
  "provenance_claim": "real"
}
```

Message kinds: `hello`, `welcome`, `observation`, `acknowledgement`, `heartbeat`, `goodbye`, `error`.
Frames carry a big-endian length header and are capped at **4096 bytes** (`kMaxFrameBytes`).

## 2. Source identity requirements

- Identity is **re-derived from the TLS client certificate**, never read from the payload. `device_id` is a
  *claim*; the hub uses the certificate's identity. A claim of another device's id does not become that
  device (audited: `tests/net/test_network_contract_audit.cpp`).
- The registry holds, per device: `credential_id` (certificate fingerprint/CN), `paired`, `revoked`,
  `capabilities`, `consent_*`, `capture_enabled`. A peer whose credential is not the registered one is
  refused.
- A device must be **paired** in the registry before anything it sends is stored.

## 3. Authentication expectations

- **Mutual TLS with a private CA.** The hub presents a certificate; the producer presents a client
  certificate that chains to the same CA, and the hub verifies it (`require_client_certificate`). Server
  identity is verified by the client and never disabled.
- The hub records the peer's `credential_id` (SHA-256 fingerprint of the peer certificate) and binds it to
  the registered device.
- Revocation: a device revoked in the registry is refused, and **authorization is re-evaluated on every
  observation** — not once per session — so a mid-session withdrawal stops the stream.
- What this is not: the consumer layer performs no authentication of its own. It validates; the transport
  authenticates. Do not put a bearer token on the wire and call it identity.

## 4. Clock synchronization requirements

- `acquired_at_ms` is **milliseconds since the Unix epoch** as an integer, stamped by the producer when the
  sensor observed the value; the hub separately records `ingested_at`.
- The hub refuses a timestamp more than `max_future_skew_ms` in the future (`malformed_timestamp`), and one
  older than `max_age_ms` (**exactly 24 h**, boundary tested) or behind the device's own out-of-order
  window (`stale_timestamp`).
- **Requirements on the producer:** a synchronised clock (NTP or better within the skew allowance), and
  timestamps that are monotonic per device. No clock discipline is negotiated by this protocol; a drifting
  producer degrades its own evidence into refusals.

## 5. Replay protection contract

- Every observation carries a per-device **monotonic `sequence`** and a unique `observation_id`.
- Acceptance is **two-phase and durable**: the hub marks `begin(device, sequence, observation_id)` before
  processing and `complete(...)` after, so a crash cannot reopen the hole.
- On connect, `hello` returns `welcome` carrying **`resume_after_sequence`** — the highest sequence already
  accepted from that device. **A restarted producer resumes above it** rather than restarting its counter.
- Replays are refused with `replayed_sequence` / `duplicate_observation`; an in-flight duplicate is refused
  as `duplicate`. Replays are refused **across reconnects**: durable state, not session state, holds the
  watermark (audited).
- If durable replay state is unusable, the hub **fails closed** rather than continuing with an empty
  watermark.

## 6. Supported event types

`source_type`: `motion`, `contact`, `presence`, `camera_availability`, `microphone_availability`,
`device_availability`, `tamper`, `environmental_threshold`.

`observation_type`: `motion-changed-fraction`, `audio-rms`, `audio-peak`, `audio-zero-crossing-rate`,
`activity-level`, `availability`, `contact-state`, `temperature`, `humidity`, `pressure`, `illuminance`,
`tamper-state`.

Both lists are policy in `ObservationIngest::Config` — an event type outside them is refused with
`unknown_source_type` / `unknown_observation_type`, so extending the vocabulary is a deliberate change on
the hub side, not something a producer can do by sending a new name.

## 7. Error and refusal codes

**Wire refusals** (`RejectReason`, sent back on the wire and stable as strings): `none`, `bad_frame`,
`bad_version`, `bad_json`, `oversized`, `unknown_kind`, `unknown_observation_type`, `unknown_source_type`,
`invalid_value`, `malformed_timestamp`, `stale_timestamp`, `replayed_sequence`, `duplicate_observation`,
`device_unknown`, `unauthorized`, `consent_missing`, `consent_withdrawn`, `capture_disabled`,
`processing_failed` (plus the policy/registry codes in the header). Consent is more specific than
"unauthorized" by design: a withdrawal with a known subject is `consent_withdrawn`, a device with no
consent subject is `consent_missing`.

**In-process refusals** (`IngestCode`, used by `SensorEventConsumer` on the normalized JSON path):
`not_an_event`, `unsupported_schema`, `malformed`, `source_identity`, `provenance_invalid`,
`provenance_conflict`, `future_timestamp`, `stale_or_replay`, `duplicate`, `consent_missing`,
`source_offline`.

## 8. Required build dependencies

| dependency | needed for | note |
|---|---|---|
| OpenSSL (dev headers) | the TLS transport, `hg_device_ingress`, `hg_device_client`, `hg_ingress_probe` | the protocol/registry/ingest/detector layers are OpenSSL-free and build anywhere; only the socket layer needs it |
| nlohmann_json | the message payloads and feature objects | header-only |
| spdlog | logging | bounded, no payload dumped |
| Threads (pthreads) | the bounded worker pool in the ingress server | `max_workers` is capped |
| CMake ≥ 3.16, C++20 | the build | |

## Behaviour the audit verified (Task 1's nine requirements)

`tests/net/test_network_contract_audit.cpp` checks each requirement against the **real** registry, ingest,
replay and detector classes — 9 cases, 149 assertions:

| requirement | verdict |
|---|---|
| authenticated source identity | a peer's claim cannot become another device's identity; an unregistered claim is refused; a wrong credential is refused |
| consent and authorization | the registry is authoritative over the peer's context; withdrawal refused as `consent_withdrawn`; capture disabled refused |
| timestamp validation | future refused; exactly 24 h accepted and one millisecond beyond refused; zero refused; both clocks recorded |
| replay across reconnects | a fresh hub process reading durable state still refuses sequence 5 and the device resumes cleanly at 6 |
| source-offline handling | a silent device yields **one `device_availability` / `"offline"` transition and zero activity observations** — silence is never recorded as "nothing happened" |
| per-observation provenance | an authenticated live observation is recorded real; a `real` claim against a synthetic message is recorded unsubstantiated |
| bounded messages and memory | a 5000-byte feature cannot be framed; the pending queue stays at its bound under a flood and every surplus message is refused **with a code**, never dropped silently |
| explicit refusal codes | the vocabulary round-trips through the wire; an unknown code is not silently accepted |
| no raw media | the wire message has no media field, and a 200 KB blob in a feature cannot be framed |

## What this handoff does not claim

No physical Alexa, CCTV, or V380 integration has been performed or measured here, and none is claimed: the
producer side has been exercised with fixtures and a device client, not with a real third-party device. No
raw video or audio crosses this contract in either direction. Nothing here changes Hermes 2's work, and the
Mi A3 hub needs **no camera or microphone driver of its own** under this architecture — which is the point
of the contract.
