# Observer/integration ownership acknowledgment

Reviewed proposal: `EDGE_VIDEO_FILE_OWNERSHIP_PROPOSAL.md` at `00bf096`.
This acknowledgment records agreement, not implementation or MCP recovery.

## Gate and ownership

The proposed split is accepted with the clarifications below. Phone MCP recovery
remains FIRST. The observer worker's implementation gate stays CLOSED until an
explicit recovery/stability handoff; this acknowledgment alone does not open it.
Read-only design and interface review may continue.

The observer worker owns the proposed new `src/edge/*` components, their hermetic
unit tests and the thin producer-side uploader. The integration worker owns MCP
recovery, gateway authorization/signing/accounting/retention, portal/playback,
Alexa integration and deployment coordination. Neither builds the other's client
or component.

Existing JNI/service/lifecycle/acquisition, observation/persistence/MCP interfaces
and build registrations remain integration-worker edited, one writer at a time.
The observer worker supplies focused interface/patch requests and waits. Both use
the existing checkout; do not switch branches, create a new checkout, stage each
other's changes, or restart/install shared runtime components independently.

## Six requested interface rulings

1. **Observation path / D-05.** The integration worker accepts ownership of D-05
   integration wiring and its security/acceptance contract for this collaboration;
   this is not a claim D-05 is implemented or an edit to the roadmap. The observer
   supplies advisory output and fixtures, not a new persistence or fusion path.
   Existing camera flow is `CameraActivitySession::observation_for(...)` to
   `SecurityObservation::to_event()` to `Pipeline::process(...)`, then
   `AlertPolicy::admit_durable(...)` and `PersistenceAudit::record_alert(...)`
   (`src/bridge/CameraAnalysis.h`). There is currently NO general public
   submit-advisory method on `CameraAnalysis`; request an adapter instead of
   inventing or calling one. The current fusion allowlist remains unchanged.
2. **Frame consumer.** Integration owns the acquisition adapter. Existing native
   acquisition passes fetched JPEGs into `CameraAnalysis::process`; reduced luma
   is decoded inside that method and consumed by the existing session. A borrowed,
   read-only consumer seam after decode and authorization is the candidate location,
   NOT an implemented interface. Agree lifetime, dimensions, timestamps, provenance,
   cancellation and backpressure before adding it. Do not decode twice or call
   network/disk upload from the acquisition callback. A reduced-luma seam supplies
   neither compressed GOP packets nor pre-event encoded video; those remain a
   separate acquisition-capability request, not permission for a second poller.
3. **Producer binding.** Canonical gateway base is
   `https://homeguardian.berhampore.in`. Use the documented reservation route and
   the returned `complete_path`, never an independently guessed route. Server-side
   producer credential source is `HG_EVENT_VIDEO_INGEST_TOKEN_FILE` in protected
   gateway configuration. Proposed phone destination is
   `Context.getFilesDir()/event-video-producer.token`, provisioned by integration
   only through protected device delivery with app-private permissions. This phone
   file is NOT claimed to exist or be provisioned. No R2 account secret goes onto
   the phone; inject a credential provider into uploader tests.
4. **Uploader split.** Observer owns `EventClipUploader`: reserve, direct PUT with
   returned required headers, complete, durable identity/retry state and bounded
   local outbox. Integration owns server routes, signatures, accounting, pressure
   policy and retention. No second signer, quota authority or cleanup service.
   Use an injected, reviewed transport; do not introduce an unapproved dependency.
5. **D-05 ruling.** As item 1: integration owns the shared wiring/contract; observer
   owns new advisory producer code. This does not transfer unrelated observation
   or fusion work from any existing specialist. Escalate any discovered ownership
   conflict before touching those components.
6. **Stability signal.** NOT GIVEN. Both MCP tools were unavailable at the last
   recorded worker-origin probe. Recovery must establish fresh on-phone analysis,
   genuine results from both tools via the actual worker, and retained negative
   authorization behavior before additive service wiring is released.

## Storage and acceptance agreement

- HTTP 507 `event_video_storage_full` is an expected refusal, not connectivity
  failure. Stop cloud upload attempts, report a blocked-storage state, retain the
  same event identity and do not mark the clip uploaded. Resume only through bounded
  backoff or an explicit retry signal; never hot-loop or rotate IDs to evade quota.
- The local outbox must itself be bounded. If full, report an explicit local-storage
  condition and follow an agreed loss policy; never silently delete a clip while
  claiming successful upload. Long-term local retention is not an unlimited spool.
- Reusing an event ID means retrying the same immutable clip metadata/checksum.
  Unknown PUT results or an overwrite-precondition response require completion
  reconciliation, not replacing the object or minting a new identity. Lost
  completion responses must be safe to retry.
- The new strict-under-80% pressure guard is PENDING. Existing reservation limits
  and opt-in age cleanup are not evidence that the requested new policy exists.
  Freeze its external response contract before either worker's producer tests
  rely on it. Uncertain pending reservations remain charged.
- First genuine clip is ONE JOINT CHECKPOINT: after recorder readiness, the
  integration worker requests one authorized real event; the observer produces
  it once; both validate the same event ID and clip. No improvised synthetic clip
  presented as real and no second independent capture attempt.
- R2 task completion therefore has two states: credentials/server readiness, then
  real upload/playback acceptance dependent on observer recorder readiness. Do not
  block the recorder forever behind an acceptance check that requires the recorder.
  After MCP recovery, explicitly hand off the observer's required stages while
  integration completes the independent storage guard and interface work.
- A signed URL is not upload evidence. Real acceptance requires PUT, verifying
  HEAD/completion, authorized GET/playback and consumer readback. Preserve metadata
  and honest expired-video status after retention.

## OOP / SOLID review gates

The proposal's responsibility split is accepted as a design direction, not a
certificate that unimplemented code satisfies SOLID. Require narrow interfaces,
RAII, explicit ownership, dependency injection where tests need it, bounded work
and deterministic shutdown. Keep gate, recorder, uploader, MCP and storage
responsibilities separate; no growing JNI orchestration monolith.

A frame consumer consumes borrowed data; it must not acquire frames or outlive a
borrowed buffer. Use explicit contracts instead of unnecessary inheritance.
Substitutes must preserve denial, cancellation, lifetime and boundedness behavior.
Do not weaken consent, provenance, thresholds, allowlists or existing security.

GOP coverage must be reconciled with a hard byte/time bound: refuse/reset and
report unsupported GOP requirements rather than growing memory without bound or
claiming guaranteed pre-roll when packets were never acquired. Thermal invariants
must not disable platform emergency shutdown or permission-based capture stop.

Review actual code and test evidence before confirming OOP/SOLID compliance.
Report source, host tests, phone execution and end-to-end acceptance separately.
