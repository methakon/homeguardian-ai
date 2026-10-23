# Pre-final verification — 2026-10-23

Audited main revision: `86f90db9a7fa5c9cb9fb8a73ad08c795e4455dce`.
Verification finished at 23:48 IST. The source revision stayed unchanged and the
checkout was clean throughout the final checks. Subsequent documentation-only
commits do not change the tested runtime.

## Verdict

**PASS for the tested local submission paths. NOT full production or physical
monitoring certification.** All 19 verification groups returned exit 0. Counts
below overlap across suites and must not be summed into one assertion total.

| Area | Actual result | Scope |
|---|---|---|
| Native C++ suite | 76,419 assertions / 789 cases passed | Default host suite; `HG_SOURCE_DIR` points to the audited checkout |
| Canonical judge gate | 29 passed / 0 failed | MCP 2025-11-25, two read-only tools, auth/Origin/session/bounds, coordinator isolation and shutdown |
| Explicit ingress gate | 392 assertions / 6 cases passed | Real loopback mTLS with synthetic observations; included in the judge gate, not the default native count |
| ASK/mailbox contracts | 73 tests passed under each of two SDK dependency environments | Request/response SDK contracts, unavailable handling, transport and worker fixtures; no live AWS requests |
| Android host transport | 45 lifecycle, 43 schema, 27 policy, 59 cross-receiver, 34 TLS, 19 synthetic-vector checks passed | Production Java compiled against Android 34; TLS uses local test peers, not a handset |
| Coordinator/outbound bridge | Security regressions and 46 bridge checks passed | Local synthetic peers and genuine native MCP replies |
| Camera source/relay/gateway/bubble | 113 tests passed, 0 skipped | Fake media and local test peers; physical live-camera tests excluded |
| Worker guard | 12 cases passed | Isolated HOME, temporary ledger/state |
| Worker envelope | 16 cases passed | Author/scope/branch/provenance and refusal controls |
| Worker adversarial suite | Exit 0 | Scope traversal, context and provenance negatives; suite's stated coverage limitations remain |
| Approval consumption suite | Exit 0 | First use, replay refusal and expiry; not concurrent-consumption certification |
| WireGuard preflight | 7 mocked cases passed | No real interface, route or firewall changes |
| Docker boundary instrument | Safe/unsafe self-tests passed | Fixture controls, not a new audit of deployed containers |
| Vocabulary conformance | Exit 0 | Reports missing consent-missing/withdrawal coverage in these LAN fixtures; not a complete coverage verdict |
| Reconnect policy | 15 simulated properties passed | Not physical network recovery |
| MCP browser-client integration | 26 checks passed | Genuine local native server |
| Device dashboard/security | 161 checks passed | Authentication, CSRF, registry, scoped view tokens, signaling and teardown |
| Portal smoke | 38 checks passed | Synthetic uploads, preview/read/delete, limits, traversal refusal, authentication, lockout and unavailable-store behavior |
| Java/coordinator/native integration | 21 checks passed | Genuine Java bridge and C++ server over local sockets; no phone |

Rows are not a one-to-one mapping to invocation groups: the ingress checks are
nested inside the judge gate, the two ASK dependency environments share a row,
and coordinator regressions share the outbound runner. No failed run is counted
as a pass.

Both Python dependency environments passed `pip check`. The scoped submission
claim scanner reported **FAIL=0, REVIEW=0**; this scanner result is not a substitute
for manual claim review. `git diff --check` passed.

## Test isolation and limits

The web suites were executed from an export of the audited tracked files with
fresh fixture configuration, synthetic credentials and temporary stores. They
were not allowed to overwrite the working checkout's registry or operator files.
The Java integration test's hardcoded temporary prefix was redirected at runtime
to the test workspace; its production source and assertions were unchanged.
Native executables came from the current project build; runtime/test sources
match the audited revision. This was not a fresh clean-clone build, sanitizer
rerun, APK build/install, or deployment verification.

All test processes were torn down. The checked local ports 8790, 18880, 18980,
18994 and 18995 had no remaining listeners after completion. No production
service was restarted or reconfigured; no camera or microphone was activated.

## Remaining gaps and claim boundaries

1. **Private owner-camera viewing is incomplete on this revision.** The camera
   source adapters exist, but no caller in `web_mcp/` connects them to the viewer.
   `web_mcp/webrtc_streamer.js` allocates a video track and negotiates signaling;
   passing the dashboard suite is not proof of physical frames rendered in a
   browser. The camera integration workstream must supply that acceptance.
2. **Alert durability is not fail-closed end to end.**
   `src/main_ingress.cpp:275–284` consumes policy admission before checking the
   alert write, and persistence failure is not propagated as delivery failure.
   Cooldown, ingestion ACK and derived-alert persistence need a coordinated fix.
3. **Physical fusion policy must be explicit.** Ingress retains the conservative
   default evidence allowlist unless configured; source detector IDs must be
   explicitly admitted by the intended deployment. No threshold was lowered.
4. **Ingress/MCP stores require explicit alignment.** Their default database paths
   differ. Configuration supports alignment, but this run did not inspect the
   deployed configuration or prove physical ingress data reaches the queried store.
5. **Running is query-service availability, not monitoring health.** The standalone
   MCP status provider does not prove acquisition/detector liveness.
6. **Deployment/recovery limits remain open.** Previous native/mailbox acceptance
   is preserved in `ALEXA_LIVE_PATH_VERIFICATION.md`, with owner-attested voice
   replies distinguished from independent mailbox/native results. This audit did
   not repeat live cloud calls or verify a new deployment. Deployed coordinator
   hardening, stale-session recovery, reboot/network-change recovery and sustained
   reliability remain separate gates.
7. **Release packaging is historical.** The legacy packaging script still checks
   its older security baseline. Its guard was not bypassed; no current-main archive
   or clean-clone certification is asserted. Use an explicit source revision.
8. **Submission delivery remains unverified.** Public video URL, Devpost submission
   confirmation and rendered-entry review are not established by this test run.

## Submission recommendation

Proceed with the real self-hosted MCP server and working classic ASK/mailbox
integration, supported by clearly labelled synthetic detector demonstrations.
Do not claim production Alexa+, validated physical safety monitoring, or working
private live-camera playback until independently demonstrated. No public camera
stream or public MCP listener is required.
