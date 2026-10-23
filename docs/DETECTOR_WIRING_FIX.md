# Production detector wiring (D-03 ingress)

Date: 2026-10-23. State: SOURCE-TESTED / NOT DEPLOYED. Third corrective batch,
re-verified and extended on 2026-10-23 after independent review.

## Defect closed

The optional TLS ingress built a detector set but never fed it: accepted wire
observations went straight to the event pipeline, while the only detector call was
`drain()` plus a sweep that ran solely when a frame arrived. Derived activity and
anomaly evidence was therefore unreachable on the production route, and a device
going quiet could never be evaluated on its own.

## Changes

- `src/net/DeviceIngressServer.cpp`: the accepted-observation sink now records the
  observation the sink actually received, and only after `ObservationIngest::accept`
  reports success and not an idempotent repeat does the server call
  `detectors_.observe(...)`. Refusals, replay/identity/consent/capture failures and
  sink failures never train detectors or refresh liveness. Derived detector output
  continues to the same `sinks_.derived` pipeline.
- Idle availability: a bounded maintenance worker (250 ms) refreshes authorization
  and sweeps availability without waiting for an inbound frame. It waits on a
  dedicated `maintenance_cv_`, because sharing `queue_cv_` with `notify_one()` let a
  wake-up intended for maintenance leave an accepted connection queued.
- Quiet-device delivery is re-authorized at delivery time against the credential
  bound to that device's last accepted evidence, plus paired/revoked/consent/
  capture and consent subject/purpose. Nothing is delivered while authorization
  state is absent or malformed.
- `src/net/IngestDetectors.{h,cpp}`: offline transitions no longer claim
  `Provenance::real` or a fabricated `security_monitor` consent; they inherit the
  provenance and consent context of that device's accepted evidence.
- `CMakeLists.txt`: `src/core/ActivityEpisode.cpp` added to `HG_INGEST_CORE_SOURCES`
  (the real TLS ingress previously failed to link `ActivityEpisodeSelector::best`);
  the TLS ingress test is compiled into the test binary and a
  `production_ingress_tls` ctest is registered only when OpenSSL is found.

### Review follow-up: shutdown race and canonical-gate inclusion

- `request_stop()` no longer flushes durable replay state. `ReplayState` has no
  internal lock, so that flush ran concurrently with a worker finishing
  `complete()`/`abandon()` — a data race on the watermark file. The flush now runs
  in `flush_replay_after_stop()`, called only after every worker has been joined
  (in `run()` and in the destructor) and guarded against running twice.
- Two new regressions: the maintenance worker stops within 2 s with **no inbound
  frame at all** and delivers nothing; and stopping at the same moment an
  authorization revocation becomes due neither hangs behind the core lock nor
  delivers a transition after the stop (three consecutive attempts).
- The suite is now part of the canonical release gate. `scripts/verify_judge_release.sh`
  gained `NATIVE_DETECTOR_WIRING` (step 6b): it always asserts the suite is
  registered in `CMakeLists.txt`/CTest and that its sources exist, then executes it
  over real loopback mTLS. Missing TLS prerequisites produce `SKIP` — never `PASS` —
  and any execution failure fails the gate.

## Verification

Build (project-local rootless OpenSSL staging, no system packages):

    cmake --build hg_build --target homeguardian_tests hg_device_ingress -j2
    -> both targets built

Real mutual-TLS regression (fresh synthetic CA/certs per run, no direct
`observe()` call in the fixture; 6 cases):

    python3 tests/net/run_production_ingress.py
    -> All tests passed (392 assertions in 6 test cases)
    ctest --test-dir hg_build -R production_ingress_tls
    -> 1/1 Passed (12.22 s)

Covered: accepted observation derives exactly one motion observation and is not
re-derived for a completed retransmission, replayed sequence or identity mismatch;
a sink/persistence failure does not train the detector and a retry afterwards does;
a quiet device produces exactly one offline transition without another sample, with
inherited provenance/consent; and rotated credential, withdrawn consent, disabled
capture, revocation, unpaired device, missing registry and malformed registry all
suppress the idle delivery. Ten consecutive fresh connections are never starved by
the maintenance wait.

Shipped binary over synthetic-CA mutual TLS:

    python3 tests/net/run_ingress_binary_smoke.py
    -> accepted=3 rejected=0, clean SIGTERM stop, exit 0

Regression: full native suite from `hg_build` with `HG_SOURCE_DIR` set ->
**76,095 assertions in 777 test cases, all passed**. The new ingress cases are
tagged `[.]`, so they run through the ctest/runner above rather than by default.

Canonical gate (same tree, `HG_BUILD_DIR=hg_build`):

    bash scripts/verify_judge_release.sh
    -> VERDICT PASS, COMMIT_SHA 4b54d2d, 29 checks passed / 0 failed
       NATIVE_DETECTOR_WIRING  PASS (392 assertions in 6 test cases)

Implementation-time RED was observed before each fix (no derived output, no idle
sweep, idle delivery after rotation, and a timed-out queued connection during the
maintenance-wait race). The implementer's RED logs are in the session transcript;
the GREEN figures above were re-run and observed independently.

## Boundaries

This is source and synthetic-mTLS acceptance on the workstation: generated test
certificates and synthetic observations, not hardware. No phone, VPS,
Docker service, capture, credential or cloud state was changed, and no physical
observation or alert is claimed. Known limits: the availability cadence is a fixed
250 ms pulse; idle delivery re-checks registry state and the bound credential, not
the live socket; and default fusion policy still keeps detector evidence advisory,
so no alert threshold changed. Alert-persistence/ACK ordering, out-of-order
correlation and native session recovery remain open from the earlier audit.
