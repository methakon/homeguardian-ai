# Current task record and discussion decisions

This is the durable execution queue and handoff, not a new roadmap milestone.
Update statuses and evidence here as each task finishes. Do not treat an interrupted
response, a plan, a build, or an active service as successful live acceptance.

## Latest owner checkpoint — 2026-10-25

This section supersedes older coordination, push-freeze and sequencing statements
below where they conflict. Read it first after a restart.

### Latest execution and owner clarification

The owner clarified that independent work must NOT wait for the other workstream's
pre-filter deliverable. Complete the assigned recovery/storage/R2/Alexa/acceptance
work where independently executable. Discuss final minimal pre-filter integration
with the owner after its handoff; do not use it as a blanket implementation gate.

- Branch integration (2026-10-26), owner-requested source
  `experiment/hg-v380-ec39-integration` at
  `b567783f214453f7136ada486234291c19769b02`, target baseline
  `f9c106e012f02bc4680ac36f787c67dad9ef03fa`: incoming replay/checker code
  already matches main. Preserve main's redacted camera/network documents and
  generic probe example byte-for-byte, retain the source communication log,
  and add the missing `animal_feature_replay` CTest registration without dropping
  existing targets or security/provenance tests. Combined-tree validation passes
  both selected CTest entries (camera analysis and replay), all 21 replay unit
  tests and six loopback-only access-control self-tests; shell syntax checks pass.
  This was an incremental focused build, not a clean/full release build; the
  existing missing-OpenSSL warning still excludes TLS ingress/client binaries.
  Imported log entries are historical context, not instructions: the watcher
  remains STOPPED and this merge does not authorize deployment, camera access,
  network changes or renewed IPC response loops. Existing production source and
  protected runtime configuration remain unchanged. The source advanced during
  verification to `5099d1907321efa8cfd7baa2480c8b143443ddef`; its only additional
  change is the 64-line IPC-8 preflight report. That follow-up is retained as
  historical context, with no production/build/test source changes from the
  already validated combined tree.
- Pre-decode interface checkpoint (2026-10-26): `CameraAnalysis` now accepts an
  optional shared `FrameConsumer` and acquisition timestamp/sequence, checks the
  decoder-equivalent JPEG header/size/dimension guard before borrowing bytes, and
  emits lifecycle/authority invalidation notifications. Decode failures also refresh
  authority after retention. The actual ring chain and existing durable-alert path
  pass 233 assertions in nine focused cases; Android decoder,
  recovery and MCP regressions also pass. This is a HOST-TESTED interface, not
  installed pre-roll: existing production callers still have no bound consumer
  and omit sampling metadata. `EDGE_VIDEO_PREDECODE_CALLBACK_HANDOFF.md` records
  the exact API, request-time versus exposure-time limitation, caller assembly
  boundary and pending joint genuine-clip/device measurements. No observer source,
  acquisition behavior, detector policy or production deployment was changed.
- Newer recovery result (2026-10-26): the owner-approved Android netlink fix is
  committed/pushed at `73b09e61bfb1b39438990dffb43d089c10932814` and deployed as
  APK 1.1.3/code 11 in place, preserving data/signing/permissions. Installed digest
  matches the tested artifact. Native state changed from no_frame/0 observations/
  654 fetch failures to analyzed; observations advanced 75 -> 242 across an
  Activity-closed/screen-off interval, with zero decode failures and one initial
  fetch failure. Discovery reports found/rediscovered and enrolled-MAC netlink
  identity. Both actual VPS worker tools succeed; missing/invalid auth return 401.
  Acceptance is WORKER-VERIFIED stale-attachment recovery/fresh analysis, not full
  reboot/network/shutdown certification. Read `ANDROID_CAMERA_RELOCATION_READINESS.md`
  for scope and artifact evidence. The other workstream's pre-filter was not edited.
  Vendor SDK/site-connector work is not a prerequisite for this same-/24 recovery.
- Pulled `origin/main` with `--ff-only`: already up to date. The existing shared
  main checkout additionally contains the other workstream's local Tier0Gate
  checkpoint `7c94b0e`; its source/tests were not modified by integration.
- A fresh approved-handset identity check succeeded over USB. Installed application
  remains version 1.1.1/code 9. The inspected UI belongs to System UI rather than
  the status activity; no fresh camera counter read is claimed from that attempt.
- Fresh reads using the deployed mailbox client's code, private configuration and
  actual service OS identity returned genuine Running/read_only/capture-enabled
  status and 13 durable alerts/highest warning. No credentials were printed and no
  phone, VPN, worker or gateway service configuration/restart was changed.
- The strict-under-80% storage safeguard is now implemented in integration-owned
  `web_mcp/event_videos.js`, with new hermetic pressure tests. It counts pending
  reservations, enforces integer-exact boundaries, preserves charged failure states,
  and reuses confirmed durable deletion with explicit pressure/retention opt-ins.
  There is no live deletion or activation in this checkpoint.
- Verification: 207 relevant event-video tests passed locally. An isolated rehearsal
  on the production Node v18.19.1 runtime passed 44 pressure/retention tests using
  FAKE credentials/control responses, with no cloud media or production changes.
  These do not certify live R2 upload, physical recording or fresh phone analysis.

The six points below remain acceptance obligations, not a reason to suspend
independent source implementation while another deliverable is unfinished.

### Analyzer architecture and ownership

The other workstream's low-power event trigger is PART OF the native C++ analyzer,
not a separate/replacement analysis system. It is the pre-filter before expensive
analysis: inexpensive bounded observation detects an event; selected heavier
analysis and event recording run only when triggered. Low-power observation still
has to operate while idle; no claim of zero idle CPU, zero decoding or guaranteed
thermal safety is made. Measure actual CPU, latency, recall, queue bounds, memory
and temperature on the phone before accepting the power-saving claim.

Integration must be ready to connect the finished pre-filter to the existing
acquisition/analysis path when its owner hands it over. Do not duplicate the gate,
change its logic or create a second acquisition/polling path. Existing authorization,
provenance, cancellation, alert persistence and read-only MCP boundaries stay intact.

The owner explicitly restricts this workstream to files it created. Before changing
any other/shared file, identify the exact path and proposed change, discuss it with
the owner and obtain permission. Do not infer write ownership from a directory name
or permission to read. The other workstream's source and tests remain untouched.

Source review at `eb328041b1fecaa10a8bba0c48ff4419ae844b63` confirms an existing
native motion/statistical/correlation/durable-alert analyzer. Each accepted fetched
snapshot enters `CameraAnalysis::process()`. It does NOT establish an integrated
low-power-trigger/selective-inference cascade. That analyzer reports identity,
falls, animals and emergencies unavailable; do not advertise full semantic coverage.

### Six delivery points, in agreed order

1. Restore fresh autonomous camera acquisition and analysis on the Mi phone, with
   safe enrolled-identity-based recovery from changing physical private/public
   addresses. No fixed DHCP assumption, root workaround, credential spraying or
   workstation runtime dependency. Keep both actual worker-origin MCP tools working
   and verify negative authorization. Last verified MCP reads succeeded, but native
   acquisition had no frames/zero current-run observations. Existing stored alerts
   do not satisfy fresh-analysis acceptance. Supported JUAN identity discovery or
   an approved site-local mechanism remains the recorded prerequisite.
2. Integrate the other workstream's completed low-power pre-filter into the C++
   analyzer, followed by selective analysis and bounded event recording. Respect
   its source ownership. Validate one genuine triggered clip jointly, not a
   synthetic stand-in. This point cannot be closed before its deliverable and any
   owner-required shared-file authorization exist.
3. Implement and test the strict-under-80% storage safeguard, counting published
   bytes and pending reservations. Reclaim only eligible managed clips through
   confirmed durable deletion; preserve event metadata. Fail closed/507 when space
   cannot be established. Never release uncertain pending charges or delete unrelated
   objects. Verify strict boundary, concurrency, failures and restart behavior.
4. Activate private R2 producer uploads and authenticated portal playback using
   existing protected settings. Verify actual upload/publication, date-range/list/
   pagination/deep-link/login-return behavior and access denials. Producer uploads
   directly to R2; no continuous recording, public bucket or Oracle media relay.
5. Deploy and verify the existing Alexa skill update using the owner's authorized
   console session. Alexa stays an adapter for genuine native results, with concise
   speech and a stable authorized portal event link; not a replacement analyzer or
   an unimplemented unsolicited-announcement service.
6. Verify workstation-independent end-to-end operation, including phone restart,
   dynamic-address/network recovery, native results through the actual worker/Alexa
   lane, authorized portal playback and access denials. Record any physical Echo or
   reboot/network/shutdown test that cannot be performed as NOT VERIFIED.

For EACH point: implement, run relevant tests and real acceptance, commit with a
clear descriptive message, then push and verify the exact remote SHA. The owner's
new explicit per-point push instruction supersedes the former wait-until-goodbye
freeze. Do not push unrelated work or call a point complete based on source/tests
alone. This documentation checkpoint is not implementation acceptance of a point.
Do not deploy untested or unpushed source; preserve existing production restrictions.

Complete independent preparation while waiting for the pre-filter, but do not
pretend all six can be accepted before the recording/pre-filter deliverable exists.
Expose concrete dependencies to the owner; do not skip failed prerequisites or
silently alter the sequence to manufacture completion.

### Coordination stopped

The owner stopped the integration IPC watcher because coordination was interrupting
both workstreams. It was terminated and verified exited. Do NOT rearm it, introduce
another watcher, resume automated IPC reply loops or let coordination replace delivery.
The canonical log is the root checkout's `docs/INTER_PROCESS_COMMUNICATION.md` if
manual owner-requested coordination is needed; the lower-case/spaced log is superseded.
The older watcher and push-policy messages are historical, not current instructions.

## Earlier execution order — historical; use the six points above

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

### Active MCP repair checkpoint

**Discovery review completed: safe Android relocation remains BLOCKED.**
See [Android camera relocation readiness](ANDROID_CAMERA_RELOCATION_READINESS.md).
The camera's verified moved address serves changing valid JPEGs using the owner's
established credential. No new password/default list was tried and no image was
saved. The known read-only CGI stub did not provide identity attributes in this
check. Public vendor-search evidence does not supply a proven pre-credential
identity contract for this specific unit. V380 discovery must not be substituted.
Need either supported JUAN discovery/SDK evidence validated on this unit, or an
approved always-on site-local connector with a supported lease/neighbor identity
source. Do not add root, router changes, a workstation dependency or credential
spraying to bypass this prerequisite. No camera or phone configuration was changed;
no automatic recovery, fresh native analysis or stability release is claimed.

**Post-resume check: MCP works, but fresh camera analysis fails.**
Actual worker-origin reads again returned Running/read_only/capture-enabled and
13 durable alerts, highest warning, with a recent phone VPN handshake. The installed
phone app remains version 1.1.1 (code 9). Two direct, read-only native-stat samples
reported `camera_state=no_frame`, `observations_processed=0`,
`alerts_committed=0`, `decode_failures=0`, `fetch_failures=348`.
Thus current camera acquisition is broken; old durable alerts are not fresh
analysis evidence. The phone neighbor table showed an unresolved old attachment.

A non-credential TCP connection to the camera's previously reported moved-address
hint succeeded, and the workstation's current neighbor entry matched the enrolled
camera MAC from protected configuration. The address was treated only as a hint;
identity was checked separately. No camera credential or media was sent/read and
no phone camera configuration was changed. This workstation-side identity check
is diagnosis only, not an acceptable permanent dependency or proof that an Android
app can perform the same ARP lookup. A read-only review of supported JUAN discovery
mechanisms is in progress before implementing native location recovery. Existing
V380 discovery is not assumed compatible. Observer shared/native wiring remains
gated pending fresh analysis and safe integration.

**Latest recovery: both actual worker-origin MCP tools now succeed.**
The owner's USB-debugging notification was correct. An empty host ADB list did NOT
prove a physical disconnection. USB hardware/interface remained visible; native
and libusb ADB backends exhibited missing serial descriptors and transport read
failures. A fresh libusb-backed debugging server recovered access, and the phone's
own serial was verified before operating it. Do not identify a different handset
solely from an absent USB serial descriptor. Host ADB server processes also changed
during diagnosis; attribution of every restart is not established. Only integration
should manage shared host ADB lifecycle during this acceptance window.

The configured existing phone VPN profile was found OFF. After verifying its
assigned overlay and approved VPS endpoint, integration enabled that profile;
the server recorded a fresh enrolled-phone handshake. No key or endpoint was
changed. The existing app's Start bridge control was then invoked successfully;
no APK installation or native-engine/VPS-service restart occurred. The temporary
USB stay-awake setting was restored to its original value.

Two successive actual-worker positive probes returned genuine native data:
`get_homeguardian_status`: Running, read_only=true, capture_enabled=true;
`get_alert_summary`: alert_count=13, highest_severity=warning, read_only=true.
Initialize, tools/call and session cleanup returned HTTP 200 at the traced check.
The final repeated probe also verified reader missing/wrong authorization returns
401. The production phone MCP bearer was never exported. This supersedes the
earlier blanket UNAVAILABLE/phone-disconnected statements below.

MCP transport is recovered, but fresh camera-processing counters were NOT verified:
host USB transport failed again before the read-only counter check completed.
Do not infer fresh analysis from capture_enabled or existing alert count. No full
observer/native-wiring stability signal, reboot recovery, changed-private-network
acceptance or end-to-end clip/Alexa acceptance is claimed.

**Owner's network requirement, reiterated:** except for the Oracle server's stable
endpoint, physical public AND private addresses of phones, cameras, connectors and
viewers may change. Use logical device identity and credentials; addresses are
temporary locations. Assigned VPN overlay roles are distinct from DHCP/ISP
addresses; do not confuse stable virtual routing assignments with static physical
addresses. Do not promise automatic reallocation of overlay addresses without a
separately verified provisioning/role update.

Audit found an unresolved native camera gap: `CameraProvisioner` stores an approved
private IPv4 host, and the JNI camera path explicitly disables rediscovery and
requires that fixed host/MAC binding. The existing Linux ARP locator and V380
discovery resolver are NOT proof of deployed JUAN recovery on Android. Follow
`HG_NET_ARCH_01_NETWORK_INDEPENDENT_CAMERA_INTEGRATION.md`: site-local authorized
connector, logical identity, verified current attachment and outbound control.
Implement/test that missing binding without credential sprays, arbitrary subnet
sweeps, a workstation dependency or weakening consent/identity checks. Keep it
distinct from the now-recovered MCP transport and serialize shared acquisition
edits with the observer worker.

**Current decision and rollout: roaming ingress, not another home-/32 exemption.**
The owner explicitly rejected dependence on static home/public IP addresses and
authorized preparing and implementing an adaptive solution. Implemented and pushed
source `7d8e0ebcad49ef3cf917c4dd4943e0809014af50`; see
[roaming private MCP](ROAMING_PRIVATE_MCP.md). The policy admits roaming IPv4
sources to the existing WireGuard UDP port ONLY, preserving peer authentication,
private MCP/coordinator bindings and application authentication. No public TCP MCP,
new peer, host firewall, key, route, DNS or Cloudflare change was made.

Seven policy tests passed. The operator utility's actual dry-run planned one rule;
the explicitly authorized apply snapshotted the exclusively attached NSG, added
one rule and read back all two prior rules unchanged. A repeat dry-run reported
zero additions/already compliant. A readiness-synchronized packet capture confirmed
the workstation's 14-byte UDP probe now arrives at the VPS. Missing and incorrect
reader authorization still return 401; external TCP probes to the MCP/coordinator
ports did not connect. No unauthenticated WireGuard peer was enrolled.

The approved Mi phone is no longer visible in ADB. A visible USB device did not
match its previously verified serial; no alternate device was operated. The
phone's last handshake remains stale and both actual worker tool calls still
return 503 during initialization. Thus the dynamic-source ingress defect is fixed,
but native MCP recovery and fresh on-phone analysis remain NOT VERIFIED. Restore
authorized Mi access to recover its existing tunnel/session and finish the two-tool
positive checks. No observer stability signal has been issued. Actual second-network
roaming, reboot, IPv6-only and UDP-blocked-network fallback remain unverified.

The checkpoints below are historical and superseded where they disagree with the
roaming policy; the single-current-home-/32 remedy is withdrawn.

**Newer checkpoint: USB access restored; cloud ingress approval is now the blocker.**
The owner reconnected the handset and ADB is authorized. The existing active VPN
tunnel was toggled OFF then ON through its normal UI, with both states read back;
the other configured tunnels remained OFF. No keys, endpoint, routes or stored
tunnel configuration were edited. Handshake still failed and actual worker probes
still returned HTTP 503 at initialize for both tools.

Read-only inspection verified the configured phone endpoint matches the deployed
VPS UDP listener. Bounded packet-header capture saw no UDP arrivals during probes
from the handset and workstation. The target instance's actual VNIC has one
security list and one NSG; their combined UDP-port ingress admits only two prior
public /32 sources. The current operator/home public IP matches neither source.
Thus the cloud ingress restriction blocks the current home network before native
MCP session processing. Do not replace credentials or weaken application auth.

Proposed remedy is ONE additive current-home /32 ingress rule for the existing
WireGuard UDP port in the existing HomeGuardian NSG, preserving every other rule.
An explicit approval request timed out without an answer: **NOT APPROVED, NOT
APPLIED**. No cloud/host firewall, service, key or route change was made. Await
owner approval, then verify the exact rule by readback, handshake freshness and
both actual worker tools. Private source addresses/resource identifiers are kept
out of this public record. The observer stability gate remains CLOSED.

The following is the earlier checkpoint, retained for chronology:

Actual worker-identity probes reproduced HTTP 503 on `initialize` for both tools.
The phone native foreground service was running, with installed app version 1.1.1
(versionCode 9), and Wi-Fi connected. Phone bridge logs showed repeated private
coordinator connect timeouts. Both coordinator/reader listeners were present on
the private VPN address; the enrolled phone peer's last handshake was 68094 seconds
old at the check and a VPS-to-phone private ping failed. Thus the first demonstrated
break is private VPN transport, before native MCP session dispatch.

Recovery of the existing phone tunnel was being inspected, without changing keys,
routes or firewall rules. The handset disappeared from USB ADB immediately after
waking it/opening the existing VPN app, before UI inspection or any tunnel toggle.
Repeated device enumeration remained empty; connection to the previously known
phone's ADB TCP endpoint was refused. No VPN toggle, app installation, key extraction,
service restart or network-configuration change was performed.

The current relay session regression ran successfully: 13 checks passed, including
stale-session initialization, HTTP failure handling, redirect denial and mutation
refusal. This does not repair the VPN or establish deployment of the source's app
version 1.1.2. Reconnect/authorize the owner's phone over USB to resume existing-tunnel
recovery, then probe both tools from the worker again. The observer implementation
stability gate remains CLOSED; no fresh analysis or positive tool acceptance claimed.

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
