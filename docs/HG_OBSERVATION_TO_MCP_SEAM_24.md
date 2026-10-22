# HG-CPP-OBSERVATION-TO-MCP-24 — the seam between an admitted observation and an MCP-visible result

Status: **inspection complete, hypothesis corrected, no adapter implemented.** This document is the
patch-ready ownership and interface plan; the code it describes is unchanged.

Mission: HG-CPP-OBSERVATION-TO-MCP-24, following HG-CPP-MCP-LOCAL-SMOKE-23 (which verified the MCP surface:
loopback-only, bearer auth, Origin allowlist, two read-only tools, 46/46 in `scripts/verify_mcp.sh`).

---

## 1. The seam, by code anchor

There is exactly one place where an admitted observation crosses from the network boundary into analysis:

```
ObservationIngest::accept(const PeerContext& peer, const DeviceMessage& message, const Sink& sink)
    src/net/ObservationIngest.h:133
using Sink = std::function<void(const SecurityObservation&)>;
    src/net/ObservationIngest.h:79
```

`accept()` is the admission decision — authentication, pairing, revocation, credential match, identity
claim, consent, capture state, source/observation allowlists, value/timestamp/sequence sanity, queue
bounds, and the provenance it records (`sec/net/ObservationIngest.h:3-24`). The `Sink` is the boundary: the
first thing the application sees, and the last thing the network layer controls.

**Transactional ownership of "this observation counts once" already exists and is two-phase**
(`src/net/ObservationIngest.h:81-123`):

| hook | meaning | owner of the decision |
|---|---|---|
| `is_completed_repeat(device, seq, obs_id)` | true = already durably completed → answer idempotently, process nothing | durable store |
| `begin(device, seq, obs_id, *reason)` | `marked` / `already_completed` / `replayed` / `concurrent` / `unavailable` (fails **closed**) | durable store |
| `complete(device, seq, obs_id, *reason)` | false = the acceptance was not recorded, so it is not reported as one | durable store |
| `abandon(device, seq)` | best effort: processing failed, drop the in-flight marker | durable store |

This is the answer to "transactional ownership at the seam": the **durable store owns the acceptance
transaction**, `ObservationIngest` owns admission, and the `Sink` consumer owns processing. A downstream
failure is retryable precisely because `begin`/`complete` are separate (the D-03.1 single-hook version
silently lost a sequence — `src/net/ObservationIngest.h:83-87`).

## 2. What I validated, and what the code contradicts

The proposed design was: *"canonical application state belongs to the native C++ hub/application-persistence
domain; ingress is a transport/admission boundary that forwards admitted feature observations through an
existing bounded ingestion interface; MCP reads the application's authorized status/alerts via
AlexaContract, preserving its own separate audit DB for request audit only."*

**Validated:**
- The bounded ingestion interface exists — `ObservationIngest` with an injected `Sink` and injectable
  durable hooks, deliberately free of sockets/TLS/threads/SQLite so the whole authorization path is
  testable (`src/net/ObservationIngest.h:22-24`).
- The MCP server does not reimplement application state: it builds the same `Application`,
  `PersistenceAudit` and `AlexaCredentialStore` the NDJSON bridge builds, and authorization is the
  contract's (`src/main_mcp_server.cpp:4-7`).

**Corrected, with evidence:**

1. **The ingress is not a transport.** `src/main_ingress.cpp:7-12` states it "converts accepted observations
   into the EXISTING Observation → Event → Pipeline → AlertPolicy path and **persists alerts to SQLite**",
   and it does: `audit->record_alert(...)` at `src/main_ingress.cpp:276` and `:284`. So an ingress
   deployment today is a **complete application instance** that terminates ingestion locally. The
   forwarding interface the hypothesis assumes does not exist; the bounded interface exists *inside* the
   process.
2. **`HG_MCP_AUDIT_DB` is not a request audit.** `PersistenceAudit` is "durable, privacy-safe audit of
   DELIVERED alerts … using … the `alerts` table via `AlertRepository`"
   (`src/core/PersistenceAudit.h:3-11`); its API is `record_alert` / `total_alert_count` /
   `prune_older_than*` (`:65-101`) — there is no request concept. So an MCP-side "audit DB" written by the
   MCP path would be a **second writer of alert records**, which the design decision itself forbids.
3. **In fact nothing writes it.** `record_alert` appears only in the ingress (`src/main_ingress.cpp:276,284`)
   and `src/mcp/*` references neither `PersistenceAudit` nor `record_alert`. The MCP process opens an alert
   store it never writes, and **MCP request auditing does not exist in C++ today**.

## 3. The three stores, and who may touch them

| store | today's writer | today's readers | content |
|---|---|---|---|
| `homeguardian_ingest_audit.db` (ingress) | the ingress (`record_alert`) | none | alert records + (separately) `device_replay_state.json` |
| hub store (`hg-s3_hg_data`) | the hub's in-process `Application` | the hub's own views | events + alerts |
| `HG_MCP_AUDIT_DB` (MCP) | **nobody** | nobody | empty alert schema |

Consequences, both observed live in HG-CPP-MCP-LOCAL-SMOKE-23: an admitted observation reaches the store of
**whichever process admitted it**, and the MCP answer reflects the store **its** process opened. Sharing one
file between the ingress (writer) and the MCP server (reader) is *not* the hazard — the hazard is **two
writers of alert records**, which is what a naive "mount the same DB" would create between any two of these.

## 4. Patch-ready ownership plan (for Antigravity — not implemented)

Pick **one** placement; do not mix. Both keep the single-writer rule.

- **Option A (smallest, config/placement only — no C++ change).** One process is the application for device
  observations (in the deployment that is the ingress, since it already runs the full path), and the MCP
  server is pointed at *that* application store **read-only**, with its own audit handle left unused or
  removed. Single writer, reader-only MCP, no new interface. Requires: a read-only open mode where
  `PersistenceAudit` currently opens read-write (`src/core/PersistenceAudit.h:59`), or an explicit
  configuration assertion that the MCP process never calls `record_alert`.
- **Option B (bigger, new interface).** The ingress stops terminating analysis and forwards admitted
  observations through a bounded, authenticated internal channel to the hub's ingestion, so the hub is the
  sole application. This is the design the hypothesis describes, and it needs a new bounded transport plus
  a home for the replay hooks (currently `ReplayState`, `tests/net/test_network_contract_audit.cpp:224`).
  Not proposed as the smallest change.
- **Either way, if MCP request auditing is wanted, it is a new capability**: a request-audit store with its
  own schema (principal, method, outcome, no payload) — never an `alerts` row, and never a second writer of
  application state.

**Not changed, deliberately:** no integration worktree edits, no keys, no broad bind, no second gateway, no
change to `ObservationIngest`'s contract.

## 5. What the tests prove today, and the two gaps this mission closes

Already proven, on the real engine and the real MCP handlers:

- `tests/integration/test_network_sensor_to_mcp.cpp` — authorized producer → authenticated receive →
  normalized observation → ingest detectors → the real `Application` pipeline and fusion rule → delivered
  alerts → the real MCP tools over JSON-RPC (`:275`), plus consent denial, disconnection, replay, malformed
  input and revocation leaving engine state alone (`:374`), an unauthenticated MCP caller getting no data
  (`:444`), a release trace (`:472`), and a **genuinely alert-generating** fixture: two authorized nodes
  (motion + audio) whose corroboration makes the release deliver an alert which the real
  `get_alert_summary` then reports, `alert_count == delivered.size()` (`:609-714`).
- `tests/net/test_network_contract_audit.cpp:224-244` — restart and idempotence at the durable seam: a fresh
  `ReplayState` reading the same store answers `is_completed_repeat` true, `begin` = `replayed`, and the next
  sequence resumes forward.
- `scripts/verify_mcp.sh` — the transport/auth surface (46/46).

**Gaps closed by this mission's tests:**
1. The non-alert fixture asserted only that the payload *contains* a field named `alert_count`; it never
   asserted the answer is **zero**. "No false alert" is now asserted on the tool output itself.
2. Store separation was untested: the application's alert store and the MCP-side audit handle are now shown
   to be distinct artifacts, and an MCP request is shown to write nothing to either — the honest statement
   of finding 3 above.

## 6. Open, and honestly not closed

- An admitted observation still cannot reach an MCP answer **across processes**, because no forwarding
  interface exists (finding 1). That is a placement decision, not a bug to patch silently.
- `HG_MCP_AUDIT_DB` currently buys nothing: an unused alert store in the MCP process. Either drop it or give
  it the request-audit schema it claims.
- Every observation in these tests is synthetic and labelled so. No camera, microphone or device produced
  anything here.

---

## 7. IMPLEMENTED: HG-CPP-MCP-READONLY-25 (Option A chosen)

Option A is now in the code, not a plan.

| change | file | why |
|---|---|---|
| read-only connection: `SQLITE_OPEN_READONLY` + `PRAGMA query_only` + busy timeout; never creates the file or its parent directory; records whether the READONLY flag sufficed for a WAL store or the connection had to be opened read-write and pinned | `src/persistence/SQLiteDatabase.{h,cpp}` | the guarantee that there is no second writer is SQLite's, not a convention |
| `AlertRepository(db, ensure_schema=false)` | `src/persistence/AlertRepository.{h,cpp}` | a reader must not run `CREATE TABLE IF NOT EXISTS`: creating an empty `alerts` table inside a missing store IS the "no alerts" masking |
| `PersistenceAudit(path, Access::read_only)`: opens an existing store, proves it is a HomeGuardian alert store (`count()` must succeed), skips schema creation and migration, throws otherwise | `src/core/PersistenceAudit.{h,cpp}` | a reader that cannot fail closed will report "no alerts" for "no storage" |
| the MCP server reads `HG_MCP_ALERTS_DB` read-only; `HG_MCP_AUDIT_DB` kept as a **deprecated alias** (it never meant "request audit"); an unopenable store leaves the provider unavailable, which the contract turns into a refusal | `src/main_mcp_server.cpp` | observed, not asserted: the shipped ingress config states the same design ("The SAME file the hub serves … one writer at a time is serialized and readers see a consistent snapshot") |

Verified fail-closed behaviour: a **missing** store answers `isError: true`
("the request could not be authorized") - never `alert_count: 0` - and the reader does not create the file.
A valid **empty** store answers `alert_count: 0` with success. Those are the two different statements.

Evidence: `scripts/verify_observation_to_mcp_path.sh` (14 checks, three processes) and
`tests/persistence/test_alerts_read_only.cpp` (4 cases) with the ownership case in
`tests/integration/test_network_sensor_to_mcp.cpp`.
