# HomeGuardian AI — Privacy, Consent, Retention and Data Lifecycle

This document describes the Phase E data model, consent semantics, retention and
deletion behaviour, schema migration, and the limitations of the current
implementation. It is the authoritative reference for how profile, consent, and
routine data is stored and handled.

## 1. Data model

### FamilyProfile (`src/core/FamilyProfile.h`)

| Field | Type | Notes |
|-------|------|-------|
| profile_id | string | Stable, opaque identifier. Not derived from a name. |
| display_name | string | Human-readable label, max 128 chars. |
| age_band | optional enum | `child`, `teen`, `adult`, `senior`. Genuinely optional. |
| enabled | bool | Profile active flag. |
| created_at / updated_at | timestamp | Milliseconds since epoch. |
| schema_version | uint32 | >= 1. |

**Not collected by default:** birth dates, identity documents, face embeddings,
voiceprints, biometric templates, or any other sensitive attribute. Optional
attributes stay optional; nothing sensitive is added implicitly. A test asserts
that the serialized profile contains none of these fields.

Profiles must not be used to diagnose medical conditions or to infer
personality, dangerousness, intent, trustworthiness, or emotional state from
facial or vocal characteristics. No such inference exists in Phase E.

### ConsentRecord (`src/core/ConsentRecord.h`)

| Field | Type | Notes |
|-------|------|-------|
| consent_id | string | Unique record identifier. |
| subject_scope | string | A profile_id or a household-wide scope label. |
| purpose | string | Processing purpose, e.g. `fall_detection`, `presence`. |
| data_categories | string[] | Authorized categories, e.g. `camera`, `audio`, `sensor`. |
| decision | enum | `granted`, `denied`, `withdrawn`. |
| policy_version | string | Policy/consent version label. |
| recorded_at | timestamp | When the decision was recorded. |
| expires_at | optional timestamp | Optional expiry / review requirement. |
| provenance | string | Opaque origin label. No identifying personal data. |
| schema_version | uint32 | >= 1. |

### Routine (`src/core/Routine.h`)

| Field | Type | Notes |
|-------|------|-------|
| routine_id | string | Unique identifier. |
| profile_id | string | Owning profile. |
| label | string | Human-readable, max 128 chars. |
| schedule | optional string | `daily`, `weekly`, `weekdays`, or `HH:MM` (24h). |
| time_zone | optional string | One of `UTC`, `Asia/Kolkata`, `America/New_York`, `Europe/London`. |
| enabled | bool | Active flag. |
| created_at / updated_at | timestamp | Milliseconds since epoch. |
| schema_version | uint32 | >= 1. |

A routine records **configuration only**. It is never treated as confirmation
that a person completed a meal, medication, or activity. A missing event remains
"not confirmed"; it never becomes a negative finding. A test asserts the
serialized routine carries no completion/confirmation state.

## 2. Consent semantics

- **Default-deny.** When no consent record exists for a subject and purpose,
  processing is not authorized. `ConsentRecord::is_active(now)` returns true only
  when the decision is `granted` and the record has not expired.
- **Explicit states.** `granted`, `denied`, and `withdrawn` are distinct.
  `denied` and `withdrawn` are never active under any circumstance.
- **Expiry.** A `granted` record with `expires_at` in the past is not active.
- **Withdrawal.** A `withdrawn` record is never active, and an authorization gate
  keyed on "any active grant for this purpose" must evaluate to not-authorized
  once the latest decision for the purpose is a withdrawal. This is covered by
  the "withdrawal prevents future authorization" test.
- **Household configuration, not legal proof.** A stored consent record is
  household configuration. It is **not** interpreted as proof of legally valid
  consent. Enforcement is the responsibility of a future media-processing
  component; no such component exists in Phase E.
- **No media, biometrics, or secrets.** Consent and profile records store no raw
  media, face templates, voiceprints, or secrets.

## 3. Persistence and schema migration

- Schema version is tracked via SQLite `PRAGMA user_version`.
- **v1** created the `events` and `alerts` tables (Phase D).
- **v1 → v2** is an **additive** migration that adds `profiles`, `routines`, and
  `consent_records`. Existing `events` and `alerts` tables are untouched, so all
  prior data is preserved.
- The migration runs inside a single transaction. On any error it rolls back and
  rethrows, leaving the schema and `user_version` unchanged. It is safe to rerun
  because it only executes when `user_version == 1`.
- Foreign keys: `routines.profile_id` references `profiles(profile_id)` with
  `ON DELETE CASCADE`. `PRAGMA foreign_keys = ON` is set on every connection.
- Indexes exist on `routines(profile_id)`, `consent_records(subject_scope)`, and
  `consent_records(subject_scope, purpose)`.
- Uniqueness: every table has a TEXT PRIMARY KEY. Extended SQLite result codes
  are enabled so a duplicate PRIMARY KEY / UNIQUE violation is reported as a
  `Duplicate*Exception`, while a foreign-key violation is reported as a
  `DatabaseException`. This distinction is required for correct error handling.

## 4. Retention, export and deletion

- **Configurable retention.** `routine_retention_days` (default 730) and
  `profile_retention_days` (default 1825) are validated configuration fields.
  A value of `0` means "keep until explicit deletion".
- **Profile deletion.** Deleting a profile removes its dependent routines via
  `ON DELETE CASCADE`. Consent records are keyed by `subject_scope` and are not
  automatically deleted by the cascade; they should be reviewed and removed
  explicitly when a subject is removed, per the retention policy.
- **Historical event evidence is not deleted** merely because a profile is
  deleted. Event and alert history is retained under its own event/alert
  retention policy unless that policy explicitly requires removal.
- **Export / inspection.** Profile, consent, and routine records expose
  `to_json()`. Export uses these structures and never includes secrets, media,
  or biometric data. No secret or personal data is written to default
  configuration files.

## 5. Configuration

Phase E configuration fields (`src/core/Config.h`):

| Field | Default | Notes |
|-------|---------|-------|
| media_capture_enabled | false | Camera/audio capture is OFF by default. |
| cloud_processing_enabled | false | External upload / cloud processing is OFF by default. |
| consent_policy_version | "1.0.0" | Policy label applied to new consent records. |
| routine_retention_days | 730 | Validated range 0–36500. |
| profile_retention_days | 1825 | Validated range 0–36500. |

Invalid values are rejected at load/save time. Development/test configuration
is kept separate from the shipped default (`config/homeguardian.json`), which
has capture and cloud processing disabled. A test asserts the shipped default
keeps both disabled.

## 6. Phase F1 — Consent-Gated Simulated Sensing

### Consent gate (`src/core/ConsentGate.{h,cpp}`)

`ConsentGate` is the single decision point consulted before any protected
acquisition or processing. It is deliberately narrow so the consent rule is
defined in exactly one place; components must not re-implement it. Given an
`AcquisitionRequest` (subject_scope, purpose, data_category) it returns a
`GateDecision` and fails closed. Deny reasons, in evaluation order:

1. `capture_disabled` — `media_capture_enabled` is false in configuration.
2. `no_consent_record` — no record exists for subject+purpose.
3. `decision_not_granted` — the latest decision for subject+purpose is denied or
   withdrawn (a later withdrawal/denial overrides any earlier grant).
4. `expired` — the latest decision is a grant whose `expires_at` has passed.
5. `ambiguous` — more than one active grant exists for the same purpose.
6. `category_missing` — the grant does not authorize the requested data category.
7. `none` — authorized.

Missing, ambiguous, expired, denied, and withdrawn authorization all fail closed.
The gate is a pure decision function: it performs no acquisition and no side
effects.

### Gated acquisition (`src/core/ConsentGatedAcquisition.{h,cpp}`)

`ConsentGatedAcquisition` wires the gate to a `SimulatedSensorSource`. On
`acquire()` it consults the gate first; if denied it returns immediately with no
event and the sensor is never read (`emitted_count()` unchanged). If authorized,
it pulls the next synthetic reading. This demonstrates the control flow that a
future real hardware boundary must follow.

### Simulated sensor (`src/core/SimulatedSensorSource.{h,cpp}`)

A deterministic `IEventSource` that emits synthetic observations. Every emitted
event's payload carries `"synthetic": true` and a `sensor_kind`. It supports
configurable observation timestamps, malformed input (array payload rejected by
the `Event` constructor), and simulated disconnect/reconnect (while disconnected,
`next()` yields nothing).

### Threat model (Phase F1)

- **Unauthorized acquisition.** Mitigated by the consent gate failing closed and
  by the gated-acquisition path never reading the sensor on a deny. Tested for
  denied, missing, expired, withdrawn, unknown-profile, and mismatched
  purpose/category.
- **Bypassing the gate.** The gate is the only path to acquisition in this phase.
  A future hardware component must call the gate before opening any device; that
  boundary does not exist yet.
- **Confusing synthetic data for real data.** Every simulated event is tagged
  `synthetic`. Tested.
- **Withdrawal not honored.** A later withdrawal/denial for a purpose overrides
  earlier grants because the latest `recorded_at` decision is authoritative.
  Tested.
- **Global kill switch.** `media_capture_enabled = false` denies all acquisition
  regardless of consent. Tested.

### Important limitation

This phase proves **simulated** enforcement only. There is no real camera,
microphone, biometric, or cloud component, so it does **not** prove enforcement
at a real hardware boundary. No face recognition, voiceprints, health diagnosis,
or emotion/intent inference is implemented. Camera, microphone, biometric
processing, and cloud upload remain disabled.

## 7. Limitations

- Consent is enforced by the simulated gate in Phase F1, but only at a simulated
  boundary. No real hardware component exists, so real-hardware enforcement is
  not yet proven.
- A stored consent record is configuration, **not** proof of legally valid
  consent.
- No raw media, face templates, voiceprints, or secrets are stored anywhere.
- Schedule parsing is intentionally minimal (`daily`/`weekly`/`weekdays`/`HH:MM`)
  and time zones are a small allow-list. It is not a general cron engine.
- The routine model does not confirm activity completion from any data source.
