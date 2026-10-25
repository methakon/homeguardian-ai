# Pre-decode frame callback handoff

## Source contract

The integration change is confined to `src/bridge/CameraAnalysis.h`; accompanying
changes register and exercise tests, not production acquisition or detection.
The observer-owned `src/edge/*` implementation is unchanged.

Supply a `std::shared_ptr<hg::edge::FrameConsumer>` as the optional fifth
`CameraAnalysis` constructor argument. Call
`process(jpeg, captured_at, sequence)` with the acquisition's sampling timestamp
and positive acquisition sequence. The callback runs after the existing source
authorization check, before `decode_jpeg_luma`.

Before `on_frame`, the same header-only guard used by the decoder checks:

- At most 512 KiB, inclusive.
- JPEG start/end magic and a bounded, readable SOF header.
- Positive header-derived width/height, each at most 4096, inclusive.

This is equivalent admission validation, not proof that the compressed scan is
decodable. The existing decoder still handles compressed-data failures. There is
no additional decoder, acquisition path, network client, encoder or disk writer.
The borrow lasts only until `on_frame` returns; retained bytes must be an owned,
bounded copy. Callbacks run under the existing processing serialization and must
be bounded and non-reentrant; they must not call back into `CameraAnalysis`.

The analyzer retains its existing wall-clock correlation, observation and durable
alert behavior. The extra sampling timestamp is used only by the frame consumer.
A legacy `process(jpeg)` call, missing timestamp or zero sequence does not supply
frames to the consumer and invalidates its previous retained context. It does not
invent capture time or change the existing analyzer's processing behavior.

## Authority and lifetime

The consumer is notified false on explicit withdrawal, failed authorization,
consent revision/subject or source-type change, provenance change, and destruction.
Successful authorization notifies true when re-enabling retention. Source and
credential binding are immutable per instance: destroy the old instance before
binding a replacement source. The shared consumer remains alive for the final
shutdown notification. Authorization/withdrawal/processing use the same mutex,
with a private authorization helper to avoid recursive locking. When a consumer
is attached, authorization is refreshed after decode on failure as well as success;
retention therefore cannot survive a withdrawal observed at that refresh simply
because compressed-data decoding failed. Decode-failure accounting is preserved.

The consumer must clear retained bytes and invalidate queued work on false.
The actual `RingFrameConsumer` clears its `EventRing`; a recorder/queue-owning
adapter must also invalidate its own queued/in-flight generation. A test double
exercises that notification with a queued-generation token. This is not proof
that an as-yet-unbound production recorder queue was canceled.

A callback exception disables further frame delivery and attempts withdrawal;
it does not bypass or suppress the existing decoder/analyzer. Shutdown catches
notification exceptions. A consumer that refuses its own invalidation cannot be
certified safe by this interface: production adapters must honor the contract.
External registry changes are observed at the existing authorization checks, not
by a new filesystem watcher. No watcher is introduced or restarted.

## Timestamp and running-system boundary

The interface preserves the supplied acquisition timestamp exactly, including
queue delay; it never replaces it with the `process()` entry clock. Positive time
and nonzero sequence do not attest actual acquisition, freshness, order, uniqueness
or enrollment association; production assembly still owns those guarantees. Tests use a
sampling timestamp thirty seconds before processing and read it back unchanged.

The current `LiveCameraFeed::fetch_frame(now_ms)` places the caller's snapshot
request-start timestamp into `CameraFrame::captured_at_ms`. It does not provide a
verified camera exposure timestamp. The desktop rediscovery branch currently
assigns a completion-time timestamp to its recovered snapshot. Neither fact is
changed or relabeled as physical shutter-time evidence in this patch.

The existing Android JNI and desktop production callers still use the legacy
entry point and do not construct/bind a consumer. This narrow header interface
handoff therefore does NOT yet make the installed application retain pre-roll.
Production assembly must bind the observer's consumer, pass the acquisition
sampling timestamp and sequence, preserve source/provenance cancellation context,
and identify or withhold frames with unknown sampling time. Those shared caller
edits and deployment are a separate integration checkpoint, not silently included
in the requested single-production-header change.

## Verification and outstanding acceptance

`cmake --build build --target hg_camera_analysis_tests --parallel 1`
then `build/hg_camera_analysis_tests` passes 233 assertions in nine test cases.
The five new callback cases cover validation/metadata, lifecycle notifications,
authority withdrawal/replacement on decode failure,
the actual `CameraAnalysis -> RingFrameConsumer -> EventRing` chain, and callback
failure containment. They include buffer poisoning, exact byte/dimension limits,
missing metadata, sequence gaps, ring bounds, credential rebind, missing registry,
withdrawal, consent changes and shutdown. Existing durable-alert tests still pass.
All media fixtures in these host checks are synthetic test inputs, not real
capture acceptance.

Additional passing checks:

- Android decoder: three tests, including real API 30 compilation/link and the
  unsupported API guard.
- Android MCP session: one test harness, thirteen internal regression checks.
- Android recovery: three tests, including 1092 offline netlink/identity assertions.

No phone installation, service restart, gateway modification, cloud upload or
physical event capture is part of this change. A genuine jointly triggered clip,
production queue cancellation, actual capture-time evidence and phone CPU,
latency, recall, queue bounds, memory and temperature remain NOT VERIFIED. The
pre-detector cadence skip is unchanged. No power-saving or fast-event-capture
claim follows from this interface checkpoint.
