# Current task record and discussion decisions

This is the durable execution queue and handoff, not a new roadmap milestone.
Update statuses and evidence here as each task finishes. Do not treat an interrupted
response, a plan, a build, or an active service as successful live acceptance.

## Execution order

1. **FIRST: restore both phone MCP tools.** Owner explicitly prioritized this over
   R2 work and hosted Alexa deployment. Reproduce from the actual VPS mailbox-worker
   identity; trace the phone native service, outbound bridge, private reader,
   authentication and MCP session lifecycle. Check installed APK/source provenance
   before assuming an earlier session fix was installed. Fix the demonstrated cause,
   test it, and verify genuine results from both `get_homeguardian_status` and
   `get_alert_summary`. Verify missing/invalid authorization remains denied. Do not
   replace native data with portal metadata or synthetic responses.
2. **Implement the requested storage-pressure safeguard.** Bound accepted upload
   reservations strictly below 80% of the allocated storage budget, including
   in-flight uploads. Delete oldest managed, published clips when space is needed;
   preserve event records. Reuse durable deletion and confirmed-absence accounting.
   Refuse uploads if removal cannot be confirmed or capacity remains insufficient.
   Never reclaim uncertain pending uploads, remove unrelated objects, or claim a
   bucket guard controls all other buckets or all account billing. Verify the
   strict boundary, concurrency, failure and restart cases before activation.
3. **Activate and validate private R2 storage safely.** Read only the four authorized
   settings from the existing root `.env`; transfer required values through a
   protected deployment path without printing them or committing environment files.
   Preserve unrelated deployment configuration. After the storage guard is tested,
   validate the real bucket, direct producer upload, publication and authorized
   browser playback. Coordinate genuine clips with the recording worker; do not
   produce synthetic evidence and present it as real.
4. **Deploy the existing Alexa-hosted Code update.** Owner says the Amazon console
   is already authenticated on the system. Inspect and use that session rather than
   asking the owner to authenticate again by default. A failed local browser-debug
   connection does not establish that the Amazon session is logged out. Update only
   the existing skill's intended files; preserve its working configuration.
5. **Accept the complete consumer path.** Verify native observer data through the
   real worker and existing Alexa request/response lane, concise speech, the stable
   portal event link, login return, authorized event details and video playback.
   Verify anonymous/private-household denials remain intact. Document any physical
   Echo, phone reboot, network-change or workstation-shutdown checks not performed.

Execute these serially, starting with item 1. Do not switch to later items merely
because an earlier task is difficult. Record a real blocker and the next required
input if progress is impossible.

## Latest facts from this discussion

- Gateway retention code was pushed and deployed from
  `2aa2a57d0f9e9472d0dc7ac2023adde6dcb8b98b`. Deployment documentation checkpoint:
  `e0bfc1a6793a84cd9727504d169c32e18118130b`.
- Previously recorded verification: 187 tests passed; production-runtime retention,
  lifecycle and integration checks passed; deployed hashes matched source. These
  are earlier results, not tests rerun while saving this handoff.
- Existing age-based cleanup is opt-in and was verified OFF. Its authenticated
  maintenance result was `enabled:false, examined:0, expired:0`. The new strict
  80% pressure policy has NOT been implemented or tested in this discussion.
- Both actual worker-origin native tools returned UNAVAILABLE at the latest
  recorded probe. They have NOT been repaired in the subsequent interrupted turns.
- The owner saved R2 settings in the existing project-root `.env`, then supplied
  the Account ID there. A fresh check found all four expected names populated:
  `HG_R2_ACCOUNT_ID`, `HG_R2_BUCKET`, `HG_R2_ACCESS_KEY_ID`,
  `HG_R2_SECRET_ACCESS_KEY`. Account ID matched the expected 32-lowercase-hex format;
  file mode was 0600. No values were printed.
- Using those explicit credentials and the fixed standard account endpoint,
  read-only S3 `HeadBucket` returned 200 and `ListObjectsV2` returned 200. The
  MaxKeys=1 listing returned KeyCount=0 and IsTruncated=false. No object was
  uploaded or deleted. This proves read/list access at that check, not upload,
  delete, playback, deployed configuration or end-to-end acceptance.
- Earlier deployment lacked the four R2 runtime settings. Successful local access
  does NOT mean the running gateway has received them. No credential transfer,
  service restart or R2 activation followed the owner's latest settings update.
- Account ID location: Cloudflare dashboard, R2 Object Storage, Overview, Account
  Details. It is also the identifier preceding `.r2.cloudflarestorage.com` in the
  standard S3 endpoint. It is not a Zone ID, Access Key ID or general API token.
- The owner says Amazon is already authenticated. Only a read-only connection to
  local debug port 9222 was attempted; that connection failed. No conclusion about
  the actual browser's Amazon authentication follows from that failure, and no
  hosted skill update was deployed in these turns.

## Agreed product and security decisions

- Native C++ on the Mi phone remains the observer, analysis authority, durable alert
  producer and authenticated read-only MCP server. Node/Python are adapters only.
- Another worker owns detection and recording. Preserve that worker's files,
  devices and in-progress work. Storage and Alexa work must coordinate at their
  existing interfaces, not replace the recording implementation.
- Store only short, genuine detection-triggered clips in a private R2 Standard
  bucket. No continuous recording/upload, public bucket, public development URL,
  Oracle media relay, or private YouTube substitution.
- Any authenticated Google outdoor reviewer may read the explicitly approved
  outdoor records and clips. Google login does not grant private household,
  inventory or management access. Default role remains VIEWER.
- Portal video list defaults to the current day in Asia/Kolkata, supports date
  ranges and pagination, and preserves the selected event through login. Retained
  event metadata must remain readable after its clip is deleted.
- Alexa speaks a concise observer-derived summary and supplies a stable authorized
  portal link. The accepted lane is ASK request/response, not unsolicited Echo
  announcements. An Alexa app card is not proof of physical Echo delivery.
- Authorized copying or screen recording is accepted. Short-lived signed media
  URLs are bearer capabilities; do not place credentials or signed media URLs in
  Alexa cards, logs, source control or permanent event links.
- Authentication remains cookieless: no tokens in localStorage, sessionStorage or
  tracking cookies. Simulations remain visibly synthetic and isolated from live
  camera/event stores; producer routes reject synthetic clips.
- Free R2 allowances are not an unlimited free plan or a billing cap. Published
  pricing lists 10 GB-month Standard storage, 1 million Class A and 10 million
  Class B operations per month. Storage uses average daily peaks; deletion does
  not erase an earlier daily peak. Byte guards cannot guarantee zero charges for
  account-wide storage, other buckets or request traffic. See official pricing:
  https://developers.cloudflare.com/r2/pricing/ .

## Operating boundaries

- Continue in the existing main checkout under `.worktrees/alexa-main`; no new
  branch/worktree, scattered directories, broad staging or unrelated refactoring.
  The explicitly named root `.env` is authorized configuration input only.
- Never print or commit credentials, environment files, private URLs, camera
  identifiers or media. Keep configuration protected and use bucket-scoped access.
- Preserve the working phone/VPS private network. No workstation VPN enrollment,
  new public MCP listener, firewall/routing/key changes, or changes to unrelated
  mail services or the explicitly excluded second VM.
- Keep MCP application authentication and read-only permissions even within the
  private network. Do not weaken authorization to make connectivity tests pass.
- Test a fix, commit scoped source, verify its push, deploy the exact tested source,
  then read back the deployed target and exercise the real consumer. Never
  force-push or report an external write as successful without readback.
- Do not rerun completed full suites gratuitously. Run the focused regression and
  relevant acceptance checks for the actual change.

## Evidence required to close tasks

For each task, record changed paths, source SHA, actual test command/result,
deployment readback, consumer result, remaining limitations and the next task.
Until that evidence exists, retain PENDING or BLOCKED rather than DONE.
