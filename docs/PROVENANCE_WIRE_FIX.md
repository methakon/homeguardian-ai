# Native producer provenance wire correction

Date: 2026-10-23. State: SOURCE / JAVA COMPILE / HOST PARSER TESTED; NOT INSTALLED.

## Scope and fix

Second authorized corrective batch, after coordinator security. The native Android
acquired-observation default was `real_device`; the Java forwarding path preserved
it as `provenance_claim`, but DeviceProtocol accepts only `real` or `synthetic`.
Change the originating native default to `real`. Do not add a legacy receiver alias
or weaken authenticated provenance derivation. Java's missing-field fallback stays
synthetic; a canonical claim is not authentication or permission.

## TDD evidence

Tests extract the actual default from apk/jni/hg_harness_jni.cpp, rather than
inserting a hand-written canonical value unrelated to the producer. JVM extraction
passes it through ObservationWire. The native case feeds it to the actual wire
decoder and derive_provenance, including authentication, pairing, consent and
capture refusals, synthetic acquisition, and rejection of the legacy token.

RED before the production change:

- JVM schema: 42 passed, 1 failed; actual default was real_device.
- Native case: 7 assertions passed, 1 failed at decoded.has_value(); receiver
  reported unknown provenance_claim.

Parent GREEN verification after the fix:

    TMPDIR="$PWD/hg_build/test-tmp" bash apk/tests/run_isolated_tests.sh
    cmake --build hg_build --target homeguardian_tests -j2
    TMPDIR="$PWD/hg_build/test-tmp" HG_SOURCE_DIR="$PWD" \
      hg_build/homeguardian_tests '[native-emitter]'

Production Java compiled against android-34 (58 classes); lifecycle checks 45
passed, schema checks 43 passed, zero failures. The normal main test executable
passed the native regression: 24 assertions in one case. `git diff --check` passed.
The unchanged surrounding code still emits compiler/deprecation warnings; these
were not hidden or described as a warning-free full build.

## Boundaries and next batch

This proves source-derived producer vocabulary and real host receiver behavior,
not Android JNI execution. No APK/native Android compilation, installation,
physical sensor, TLS delivery, phone/VPS restart or capture was performed.

Detector wiring follows as a separate tested commit. Preparation of its real TLS
executable exposed an existing missing ActivityEpisode.cpp link dependency in the
optional ingress target. It is not repaired or declared green by this provenance
checkpoint. Native MCP/tests and TLS client/probe built; physical acceptance and
production deployment remain open.
