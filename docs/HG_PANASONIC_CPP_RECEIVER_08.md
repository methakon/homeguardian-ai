# HG-PANASONIC-CPP-RECEIVER-08 — isolated C++ receiver for authenticated node observations

**Worker:** Hermes Session 3. Isolated branch, no merge, no Android file touched, no Oracle change, no camera
configured, nothing installed on either handset.

## Deliverables

| Field | Result |
|---|---|
| **MISSION** | HG-PANASONIC-CPP-RECEIVER-08 |
| **BRANCH** | `experiment/hg-panasonic-cpp-receiver-08` (branched from the accepted camera state `071a0592`) |
| **EXACT SHA** | reported in the Issue #19 comment; the receiver lands in a single commit on that branch |
| **SCHEMA CONTRACT** | reconciled below — producer NDJSON ⇄ hub `DeviceMessage`, with four gaps named |
| **CPP RECEIVER STATUS** | **IMPLEMENTED, TEST-ONLY** — `src/net/PanasonicNodeReceiver.{h,cpp}`; no physical integration claimed |
| **AUTHORIZATION TESTS** | **PASS** |
| **CONSENT TESTS** | **PASS** |
| **PROVENANCE TESTS** | **PASS** (with one measured asymmetry recorded) |
| **QUEUE / RECONNECT TESTS** | **PASS** |
| **MIA3 JNI INTEGRATION REQUIREMENTS** | §5 |
| **RESOURCE BUDGET** | §6 — **not measured on the Mi A3** and not claimed |
| **UNIT / INTEGRATION TEST COMMANDS** | §7 |
| **HARDWARE GATES** | §8 |
| **NEXT ACTION** | §9 |

## 1. Phase A — the contract, reconciled

Two correct halves existed with **no adapter between them**, which is the whole gap this mission closes:

- **Producer** (Android, `dev/production-integration`, Hermes 2's lane — read only, never edited):
  `NodeSession.pump()` parses NDJSON lines and hands them to `ObservationTransport`. Fields it reads:
  `source_type`, `observation_type`, `value`, `unit`, `state`, `confidence`, `acquired_at_ms`, `provenance`.
- **Consumer** (this tree): `ObservationIngest::accept(PeerContext, DeviceMessage, Sink)` — the authenticated
  receiver that already owns identity, consent, revocation, replay, clock skew and the bounded queue.

`docs/HG_CAM_09_INTEGRATION_CONTRACT.md` (in another worktree) is a **media** contract whose remaining work is
the media channel; it confirms the identity and transport primitives are done (`TlsTransport`, per-device
certificates, private CA) and that a media session on top is outstanding. This receiver deliberately sits on
the **observation** side of that line: it transports bounded scalars, and it has no path by which a frame, an
audio sample, a transcript or a biometric template could be carried.

### The four gaps the reconciliation found

1. **No `schema_version` is sent.** The producer emits none, so the receiver accepts that case *and counts it*
   (`schema_absent`), and refuses any version it does not implement. Recommendation: the producer should send
   `schema_version: 1` so a future change is detectable rather than assumed.
2. **`provenance` defaults to `"real_device"` when absent** — a missing field becomes a physical-evidence claim
   at the source. The receiver treats the **session** as the authority: a claim may narrow, never widen, and an
   absent claim is never read as real. Recommendation: the producer should omit the field or mark it
   `"unclaimed"` rather than defaulting to real.
3. **`state` has no field on the hub side.** Rather than invent a semantic mapping (a fabricated number is
   worse than a missing one) it is counted and reported as an open interface decision.
4. **Closed vocabularies are enforced by the hub**, and the producer must speak them exactly:
   `allowed_source_types` = motion, contact, presence, camera_availability, microphone_availability,
   device_availability, tamper, environmental_threshold; `allowed_observation_types` =
   `motion-changed-fraction`, `audio-rms`, `audio-peak`, `audio-zero-crossing-rate`, `activity-level`,
   `availability`, `contact-state`, `temperature`, `humidity`, `pressure`, `illuminance`, `tamper-state`.
   The receiver validates the source type against the hub's own decoder and lets the ingest enforce the rest, so
   there is no second list to drift.

## 2. Phase B — what the receiver does

`PanasonicNodeReceiver::accept_line(peer, line, sink)`:

1. **Bounded** line length, refusing `oversized` before parsing.
2. **Strict parse** — a missing, mistyped or non-finite field is refused as `malformed_payload` with a reason,
   never filled from a default: a partially-understood observation is worse than a refused one because it looks
   like data.
3. **Schema** handling as in gap 1.
4. **Closed vocabulary** for `source_type`, borrowed from the hub's own decoder.
5. **Provenance derived, not trusted** — the security-relevant rule; see §4.
6. **Identity from the session, never the producer**: `device_id_claim` is left **empty**, so a node cannot name
   itself into another device's identity or consent.
7. **Everything else delegated** to `ObservationIngest`: authentication, pairing, credential binding, consent,
   revocation, replay, future-skew, age, ordering and the bounded queue (`queue_full` refuses rather than
   growing). The adapter re-implements none of it — that is what keeps this from becoming a second engine.
8. **Refusal vocabulary is the hub's**: refusal strings come from `reject_reason_to_string()`, the same words
   the hub already puts on the wire.

## 3. Phase C — test coverage (`tests/net/test_panasonic_node_receiver.cpp`, tag `[panasonic]`)

**234 assertions / 10 cases, all passing** under `'[panasonic]'`; the producer-shape case is additionally tagged
`[compat]` and reports **44 assertions / 1 case** on its own. Deterministic mock input only: every line and peer
context is constructed in the file, no socket, no certificate, no device.

| Required case | Covered |
|---|---|
| Valid authenticated observation | yes — accepted, delivered, `device_id` from the session |
| Unknown device | yes — refused, nothing delivered |
| Revoked device / consent missing | yes — both refused, nothing delivered |
| Consent withdrawn during connection | yes — and resuming is **two steps** (withdrawal also disables capture) |
| Malformed payload | yes — not JSON, missing field, non-numeric value, non-finite value |
| Invalid schema version | yes — absent is accepted *and counted*; an unimplemented version is refused |
| Oversized payload | yes |
| Timestamp skew | yes — far-future and too-old both refused |
| Replay | **delegated**: frame/sequence replay is asserted by the existing `[net]` suite against the ingest; this adapter additionally never emits a duplicate or decreasing sequence, which is the hole it could reopen |
| Queue overflow | yes — a full queue refuses with a named reason, never a silent drop |
| Disconnect / reconnect | yes — consent restoration + explicit capture re-enable, then acceptance resumes |
| Multiple camera-node identities | yes — two nodes stay independent (each with **its own** credential binding) |
| No active camera | yes — no input ⇒ no delivery, and no fabricated "no activity" |
| No unauthorized MCP data exposure | yes — a refused line delivers nothing to the pipeline |
| False provenance claims | yes — see §4 |
| Receiver unavailable | yes — refusals are named and never mistaken for "no activity" |

**Not claimed:** none of this is physical Panasonic integration. The suite proves the adapter's rules.

## 4. Provenance: the rule, and one measured asymmetry

- A `real_device`/`real` claim on a **synthetic session** is **refused** (`provenance_claim_widens_session`),
  not quietly downgraded — a producer claiming physical provenance for a loopback input is a defect worth
  surfacing.
- An **absent** claim on a synthetic session stays synthetic (the producer's own default would have made it
  real).
- An invented claim string is refused (`unknown_provenance_claim`).
- **The REVERSE direction is tested, and it is the direction that must never break.** An explicit `synthetic`
  claim on a **real** session is **accepted** — narrowing is always permitted — counted in
  `provenance_narrowed`, and **never** counted as a refusal: a node honestly declaring its own data simulated is
  not treated as a falsy claim, and there is no reciprocal `provenance_claim_narrows_session` refusal. This is
  asserted twice: once on the documented line (`[panasonic][provenance]`), and once on the shape the producer
  actually emits (`[panasonic][compat]`, an honest `producer_line("synthetic", …)` on a live session), so the
  direction is pinned for both the contract document and the producer's real output.
- **The claim survives on the record even when the hub's own field disagrees.** An accepted observation keeps
  `provenance_claim` and `acquisition_mode` beside the hub's `provenance`, which is what makes the asymmetry
  below auditable instead of a silent overwrite. Both are asserted, not assumed. The 2×2 matrix around the
  rule — claim agrees with the session (neither counter moves), claim narrows it (accepted, `narrowed`),
  claim widens it (refused, `provenance_refused`), claim absent (session's own value) — is asserted cell by
  cell, so a one-directional fix on either side now fails loudly.
- **Mutation-checked:** inverting the rule (refusing a synthetic claim when the session is real) fails **1
  assertion in each** of the two reverse-direction tests, so neither is vacuous.
- **Measured asymmetry, recorded rather than hidden:** the hub records the provenance *its own session model*
  supports. A claim can never **inflate** it — good — but in the measurement it could not **deflate** it either:
  a `synthetic` claim on a session the hub regards as real was recorded as `real`. The hub is the authority;
  this adapter reports what was recorded in its result detail so a caller cannot mistake a claim for a fact.
  Whether the producer *should* be able to mark its own output synthetic is a contract decision, not something
  to settle by making the adapter overrule the hub.

## 5. Mi A3 / JNI integration requirements (Phase A hand-off to Hermes 2, not an edit)

1. **Per-node credential binding**: the session credential must match the device's registered credential — a
   node presenting another node's credential is refused (asserted). The Android side must therefore present
   the credential that belongs to the node identity it claims.
2. **Identity from the certificate** (X-01), not from the payload: `device_id_claim` is empty on the hub side
   by design.
3. **Send `schema_version`** on each line (gap 1) and stop defaulting `provenance` to `real_device` (gap 2).
4. **Observation type must be an allowlisted value** (gap 4) — the producer's `source_type`/`observation_type`
   pairs must be drawn from that list.
5. **Reuse the existing mutual-TLS transport**; no second credential authority, no new socket semantics.
6. **MCP stays loopback-only and authenticated** — the receiver adds no listener and no network surface of its
   own.
7. **`state`** (gap 3): decide whether it becomes a hub field, is carried as a bounded string feature, or is
   dropped.

## 6. Resource budget — NOT MEASURED on the Mi A3

Stated plainly rather than estimated: the receiver is a pure transform over one line — parse, validate, shape,
delegate — with no threads, no sockets, no allocation beyond the parsed fields, and it inherits the ingest's
existing bound (`max_pending` = 64 observations). No CPU/RAM/thermal figure is claimed for the Mi A3 because
nothing was run there; measuring it needs the device and is another lane's step. Design intent: keep the native
side lightweight, and let the hub's existing bounds carry the backpressure.

## 7. Test commands

```sh
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[panasonic]'   # 234 assertions / 10 cases
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests '[compat]'      # 44 assertions / 1 case (producer-shape)
HG_SOURCE_DIR=$PWD ./build/homeguardian_tests                 # full suite: 10096 assertions / 752 cases
ctest --test-dir build                                        # 3/3
```

## 8. Hardware acceptance contract (Phase E — for the later joint test)

Each gate must be backed by a *positive* observation, and physical evidence must be separated from simulation at
every step:

1. A real Panasonic observation was produced (a counter/line that exists only with the sensor active).
2. It crossed the authenticated private network (transport counter + the peer's certificate identity).
3. The Mi A3 accepted it under the correct device identity (audit record naming the certificate, not an address).
4. Consent was valid at acceptance time (recorded subject/purpose).
5. The C++ pipeline processed it (the existing pipeline's own event count moved).
6. MCP reported the correct bounded state (the real tool output, never a mock's).
7. Revocation stopped subsequent acceptance (**and** required the two-step resume to restart).
8. No household media was exported or stored (nothing retained anywhere in the path).

Until a gate has its evidence, the corresponding claim stays unwritten — and the whole path stays
**BLOCKED/WORKING-DIRECTORY-LOCAL** rather than "probably fine".

## 9. Next action

Prefer the explicitly scoped outcome over a working-looking one: the receiver is ready for interface review
(`PanasonicNodeReceiver` + the four gaps above). The producer side needs the three small changes (schema
version, provenance default, allowlisted types) before a joint run is meaningful. No merge, no APK change, no
handset install happens from here.
