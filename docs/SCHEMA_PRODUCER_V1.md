# Producer observation schema — contract v1

**Owner:** Hermes 2 (Panasonic Android service / producer)
**Consumer:** Hermes 3 (C++ receiver / provenance enforcement)
**Status:** FROZEN v1. Additive changes only; a breaking change requires a major bump and both sides
moving together.

This is the shared interface between the two workstreams. It describes what the Android producer
actually emits today — taken from the emitting code, not from intent — so the receiver can be built
against it without either side guessing.

---

## 1. Wire form

**Transport:** mutual TLS, node dials out, nothing listens on the node.
**Framing:** `uint32` big-endian payload length, then that many bytes of UTF-8.
**Payload:** NDJSON — **one JSON object per line**, `\n`-terminated (a trailing newline is normal but
not required; a final line without one is accepted).
**Hard frame cap:** `kMaxFrameBytes = 4096`. Exceeding it is refused *before* allocation.

A drain may carry **zero or more** lines in one frame. Zero is normal and must not be treated as an
error: it means the node had nothing to report this tick.

## 2. The observation object

```json
{"source_type":"camera","observation_type":"motion_score","value":0.42,"unit":"ratio",
 "state":"ok","confidence":0.9,"acquired_at_ms":1761000000000,"provenance":"real_device"}
```

| field | JSON type | required | notes |
|---|---|---|---|
| `source_type` | string | **YES** | which sensor produced it |
| `observation_type` | string | **YES** | what was measured |
| `value` | number | **YES** | finite; the measurement |
| `acquired_at_ms` | integer | recommended | epoch ms, device clock. `<= 0` is replaced by receive time |
| `unit` | string | optional | defaults to `""` |
| `state` | string | optional | defaults to `""` |
| `confidence` | number | optional | defaults to `0.0` |
| `provenance` | string | optional | a **CLAIM** only — see §5 |

**Field order is not significant.** The producer emits the order above; a receiver must not depend on it.

### 2.1 A line is DROPPED when
- any of `source_type`, `observation_type` is absent, **or**
- `value` is absent or unparseable.

The producer counts these and reports `parse_failures`. A dropped line is **not** sent as an error
frame — it never reaches the receiver. If the receiver sees a gap in sequence numbers that is expected;
see §4.

## 3. Producer-side constraints the receiver MUST be built for

These are properties of the emitting code. They are listed because a receiver built on assumptions
that ignore them will break in a way that looks like corruption.

### 3.1 String fields contain no escaping

The producer's field extractor is a deliberate substring scan, not a JSON parser. It reads a value as
everything up to the next `"`.

**Therefore a string field MUST NOT contain `"` or `\`.** They do not occur in the current values
(`source_type`, `observation_type`, `unit`, `state`, `provenance` are all drawn from closed
enumerations). A receiver should treat an embedded quote as a hard parse failure rather than trying to
un-escape it.

### 3.2 Numeric fields are plain decimal

The scanner accepts `[0-9] [+\-] [.] [eE]`. `NaN`, `Infinity` and hex are **not** emitted. A receiver
that encounters them should reject the line, not coerce.

### 3.3 There is no per-line version field

Version negotiation happens once, at session level, not per observation. Do not look for a version key
on the object. See §7.

## 4. Identity, sequencing and what the receiver can rely on

| property | producer behaviour |
|---|---|
| `observation_id` | `<device_id>-<sequence>`, or `node-<sequence>` when the id is not yet known |
| `sequence` | strictly increasing per device, starts at 1, never reused or rewound |
| `acquisition_mode` | **always the literal `"live"`** for this producer |
| queue depth | **bounded at 128** |
| overflow policy | **oldest is dropped**, counted separately as `dropped` |

**Consequences for the receiver:**
- **Sequence gaps are expected and are not errors.** A gap means either a local drop (§3.1/§2.1) or a
  bounded-queue overflow. Treat a gap as lost data to account for, never as corruption.
- **Ordering is producer-side ascending, but the receiver must not assume delivery ordering.**
  Reorder within a bounded window; a sequence below the high-water mark is stale, not an attack.
- **`dropped` and a permanent refusal are different things.** A permanent refusal (§5) means the peer
  was told the frame is unusable; `dropped` means the node discarded it locally before sending.

## 5. Provenance — a claim, never a fact

`provenance` on the wire is **untrusted input**. The producer's own documentation says so, and the
hub derives real provenance from:

```
(authenticated peer certificate) x (device paired) x (device consent) x (declared acquisition mode)
```

**The receiver must ignore the claimed value for any trust decision** and record an unsubstantiated
claim where it differs from the derived result. A attacker-controlled payload must not be able to
assert that it came from a real device.

## 6. Refusals the receiver may send back

Stable strings. A refusal is either **permanent** (retrying is pointless) or **retryable**.

**Permanent — the producer stops retrying that observation:**
```
bad_frame  bad_version  bad_json  oversized  unknown_kind
unknown_source_type  unknown_observation_type  invalid_value
malformed_timestamp  replayed_sequence  duplicate_observation
device_unknown  device_revoked  device_unpaired  identity_mismatch
consent_missing  consent_withdrawn  capture_disabled  unauthorized
```

**Retryable — the producer backs off and retries:**
```
queue_full  processing_failed
```

**Authorization-class refusals (`device_revoked`, `device_unpaired`, `consent_withdrawn`,
`capture_disabled`, `unauthorized`) are terminal for the session.** The producer HOLDS its queue and
does not reconnect through them; it waits for an explicit operator restore. A receiver should not
expect a reconnect after one of these unless an operator acted.

**`processing_failed` must never be reported as success.** A downstream failure that returns an
acceptance would let the producer advance its watermark past data that was never durably handled, and
the observation could never be retried.

## 7. Version policy

- **Major bump** = the receiver MUST refuse. Breaking: removing/renaming a required field, changing a
  type, changing the framing.
- **Minor bump** = additive only (a new optional field). A receiver must ignore unknown keys rather
  than refusing the frame.
- Negotiation happens **once at session start**, echoed in the welcome. A peer with a different major
  is refused with `bad_version` — the whole session, not one observation.

## 8. What is NOT in this schema

Deliberately absent, and a receiver must not expect them:
- **no raw media** — no frame, no PCM, no pixel buffer, no transcript, no biometric template. The
  transport has no path for them by construction.
- **no credentials or key material** — never in a payload.
- **no commands** — this is a one-way producer→hub observation channel. Control flows elsewhere.
- **no free-form text** — values come from closed enumerations.

## 9. Conformance

The producer side asserts this contract in `apk/tests/`. A receiver-side conformance test should
mirror §2, §3.1, §3.2 and §4: required-field rejection, no-escape, plain-decimal, and gap-is-not-error.

**Cross-language seam rule (learned the hard way on this workstream):** a seam spanning two languages
must be verified by counting BOTH sides. A Java compile and JVM tests pass happily while the native
half is missing, because injected fakes never load the library. Whoever changes the wire form must
confirm producer emits and receiver accepts, not just that one side builds.
