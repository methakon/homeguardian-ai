# Android edge analyzer update — candidate, not deployed

## Current verdict

**NOT ACCEPTED for handset deployment.** Owner approval for an in-place update has
been received, but the latest workstation check found no ADB device. Installation,
phone alert readback, Android service restart recovery and physical sensor
acceptance have not been executed for this candidate.

The candidate manifest identifies version **1.0.6 / versionCode 7**. A fresh ARM64
native APK build has been started; this document does not yet claim that it built
or that the signer matches the installed package.

## Host verification actually executed

Command:

    HG_EDGE_JOBS=2 python3 scripts/android/run_edge_analyzer_tests.py

Result: **34 checks passed, 0 failed**. The suite exercises the existing
`IngestDetectors -> Pipeline{DetectionFusionRule} -> AlertPolicy -> PersistenceAudit`
components through `src/android/EdgeAnalyzer`:

- sustained synthetic activity creates a SQLite alert with synthetic provenance;
- the persistence read API used by MCP can retrieve that alert;
- quiet observations produce no alert;
- repeated input is suppressed during the tested in-process cooldown;
- unlisted real devices are refused;
- an explicitly listed and consented real-labelled fixture is admitted (this is
  a host fixture, not evidence of a physical sensor);
- unwired detectors and unavailable persistence are explicitly reported;
- accepted/rejected and persisted alert counters reconcile;
- classic fusion remains the library default and does not alert on this fixture;
- destroying and recreating the analyzer preserves the alert count and exact id
  in the same SQLite database.

The initial host run returned **21 passed, 5 failed**. Its shared test configuration
left episode evidence disabled, unlike the proposed explicit phone configuration.
The test configuration now opts into that existing path; a separate classic-mode
control verifies that the original confidence gate is unchanged. No detector or
fusion confidence threshold was lowered.

Additional regression results:

- `python3 -m unittest discover -s tests -p 'test_edge_bridge_*.py' -v`:
  **4 Python tests passed**, including a Java UI behavior harness reporting
  **8 scenarios passed, 0 failed**.
- `bash scripts/bridge_ws_compat/run_route_policy_test.sh`:
  **50 checks passed, 0 failed**.

These results do **not** verify MCP HTTP authentication, notifications, service
recovery or end-to-end delivery on the handset for this candidate.

## Candidate integration boundaries

- Reuses the existing C++ detector, correlation, alert-policy and SQLite components.
- Analyzer owns persistence; the existing native MCP provider is pointed at that
  persistence rather than a separate demonstration database.
- The Android foreground service calls JNI in-process; no ingestion socket is added.
- `EdgeService` remains non-exported. The proposed UI test is explicitly synthetic.
- Native MCP remains loopback-only with required authentication; no VPN listener or
  public listener is introduced by the candidate.
- Existing credentials must be retained. Do not uninstall, clear application data,
  generate a replacement signing identity or export the native MCP bearer.
- Physical sensor input remains unwired and its real-device allowlist is empty.

## Remaining acceptance gates

1. Complete the fresh ARM64 build and JNI export verification; verify signature,
   non-debuggable manifest, ABI, version and unchanged permission set.
2. Complete correctness/security review before installation.
3. Restore authorized ADB connectivity and inspect the installed version/signature.
4. Install in place with the same signing identity, without clearing app data.
5. Execute the labelled synthetic sequence through the app's own operator UI and
   correlate the resulting exact alert id with authenticated MCP readback.
6. Restart the approved phone runtime, reauthenticate and retrieve that same id.
7. Verify the native loopback binding, both authentication layers, rejected
   unauthorized operations and screen-off behavior independently.
8. Keep physical sensor acceptance separate and open until genuine physical
   observations, durable alert creation, stop/consent and recovery are measured.

No source merge into `main` or deployment acceptance is implied by this report.
