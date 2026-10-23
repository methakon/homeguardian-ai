# P1 — Android → C++ → MCP physical acceptance

Checkpoint: 2026-10-23 (IST). Verdict: **NOT ACCEPTED / IN PROGRESS**.

The physical Mi A3 can acquire and process real camera frames. The complete
physical sensor → C++ detection → durable alert → MCP readback chain has **not**
passed acceptance. Host integration tests using Android stubs do not close it.

This report records an isolated acceptance candidate, not a deployment or merge of
that candidate into `main`. Earlier statements that no physical capture had run
are superseded by the measurements below. The owner confirmed a non-private scene
and subsequently approved a temporary, certificate-authenticated private-LAN
inlet for the Wi-Fi test.

## Capability and acceptance matrix

| Stage | Result | Measured evidence / limitation |
|---|---|---|
| Separate sensor app installed on physical Mi A3 | PASS | `org.homeguardian.harness` installed and launched; it does not replace `org.homeguardian.edgeservice` |
| Real camera acquisition and bounded processing | PASS | Camera permission granted through the normal OS dialog; 320×240 frames processed with no geometry skips or frame drops |
| Microphone | NOT RUN | Audio remained off and sample count remained zero |
| Existing EdgeService preservation | PASS for this attempt | The same foreground-service process remained alive before and after the checks; its app and credentials were not replaced |
| Existing on-phone C++ engine and MCP | PASS for current read-only operation | Live handset self-probe at 02:17:34 IST succeeded in 51 ms: authenticated initialize, tool discovery and status call; engine reported `Running`, `read_only=true`, `capture_enabled=false` |
| Android-compatible test keystores | PASS for loading | On-phone probe reproduced failure for modern PKCS#12 files, then loaded one entry from each compatible device and CA store |
| Native C++ ingress setup | COMPONENT VERIFIED | Real ingress started with mTLS, an isolated SQLite database and an explicit `motion-activity` evidence allowlist; this does not prove it accepted a physical observation |
| Native MCP and audit baseline | COMPONENT VERIFIED | Authenticated initialize and `get_alert_summary` returned `alert_count=0`, `alert_ids=[]`, `read_only=true` against the isolated database |
| Android observation acknowledged by C++ | NOT VERIFIED | Recorded attempts had `sent=0`; later candidate corrections have not yet completed a successful live delivery |
| Physical change → persisted alert | NOT VERIFIED | No physical alert was committed; latest database check returned zero alerts |
| Exact alert returned by MCP after restart | NOT VERIFIED | There is no accepted physical alert id to correlate and recover |
| Wi-Fi / USB-disconnected operation | NOT VERIFIED | Wi-Fi route and test server certificate were checked; unstable USB prevented completing the phone's endpoint configuration |
| Continuous deployment of the new physical pipeline | NOT VERIFIED | The acceptance hub was host-side and temporary. The separate existing on-phone EdgeService and MCP are running, but this does not close the physical-alert gate or prove unattended reliability |

## Actual physical observations

### Existing on-phone runtime versus the acceptance hub

A fresh read-only check at 02:17 IST on 2026-10-23 confirmed the existing Android
foreground EdgeService is running on the Mi A3. Its authenticated on-device MCP
self-probe completed initialization, discovered `get_homeguardian_status` and
`get_alert_summary`, and successfully called the status tool. The latest observed
probe completed at 02:17:34 IST in 51 ms and reported:

```json
{"capture_enabled":false,"read_only":true,"state":"Running"}
```

This verifies the existing on-phone engine and read-only MCP operation, not merely
an installed APK or a host listener. It does not verify current sensor capture,
physical detection, alert creation, alert retrieval or recovery after restart.
The temporary host-side acceptance servers described elsewhere are a separate
runtime; stopping them did not stop the existing phone service. This check did not
restart, reconfigure or replace that service or read its credentials.

A quiet run on the real handset produced these scalar/counter diagnostics. These
are excerpts from an executed run, not synthetic detector inputs:

```text
counters video=on audio=off frames=339/339 dropped=0 frame_bytes=39052800
samples=0 frames_processed=339 audio_windows=0 obs_emitted=90 obs_dropped=0
proc_skip_geom=0 proc_skip_below_min=0 proc_skip_above_max=0 last_geo=320x240
counters transport queued=6 sent=0 refused=0 dropped=0 permanent=0
observation: motion_fraction=0.0000 motion_mean_delta=0.0044 pixels=76800
```

Acquisition, processing and numeric feature emission are therefore observed.
Acknowledged delivery is not: the transport counters must not be replaced by the
frame/emission counters. No sustained physical-motion acceptance run was completed.
No detector threshold or fusion threshold was lowered to manufacture an alert.

## Defects found and candidate corrections

These changes were made in the isolated acceptance candidate. A report on `main`
is not evidence that its source or APK changes have been merged into `main`.

1. **Missing network permission.** The sensor manifest lacked
   `android.permission.INTERNET`, required even for an authenticated loopback
   socket. The candidate declares it without changing media-consent gates. Three
   manifest regression tests passed in the experimental working tree.
2. **Producer/receiver vocabulary mismatch.** JNI camera `motion_fraction` is mapped
   to wire source `motion` and type `motion-changed-fraction`. Its initial state is
   `unknown`: only the hub's existing detector may decide that activity changed.
   Camera mean-delta remains a local diagnostic rather than an invented wire type.
3. **Keystore compatibility.** Both modern test stores failed on the physical
   Android runtime with `NoSuchAlgorithmException` for the PBKDF2
   `SecretKeyFactory`. Rewrapping the same test certificates in Android-compatible
   PKCS#12 stores produced `device: LOADED entries=1` and `ca: LOADED entries=1`.
   This changes file wrapping, not the TLS certificate identity or endpoint
   verification. Credentials were imported through the app's own import action;
   the staging files were removed after import.
4. **Display summary used as identity.** The native identity API returns a display
   summary. The candidate extracts and validates only the exact opaque node id for
   the transport. Eight identity contract checks passed; malformed identities are
   refused rather than normalized into valid ones.
5. **Provenance vocabulary mismatch.** JNI `real_device` is translated to wire
   `real`, while explicit `synthetic` stays synthetic and missing/unknown labels
   are rejected. The hub still derives provenance from authenticated registry and
   consent facts. Before this correction, the actual peer returned
   `kind=error reason=bad_json`; successful delivery after the correction is still
   unverified.
6. **Foreground usability.** The candidate keeps its own visible monitoring view
   awake without changing device settings. Leaving the activity still releases
   capture. This is not a screen-off/background-monitoring guarantee.

Java was freshly compiled against the installed API 27 platform jar and packaged
with an existing local armeabi-v7a native library. A full fresh native build was
not completed under the host's severe disk pressure. The reused native library
has no embedded source-revision record; retain that provenance limitation.
An earlier candidate was pulled back from the installed package and its hash
matched its input APK, but that check must be repeated for the final accepted APK.

## Remaining blockers and diagnostic results

- **Unstable USB.** The host kernel repeatedly reported `USB disconnect`,
  `Device not responding to setup address`, and
  `device not accepting address … error -71`. ADB transport identifiers changed
  while the existing EdgeService process remained alive. This is observed USB
  re-enumeration, not proof of an application crash or a server restart.
- **Wi-Fi switch authorized, not accepted.** The owner authorized one temporary
  mTLS-protected ingress on the workstation's private LAN interface. The phone's
  route used Wi-Fi and the server certificate verified for the selected private
  endpoint. MCP remained loopback-only. USB dropped during the phone's UI setup;
  no Wi-Fi observation-delivery result is claimed.
- **Replay durability remains open.** An earlier shutdown reported
  `cannot replace the replay state: No such file or directory`. Later orderly
  stops completed, but an empty replay file or a clean stop is not proof that an
  accepted watermark survives restart. Test a real accepted sequence and alert.
- **No alert to recover.** The latest isolated-database query returned zero alerts.
  No fabricated event, synthetic frame, hand-inserted alert or unrelated historical
  alert was substituted for physical acceptance.

## Scope, cleanup and privacy

The source snapshot used for the candidate was
`release/hackathon-2026-mcp-candidate`; experimental work was kept separate from
other workers' worktrees. The existing EdgeService's production credentials were
not read or changed. Only fresh test material was provisioned for the sensor app.

At this documentation checkpoint the acceptance-owned C++ ingress and MCP server
were stopped and both test ports were verified closed. The sensor app remains
installed; the last handset logs reported capture off. The original EdgeService
was rechecked alive with its unchanged process id. No persistent system service,
cloud resource or public listener was installed.

Real LAN addresses, device identifiers, keys, passwords, tokens, certificate bodies
and media are deliberately excluded from this public report. Local test logs and
credentials are not committed. Permission for a private scene or a temporary
listener does not authorize publishing sensitive runtime details.

## Exit criteria before P1 may be marked complete

1. Stabilize the device-control path or finish configuring the authorized Wi-Fi
   endpoint. Verify the actual installed candidate and its identity/signature.
2. Observe increasing real frame counters **and** fresh accepted observation ids
   at the C++ boundary, with correctly derived real provenance.
3. Establish a quiet control, then an operator-driven sustained change. Trace its
   actual observations through the unchanged detector, fusion and alert policy.
4. Correlate the exact persisted alert id with `get_alert_summary` through a genuine
   authenticated MCP session against the same database.
5. Restart only the test runtime, reopen the same database, establish a new MCP
   session and read that same alert. Verify replay protection on accepted sequences.
6. Exercise explicit stop and consent withdrawal, verify frozen counters and camera
   release, and confirm no post-stop delivery. Verify the original EdgeService is
   still intact and remove only acceptance-owned resources.

Until those checks pass, P1 stays **NOT ACCEPTED**, regardless of green host tests,
successful APK installation or real camera counters.
