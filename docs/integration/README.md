# Integration package v1.0 — HomeGuardian hub and a remote sensor producer

**Contract version 1.0.** This is the minimal package a producer implementation needs. It is deliberately
small: one schema file, one handoff document, and the exact commands that prove a build is compatible.

| what | where |
|---|---|
| message schema | `docs/integration/observation.schema.json` (checked against the encoder by a test, so it cannot drift silently) |
| full contract, auth, clock, replay, codes, deps | `docs/NETWORK_CONTRACT_V1_HANDOFF.md` |
| producer implementation (reference) | `src/net/ObservationProducer.{h,cpp}` |
| hub-side ingest | `src/net/ObservationIngest.*`, `DeviceRegistry.*`, `ReplayState.*`, `TlsTransport.*` |

## Network boundary

- The hub **listens** (mTLS, client certificate required, private CA); a producer connects out. No inbound
  port is needed on the producer.
- Every payload is a bounded JSON message in a 4-byte big-endian length frame, capped at **4096 bytes**.
- **No raw media crosses this boundary in either direction, and the MCP API is not a media relay.** Frames
  and audio samples are reduced to bounded scalars *on the producer*, and only the scalars are sent.
- The hub's MCP API serves status and alerts; it does not carry observations or media.

## Source identity

The certificate is the identity. The payload's `device_id` is a claim the hub checks against the identity
the TLS layer authenticated and against the registry's registered `credential_id`. An unenrolled, revoked
or mis-credentialed peer is refused. One identity is one `sequence` stream: a second writer on the same
identity is indistinguishable from a replay and is refused as one.

## Clock

`acquired_at_ms` is milliseconds since the epoch from a synchronised clock. The hub refuses a timestamp
more than the future-skew allowance ahead, older than 24 h, or behind the device's own out-of-order window.
No clock is negotiated; a drifting producer turns its own evidence into refusals.

## Refusal codes

Wire: `bad_frame`, `bad_version`, `bad_json`, `oversized`, `unknown_kind`, `unknown_source_type`,
`unknown_observation_type`, `invalid_value`, `malformed_timestamp`, `stale_timestamp`, `replayed_sequence`,
`duplicate_observation`, `device_unknown`, `unauthorized`, `consent_missing`, `consent_withdrawn`,
`capture_disabled`, `processing_failed`.
In-process (normalized contract): `not_an_event`, `unsupported_schema`, `malformed`, `source_identity`,
`provenance_invalid`, `provenance_conflict`, `future_timestamp`, `stale_or_replay`, `duplicate`,
`consent_missing`, `source_offline`.

## Integration test command

From the repository root, these are the checks a compatible build must pass — the same commands CI runs:

```sh
cmake -S . -B build -DHOMEGUARDIAN_BUILD_TESTS=ON
cmake --build build -j"$(nproc)" --target homeguardian_tests

# the three network/contract suites a producer integration must not break
./build/homeguardian_tests '[net]'                        # protocol, registry, replay, ingest, detectors
./build/homeguardian_tests '[producer]'                   # the reference producer
./build/homeguardian_tests '[reliability]'                # duplicates, reordering, reconnect, revocation …

# the whole suite, as CI runs it
ctest --test-dir build --output-on-failure
```

Build dependencies: see `docs/NETWORK_CONTRACT_V1_HANDOFF.md` §8 (OpenSSL for the socket layer only;
nlohmann_json; spdlog; Threads; CMake ≥ 3.16; C++20).

## What is not claimed

No physical Mi A3, Alexa, CCTV or V380 integration has been performed, and nothing here asserts that the
phone runs native Linux. The producer and the hub are exercised against each other with synthetic inputs
and a documented wire contract; the physical camera scenarios remain unrun pending an operator.
