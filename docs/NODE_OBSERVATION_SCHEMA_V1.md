# Node observation schema contract — V1

**Version:** 1.0.0
**Status:** ACTIVE — the version both sides must implement
**Owners:** producer side (Panasonic Android service, Hermes 2) · consumer side (`PanasonicNodeReceiver` +
`ObservationIngest`, Hermes 3)
**Change policy:** additive by default; any incompatible change bumps the major version and is listed here
before either side ships it. This file is the single source of truth for the line shape — a change made only in
code is a defect, and a change made only here is a wish.

> Machine-side counterpart: `kNodeSchemaVersion = 1` in `src/net/PanasonicNodeReceiver.h`. If you change one
> without the other, the drift guard in `tests/net/test_panasonic_node_receiver.cpp` fails.

## 1. Transport shape

- One **JSON object per line** (NDJSON), UTF-8, `\n`-terminated. A line is bounded by the transport's frame
  bound (`kMaxFrameBytes` = 4096); a longer line is refused as `oversized` before parsing.
- The line carries **observations only**. No frame, audio sample, transcript or biometric template may appear
  in any field, at any version.
- Transport is the existing mutual-TLS session (X-01 per-device certificates, private CA). This contract adds
  no credential authority and no listener.

## 2. Fields

| Field | Type | Required | Bounds | Notes |
|---|---|---|---|---|
| `source_type` | string | **yes** | ≤ 64 chars, allowlisted | must be one of §3.1 |
| `observation_type` | string | **yes** | ≤ 64 chars, allowlisted | must be one of §3.2 |
| `value` | number | **yes** | finite | the measurement; `NaN`/`inf` is refused, never defaulted |
| `unit` | string | no | ≤ 64 chars | empty means unitless |
| `state` | string | no | ≤ 64 chars | **no hub-side field yet** — see §6.1 |
| `confidence` | number | no | finite, 0.0 if absent | |
| `acquired_at_ms` | integer | no | epoch ms | absent ⇒ the hub stamps its own clock; skew is validated either way |
| `provenance` | string | no | `real`/`real_device`/`synthetic` | absent is **NOT** real — see §4 |
| `schema_version` | integer | no | = 1 | absent means legacy v0; any other value is refused |

Unknown extra keys are ignored rather than rejected, so a producer may add a field ahead of a version bump
without breaking a V1 consumer — but it must not *change* the meaning of an existing one.

## 3. Closed vocabularies

Enforced by the hub (`ObservationIngest::Config`). The receiver validates `source_type` with the hub's own
decoder and leaves the rest to the ingest, so there is exactly one list per vocabulary.

### 3.1 `source_type`
`motion`, `contact`, `presence`, `camera_availability`, `microphone_availability`, `device_availability`,
`tamper`, `environmental_threshold`

### 3.2 `observation_type`
`motion-changed-fraction`, `audio-rms`, `audio-peak`, `audio-zero-crossing-rate`, `activity-level`,
`availability`, `contact-state`, `temperature`, `humidity`, `pressure`, `illuminance`, `tamper-state`

Note the spelling: the types are **hyphenated** (`motion-changed-fraction`), not dotted or underscored. A
producer that invents a spelling is refused as `unknown_observation_type`, which is a refusal and not a
silent drop.

## 4. Provenance semantics — the session is the authority

The hub records the provenance **its own session model** supports. A producer's `provenance` field is a
**claim**, and the rules are one-directional:

1. **A claim may never widen.** `real`/`real_device` on a session the hub knows to be synthetic is **refused**
   (`provenance_claim_widens_session`) rather than quietly downgraded, because a producer claiming physical
   provenance for loopback input is a defect worth surfacing.
2. **Absence is never real.** An absent `provenance` on a synthetic session stays synthetic. See §6.2: the
   producer's current default of `"real_device"` on an absent field is a defect on the producer side.
3. **An unknown claim string is refused** (`unknown_provenance_claim`).
4. **A claim may not deflate the hub either** (measured): a `synthetic` claim on a session the hub regards as
   real is still recorded as real. The receiver reports the *recorded* value in its result detail, so a caller
   can never mistake a claim for a fact.
5. **An absent claim is ALWAYS sent as synthetic** (agreed with the producer side, and stricter than "the
   session decides"): absence must never travel as a real-evidence claim in the message, whatever the session
   is. Consequence worth stating plainly, because the current producer omits the field on the default path: the
   *recorded* provenance still follows the hub's session model, so physical evidence is not lost — but the
   claim carried in the message is synthetic and may be flagged unsubstantiated by the hub. Producers should
   send an explicit claim rather than relying on this default.

## 5. Refusal vocabulary

Refusals are named, never booleans, and the words are the hub's own (`reject_reason_to_string`): `bad_frame`,
`bad_version`, `bad_json`, `oversized`, `unknown_kind`, `unknown_source_type`, `unknown_observation_type`,
`not_paired`, `consent_missing`, `consent_withdrawn`, `capture_disabled`, `credential_mismatch`, `replay`,
`stale`, `future_skew`, `out_of_order`, `queue_full`, and the receiver's own additions
`malformed_payload`, `unsupported_schema_version`, `provenance_claim_widens_session`,
`unknown_provenance_claim`, and `missing_schema_version` (only when strict mode is enabled — see §6.3).

A refusal is **not** a negative observation: a node that is absent, refused or disconnected produces **no
observation at all**. Nothing in this contract may be used to turn missing data into "no activity".

## 6. Open items (deliberately not settled here)

### 6.3 `require_schema_version` (strict mode) — agreed, needs a flip condition
The receiver carries a `require_schema_version` flag, **default false**, so today a line without
`schema_version` is accepted *and counted* (legacy v0). With it true, an absent version is refused as
`missing_schema_version`. The flag exists so enforcement can tighten without breaking the running producer, but
the flip condition must be explicit and bilateral: it turns on when the producer actually sends
`schema_version: 1` on every line, and it is recorded here when it does.

### 6.1 `state` has no hub-side field
The producer sends it; the hub's wire type has no counterpart. Options: add a bounded string field to the wire
type, carry it as a bounded feature, or drop it. Until one is chosen the receiver **counts** it and reports the
gap, rather than inventing a mapping — a fabricated number is worse than a missing one.

### 6.2 Producer-side changes requested (V1-compatible, no version bump needed)
1. **Send `schema_version: 1`.** Today none is sent; the receiver accepts that as legacy v0 *and counts it*, so
   the omission is visible without breaking the stream.
2. **Stop defaulting `provenance` to `"real_device"`.** Omit the field, or send an explicit claim.
3. **Use allowlisted type pairs** from §3.

## 7. Versioning rules

| Change | Action |
|---|---|
| New optional field, ignore-able by V1 consumers | additive; no bump — documented here |
| New allowed `observation_type`/`source_type` value | additive on the hub side; producers must not rely on a consumer accepting it before the hub list carries it |
| Meaning change to an existing field, or a new required field | **major bump**: `kNodeSchemaVersion`, this document's header, and the drift guard, in one change |
| Retiring a version | announced here; the previous version stays accepted until the header says otherwise |

## 8. Verification

```sh
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[panasonic]'   # 136 assertions / 8 cases
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[net]'         # the ingest's own suite
```

The suite includes a **drift guard**: it asserts the hub's allowlists and the receiver's declared schema
version against the values written in this document, so a change on either side fails loudly instead of
silently skewing the contract.
