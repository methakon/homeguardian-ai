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

## Rulings on the four amendments at 96f58b6

These are design agreements only. The MCP stability handoff is still NOT GIVEN;
no implementation or device acceptance is implied.

1. **Raw-JPEG ring: accepted with bounds and lifecycle conditions.** Retain the
   existing fetched JPEG bytes, not reduced luma, for snapshot-based pre-roll.
   Independent JPEGs have no inter-frame GOP dependency. Preserve actual sampling
   timestamps; do not claim continuous/full-rate video or interpolate missing
   history as captured frames. The callback borrows immutable bytes only for its
   duration; the ring owns any retained bounded copy. Important correction: the
   512 KiB/header/dimension guard currently executes INSIDE `decode_jpeg_luma`,
   not automatically at `process()` entry. Integration must apply equivalent
   checks before a new pre-decode callback can retain input. Enforce total bytes,
   frame count, time window and bounded callback work, with no network or disk
   encoding in the callback. Derive dimensions from checked headers. No new
   decoder at the seam; the existing Tier-0 decode still runs per acquired frame,
   while an event-only worker may decode retained JPEGs for encoding. Recheck
   authority before retaining/encoding/publishing and invalidate ring/jobs on
   revocation, source/enrollment change or lifecycle stop. Measured input cadence
   and memory, not guessed few-KB frame sizes, determine supported pre-roll.
2. **Public advisory adapter: accepted as an integration-owned addition.** Caller
   may propose a bounded observation ID, approved new detector ID and summary;
   source identity, timestamps, consent and provenance are derived/checked by
   integration. Caller validation alone is not authority. Do not permit reserved
   detector impersonation or caller-selected severity/authorization. Reuse the
   current pipeline/policy/persistence path under the existing serialization
   discipline, without recursive locking. `DetectorStatus` has no advisory enum:
   advisory means excluded from evidence eligibility, not a fabricated status.
   A new non-allowlisted event can still affect bounded history or trigger rule
   evaluation; prove mixed-traffic history, alert and cooldown non-regression
   rather than assuming unchanged behavior from the allowlist alone. An accepted
   advisory observation need not produce a durable alert, and must not be reported
   as one. Final signature and admission/rate limits require interface review.
3. **Provenance: existing acquisition derives it; recorder does not assert it.**
   `CameraAnalysis::process` currently derives `synthetic` versus `real` from the
   registry's `is_synthetic` after authorization, and rechecks authorization before
   persistence. Thus the physical acquisition variant already uses `real` despite
   the stale simulator-only comment in `Observation.h`. Integration owns a focused
   comment correction and tests; do not change enum semantics. Supply an
   acquisition-derived, revocable context to the recorder/uploader; caller text
   cannot upgrade synthetic/unknown input to real. Synthetic and uncertain input
   must never enter the real video store. The gateway's producer bearer trusts
   this producer report; it does not independently attest camera authenticity or
   prove the footage is physically genuine. Capture authorization and registry
   provenance must be tested on the actual installed phone before live acceptance.
4. **Android encoder: MediaCodec accepted as the proposed platform path.** Use
   platform H.264 encoding plus MediaMuxer MP4 packaging, behind an injected
   recorder backend; no ffmpeg executable, new model runtime or vendored library.
   Reuse the existing Android image-decoding facilities for event-only JPEG decode.
   API 30 alone does not prove a usable H.264 encoder, compatible color format,
   dimensions, throughput, thermal behavior or successful MP4 finalization. Probe
   supported capabilities and fail closed if unavailable; no silent fallback or
   dependency installation. Specify JPEG-to-encoder conversion, bounded working
   memory, presentation timestamps, EOS/drain, cancellation, atomic finalization,
   and checksum of the FINAL MP4. Keep Android platform glue outside the pure gate
   and queue components. Host fakes cannot prove codec execution on the phone.

Observer may finalize the interface specification against these rulings while
waiting. Code and shared-file edits remain blocked on the explicit stability gate.
