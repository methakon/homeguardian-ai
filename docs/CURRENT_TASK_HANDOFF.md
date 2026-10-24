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

---

## Design discussion and decisions — edge video event detection (2026-10-24)

Recorded here as discussion and decision history. This is **not** a re-ordering of the
execution queue above: the owner-ordered tasks (restore both phone MCP tools, storage-pressure
safeguard, R2 activation, Alexa-hosted Code update) remain the execution order. Nothing in this
section is implemented.

### What was discussed

A continuous video event-detection capability for the edge node, triggered by the observation
that no single inference interval works: two minutes is blind, one to two seconds thermally
throttles the device, and five seconds still misses a subject that crosses the frame in 3-4 s.
The conclusion was that the interval is not the variable to tune — the architecture is. Cheap
continuous observation must be decoupled from expensive event-gated analysis, and the analysis
must be able to examine frames captured *before* it was asked.

### What was decided

Five decisions, recorded in full as ADR-020 … ADR-024 in `docs/DECISIONS.md`:

| ADR | decision |
|---|---|
| ADR-020 | Video event detection is trigger-driven, not scheduled. Asymmetric gate (one strong hit fires immediately; weak motion needs 2 of the last 3), latched 0.8-1.5 s past the last hit, event timestamp taken on the **first weak hit**, Tier-0 never degraded to relieve heat |
| ADR-021 | The edge node stays a native NDK/Bionic `arm64-v8a` runtime. No Termux, no PRoot/glibc userland, no extra hosting package |
| ADR-022 | Camera ingestion is network-side. The edge service holds no CAMERA or RECORD_AUDIO permission, so continuous monitoring never activates an on-device sensor |
| ADR-023 | The pre-event ring retains compressed packets, not decoded frames — a ~350x memory reduction for the same window, and retention must exceed the GOP so a keyframe is always inside it |
| ADR-024 | Where design references conflict, the later and stricter one governs, and its correction is applied rather than averaged |

### Work plan and overlap boundaries — read before writing any code

`docs/EDGE_VIDEO_IMPLEMENTATION_PLAN.md` is the executing plan for this workstream, and its
purpose is to prevent duplicate work. Its finding, recorded so it is not rediscovered: **the change
metric at the centre of Tier-0 already exists** — `MotionFrameFeatures::changed_fraction` in
`src/core/BoundedFeatures.*`, host-tested, allocation-free, under the DONE W-02 workstream — as do
the discriminators the design listed as controls to build: illumination/auto-gain rejection
(`gain_correlation`), the compact-versus-global distinction (`peak_cell_share`, `spread_radius`)
and the changed-mass location that serves as the motion ROI (`centroid_*`), all validated on real
frames in `docs/CHANGE_CHARACTER_SEPARATION.md`. Tier-0 is therefore a composition of existing
components plus a gate, a ring, a bounded queue, a thermal controller and a fetch path — not new
image processing.

The plan also fixes the roadmap boundaries, so that two workstreams do not build one thing:

* **W-02** (DONE) owns the feature descriptors. Reuse; do not fork or edit.
* **D-05** (TODO) owns the real-media observation path into the existing pipeline. Tier-0
  delivers through that contract; it does not define a parallel one.
* **E2-02 / E2-05** (TODO, Phase E2 deferred) own adopting OpenCV and ONNX Runtime as project
  capabilities. **This workstream does not adopt a runtime as a side effect** — Tier-0 as
  specified needs neither.
* Thresholds, detector allowlist entries and `src/net/*` are out of bounds; new detector ids
  arrive ADVISORY.

Sequenced stages V-0 … V-5, with V-0 (measure the existing extractor on the device) before
anything is built, and V-0…V-2 delivering a working event detector with no neural network and no
new dependency at all.

### What was verified on the physical device (read-only)

- The handset is connected and usable: adb reports it in `device` state. Identity was taken from
  corroborating device properties, **not** from the USB descriptor string, which mislabels this
  device (FRICTION_LOG Incident 80).
- Idle capacity is ample: load 0.00, 713% of 800% CPU idle, ~3.4 GiB RAM available, 93 GB free.
- **The binding thermal constraint is 56 °C** on the skin/quiet zone — far tighter than the
  generic single-board-computer figure previously assumed, and the headroom that any Tier-1
  budget must be derived from (FRICTION_LOG Incident 83, and §5.2 of the design document).
- **Camera acquisition from the node works**: reachable on the same subnet, HTTP fetch returns
  200 with a valid JPEG payload in ~28 ms.
- Two previously recorded blockers did **not** reproduce — the adb transport is healthy and
  device-to-camera reachability is fine.
- **What is running: the edge service only**, as a foreground service, loopback-bound, answering
  `401` to anonymous callers. The harness is installed but not running and holds no sensor
  permission. Vision runtime, model and capture ingestion are absent.

### What was explicitly not done

No camera or microphone on the device was activated. No device state was modified — every check
was read-only. No vision code, runtime, model or dependency was added to the tree. No listener,
permission or package was introduced. No deployment, restart or configuration change was made to
any running component.

### Open items

Full list with reasons in `docs/EDGE_VIDEO_EVENT_DETECTION.md` §7. The three that gate any
implementation: nobody has written Tier-0 yet; the mutual-TLS ingestion path remains unavailable
on this node until OpenSSL is cross-compiled for Android (plain-HTTP camera acquisition is
unaffected); and autonomous restart plus on-battery behaviour are unproven for a continuous
monitoring role. No design reference used here was validated on the target hardware, so the first
real measurement on the device — not the references — settles the tuning values.
