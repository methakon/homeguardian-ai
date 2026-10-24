# Detection-triggered event videos

## Scope and acceptance

The observer/recording worker owns detection, actual clip duration/codec validation,
and capture. This component never starts a camera, classifies a frame or records
continuous footage. It accepts only a trusted producer's `detected` / `real`
event metadata and provides the private storage handoff and authenticated viewer.
A submitted detector label is the producer's report, not an independently verified
classification by the gateway.

The production gateway currently has **no R2 configuration**. Implementation and
hermetic tests are not live Cloudflare acceptance. Do not claim clips uploaded,
playback working, or an Echo receiving a new link until those legs are exercised.

The gateway UI/backend and VPS mailbox adapter from source
`5e7392149e6c043b86da87cebbf621ffc0a9299a` are deployed. The list/player is served
by the original portal after login, but storage requests currently report 503
until R2 is configured. The Alexa-hosted Code portion is not deployed: authorized
Amazon developer-console access is still required. Restarting the gateway cleared
old in-memory browser sessions, so use the original portal and sign in again.

## Data path

1. The observer produces a browser-compatible, bounded MP4 (H.264 video; AAC if
   authorized audio is included). It measures duration, size and content MD5.
2. The producer requests a reservation from the gateway using its own machine
   bearer credential. Google VIEWER sessions cannot request upload credentials.
3. The gateway reserves bytes durably and returns a five-minute, single-key R2
   PUT URL and the exact required HTTP headers. The producer uploads directly to
   R2; video bytes never pass through the coordination VM.
4. The producer calls completion. The gateway independently checks R2 HEAD
   metadata, including size, MIME type and single-part MD5 ETag. A missing or
   mismatched object never becomes a playable event.
5. The authenticated portal lists completed clips. A playback request returns a
   two-minute signed GET URL. This URL is a bearer capability: a recipient can
   reuse it until expiry. The owner explicitly accepts copying by authorized
   viewers; screen-capture prevention is not a requirement.
6. Alexa can attach the stable portal deep link to the existing request/response
   alert summary. It never receives the R2 signed URL or storage credentials.

No public bucket, `r2.dev` URL, YouTube upload, media proxy, browser storage,
tracking cookie, public MCP listener or workstation process is needed.

## Producer integration contract

All machine routes reject browser Origin headers and URL credentials. Send
`Authorization: Bearer <producer token>` as a header and JSON as
`Content-Type: application/json`. Tokens must not be logged or embedded in URLs.

### Reserve

`POST /internal/event-videos/uploads`

Body fields:

| Field | Contract |
|---|---|
| `event_id` | required stable producer ID, ASCII alphanumeric/underscore/hyphen, 1–128 chars |
| `alert_id` | optional stable native alert ID, same syntax |
| `detected_at` | required ISO timestamp with explicit offset or `Z` |
| `summary` | required plain-text observer summary, at most 500 chars |
| `detector` | required detector identifier, at most 128 chars |
| `severity` | `info`, `warning` or `critical`, as reported by the observer |
| `confidence` | optional finite number in [0,1]; no invented default |
| `provenance` | exactly `real`; synthetic clips are rejected |
| `detection_status` | exactly `detected`; quiet/unavailable/inconclusive are rejected |
| `duration_seconds` | finite positive duration; default configured limit 30 seconds |
| `size_bytes` | exact positive integer byte count; default limit 25 MiB |
| `content_type` | exactly `video/mp4` |
| `content_md5` | canonical base64 of the complete file's 16-byte MD5 digest |
| `evidence` | optional bounded JSON detection evidence, not raw media or credentials |

Unknown fields are rejected. Credentials, media, arbitrary upload destinations and
object keys are not caller-selectable. Reuse the same event ID for a retry; do not
manufacture new IDs to bypass idempotency/quota accounting.

A successful reservation returns `video_id`, `upload` (`url`, `method`, `headers`,
`expires_in`), `complete_path` and `portal_url`. Treat `upload.url` as a secret.
PUT the file bytes to that URL with every returned header unchanged. Signed
`Content-Length`, `Content-Type`, `Content-MD5` and `If-None-Match: *` bind the upload
and prevent replacement of an already uploaded clip. If a PUT's outcome is
unknown or it returns 412 after retry, use completion to check the existing object;
do not overwrite it or drop the reservation.

### Complete

`POST /internal/event-videos/uploads/<video_id>/complete` with body `{}`.

Only successful object verification publishes the event and its notification
metadata. Completion is idempotent. A signed PUT or HTTP 201 reservation is not
an upload-success acknowledgment. Inspect the completion result before discarding
the local recording. The producer owns durable retry/backoff and local cleanup.

HTTP 507 `event_video_storage_full` means the reservation/entry budget is full:
stop cloud uploads and report the storage condition. Do not retry in a hot loop,
discard the local clip as uploaded, or rotate event IDs to evade accounting.

## Viewer routes

Google authentication plus the existing explicit outdoor-review grant is required
for every list, detail and playback request. With the owner-approved
`HG_OUTDOOR_REVIEW_ACCESS=google_viewers`, every Google-signed-in reviewer can read
these outdoor event clips without gaining private household/device-management
permissions. Anonymous, revoked and expired sessions are rejected. Browser writes
remain forbidden; simulations remain isolated.

| Method | Route | Result |
|---|---|---|
| GET | `/api/event-videos` | completed real clips, default current day in Asia/Kolkata |
| GET | `/api/event-videos?from=YYYY-MM-DD&to=YYYY-MM-DD&page=1&page_size=12` | inclusive date range, newest first, paginated |
| GET | `/api/event-videos/<video_id>` | complete stored event metadata/evidence and clip availability, no storage key or signed URL |
| GET | `/api/event-videos/<video_id>/playback` | short-lived signed R2 URL after fresh authorization |

Dates are real calendar dates, interpreted in Asia/Kolkata. Page size is capped
at 50; range is bounded to 366 days. Responses include explicit pagination totals.
The deep link is `/?event_video=<video_id>#event-videos`; it contains no
credentials, survives the existing popup login in the original tab, and opens the
selected event even when it is outside today's default list.

Published records include `clip_status: available|deleting|expired`. Expired clips
retain their original event metadata, evidence, list entries and permalinks.
The portal labels expiration/cleanup explicitly and does not offer a video player
for those states; playback requests return HTTP 410. Unverified pending uploads
never enter the list. Alexa's optional latest-video metadata includes ready clips
only, so an expired video is not advertised as playable.

The existing cookieless session wrapper attaches its closure-held bearer only to
same-origin API requests. Direct video playback uses the short-lived R2 capability,
not the Google/session bearer. CSP permits only the configured account's exact R2
HTTPS hostname, not all R2 accounts. Logout unloads the player and clears displayed
records; it cannot revoke an already issued capability before its expiry.

## Alexa boundary

`GET /internal/event-videos/notification` is a metadata-only machine read protected
by a **separate read-only** `HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE`. It is not
protected by, and never accepts, the producer upload token. It returns the latest
completed real event's bounded summary and portal link, or an explicit absence.
Storage failure returns unavailable, not a fabricated empty success.

The mailbox adapter may add that metadata to an otherwise valid native MCP alert
summary. It must not replace an unavailable observer with a stored-video success
or promote a native zero-alert result into an alert. Optional video-service failure
must not break the established summary lane. Alexa remains request/response;
there is no proactive Echo announcement or unsolicited notification deployment.
A portal URL in an Alexa app card is not proof an Echo opened a browser.

## Production configuration and cost controls

Set only in the protected deployment environment, never source control:

| Setting | Purpose |
|---|---|
| `HG_R2_ACCOUNT_ID` | account identifier; exact pinned S3 endpoint derived from it |
| `HG_R2_BUCKET` | dedicated private Standard-storage bucket |
| `HG_R2_ACCESS_KEY_ID` | bucket-scoped S3 access key |
| `HG_R2_SECRET_ACCESS_KEY` | bucket-scoped S3 secret |
| `HG_EVENT_VIDEO_INDEX_FILE` | absolute protected catalog path outside web root |
| `HG_EVENT_VIDEO_INGEST_TOKEN_FILE` | gateway-owned 0600 producer-key file |
| `HG_EVENT_VIDEO_NOTIFICATION_TOKEN_FILE` | gateway-owned 0600 notification-reader key |
| `HG_EVENT_VIDEO_MAX_DURATION_SECONDS` | clip-duration limit, default 30, hard maximum 120 |
| `HG_EVENT_VIDEO_MAX_BYTES` | per-clip byte limit, no greater than 25 MiB |
| `HG_EVENT_VIDEO_STORAGE_LIMIT_BYTES` | reservation budget; default/hard ceiling 8 GiB |
| `HG_EVENT_VIDEO_RETENTION_DAYS` | completed-clip retention from publication time, default 7 days; only applies when cleanup is explicitly enabled |
| `HG_EVENT_VIDEO_CLEANUP_ENABLED` | exact `1` enables cleanup; absent/other values leave it OFF |

Keep bucket public development URLs and public custom domains disabled. Confirm
this in Cloudflare; signed access alone does not prove the bucket is private.
Provide bucket-scoped object read/write credentials, not an account-wide/global
Cloudflare key. Do not enable paid Stream or change DNS/tunnels/mail/firewall/VPN.

The local budget counts pending uploads as well as completed ones. It does not
silently free reservations whose PUT result is unknown, because an orphaned object
could still consume storage. Saturation fails closed instead of allowing continued
uploads. A corrupt/unreadable catalog is an error, never a new empty catalog.

Cleanup is implemented but OFF by default. Enabling it is a destructive retention
policy decision; do not enable it incidentally while provisioning credentials.
Once explicitly enabled, the gateway schedules at most one completed clip per
minute, with no immediate deletion on startup. Eligibility starts at verified
publication time, not a producer-supplied historical detection date. A machine
producer may also POST `{}` to `/internal/event-videos/maintenance`; browser
sessions and the notification-reader key cannot authorize that operation, choose
a target or override the policy. The same exact opt-in applies to manual calls.

Each cleanup transaction durably marks the clip `deleting` before sending an
authenticated R2 DELETE, then requires DELETE 200/204 and authenticated HEAD 404
before persisting `expired` and releasing the byte charge. Failures keep the
charge and permit conservative retry; unknown pending uploads are never reclaimed.
DELETE/confirmation requests are individually bounded to two seconds and carry
no media. Expired event metadata is retained, so the existing 10,000-entry and
16-MiB catalog bounds still apply even after cloud bytes are reclaimed. This is
not unlimited event-history storage.

Normal gateway shutdown cancels future ticks and waits for an active bounded
transaction. Forced process death, a stale exclusion lock or unrecoverable
post-rename catalog rollback still requires operator reconciliation; never
remove a lock or release quota based on age alone. Disabling the policy stops
future cleanup but cannot restore an already deleted video. Live R2 deletion
acceptance remains separate from the hermetic tests and must precede enabling
unattended cleanup on the real bucket.

R2's published free Standard allowance is 10 GB-month, 1 million Class A and
10 million Class B requests per month, with free internet egress. The 8 GiB
application budget leaves headroom but is not an account billing cap: other
buckets, request counts and shared account usage also matter. Do not promise zero
charges merely because local bytes are below the application budget. Billing
activation/payment approval, if Cloudflare requires it, is an owner action.

References:
- https://developers.cloudflare.com/r2/pricing/
- https://developers.cloudflare.com/r2/api/s3/api/
- https://developers.cloudflare.com/r2/api/s3/extensions/

## Verification and remaining acceptance

Run the hermetic backend, gateway-route and served-UI tests plus OAuth/reviewer
regressions. Cloud transports and producer detections in unit fixtures are explicitly
synthetic; they are never production records. A signed URL match is not a live R2
PUT/HEAD/GET proof. Live acceptance additionally requires a private bucket and
credentials, a genuine clip from the recording worker, verified completion,
playback after login, anonymous/mutation denials, and actual ASK readback with the
stable portal link. Preserve the existing camera/native detection deployment.

Regression commands (all use isolated synthetic fixtures, never the real bucket):

    node --unhandled-rejections=strict --test web_mcp/test_event_videos.js web_mcp/test_event_video_retention.js web_mcp/test_event_videos_ui.js web_mcp/test_event_video_maintenance.js web_mcp/test_event_video_integration.js
    node web_mcp/test_event_video_routes.js
    node web_mcp/test_oauth.js
    node web_mcp/test_outdoor_review.js

The integration test executes the actually served video UI against real local
HTTP gateway endpoints and an explicitly fake S3 transport. Missing optional
native `alert_id`/`confidence` are accepted and displayed as `not reported`;
provided invalid values are rejected. The same test has passed on production's
Node 18 runtime under the service user without accessing real storage or changing
the running gateway. This rehearsal is not deployment or real clip acceptance.
