# Linux ARM64 hub integration package (contract v1.0, R2)

**Target:** a headless ARM64 Linux host acting as the hub. **Nothing here assumes the Mi A3 has booted
Linux**; these are the build and check steps a Linux hub runs, and they are host-architecture independent.

## Components the integration uses (all real, no mock MCP answers)

| component | path | role |
|---|---|---|
| `ObservationProducer` | `src/net/ObservationProducer.*` | the producer half: builds contract messages, adopts the hub watermark on reconnect |
| `DeviceRegistry` | `src/net/DeviceRegistry.*` | enrollment, credential binding, consent, capture switch, revocation |
| `ObservationIngest` | `src/net/ObservationIngest.*` | the authenticated receive path: schema, identity, timestamps, provenance recording |
| `IngestDetectors` | `src/net/IngestDetectors.*` | derives normalized observations per device and per observation type |
| `Application` | `src/core/Application.*` | the engine: pipeline, fusion rule, alert policy, alert sink |
| `make_homeguardian_tools` | `src/mcp/McpTools.*` | registers the two read-only tools over the existing Alexa contract |
| `Dispatcher` | `src/mcp/McpProtocol.*` | the real JSON-RPC dispatcher used for `tools/call` |
| `TlsTransport` / `DeviceIngressServer` | `src/net/` | the socket layer (OpenSSL), for a real network deployment |

The integration test is `tests/integration/test_network_sensor_to_mcp.cpp` (`[e2e]`).

## Build

```sh
# deps: a C++20 compiler, CMake >= 3.16, OpenSSL dev headers, nlohmann_json, spdlog, pthreads
cmake -S . -B build -DHOMEGUARDIAN_BUILD_TESTS=ON
cmake --build build -j"$(nproc)" --target homeguardian_tests
```

On a minimal ARM64 image the only non-header dependency the network path adds over core is OpenSSL; the
protocol, registry, ingest and detector layers are OpenSSL-free and build without it (CMake then skips only
the socket-layer binaries and says so).

### ARM64 status: COMPILED, not yet linked or run

Verified on this host (`docs/evidence/ARM64_CROSS_COMPILE.log`): **19 representative sources compile for
aarch64** with `aarch64-linux-gnu-g++ 13.3.0` in an `ubuntu:24.04` container pinned to `--platform
linux/amd64`, so the compiler runs natively and only the output is aarch64. `readelf -h` on a produced
object reports `Class: ELF64`, `Type: REL`, `Machine: AArch64` — real aarch64 codegen, not a syntax check.
The sources covered are the network, media, camera, audio and protocol layers this document describes.

**Still NOT verified, and not claimed:** the *link* of a complete ARM64 binary (that needs an arm64 sysroot
with libssl/libspdlog/libstdc++, i.e. a multiarch rootfs) and *execution* of one on ARM64 hardware. Anyone
reading this as "the hub runs on ARM64" is reading more than has been shown; the correct statement is that
the code compiles for aarch64 and the remaining steps are a sysroot link and a run on the target.

## The single integration command

```sh
scripts/linux_hub_integration_check.sh          # builds, runs the three contract tags, prints PASS/FAIL
```

or by hand:

```sh
./build/homeguardian_tests '[net]'          # protocol, registry, replay, ingest, detectors
./build/homeguardian_tests '[producer]'     # the producer half
./build/homeguardian_tests '[reliability]'  # duplicates, reordering, reconnect, revocation, drift
./build/homeguardian_tests '[e2e]'          # the whole chain, with genuine MCP answers
```

## Expected output (the lines that mean it worked)

```
[net][audit]      All tests passed (149 assertions in 9 test cases)
[producer]        All tests passed  (90 assertions in 7 test cases)
[reliability]     All tests passed  (89 assertions in 5 test cases)
[e2e]             All tests passed (182 assertions in 14 test cases)

corroboration: alert id=alert-...  rule=detection-fusion  severity=info  confidence=0.663
corroboration MCP get_alert_summary: {"alert_count":2,"alert_ids":[...],"highest_severity":"info",
                                      "read_only":true}
e2e status after : {"capture_enabled":true,"read_only":true,"state":"Running"}
```

## Error diagnostics — what each symptom means

| symptom | meaning | where to look |
|---|---|---|
| `unsupported_schema` | your `schema_version` is not 1 | contract §1 |
| `bad_version` on the wire | your `protocol_version_major/minor` is not 1/0 | handoff §1 |
| `device_unknown` / `unauthorized` | not enrolled, or the credential is not the registered fingerprint | registry pairing |
| `consent_missing` / `consent_withdrawn` | consent not granted, or withdrawn mid-stream (the registry is authoritative) | handoff §3 |
| `replayed_sequence` / `duplicate_observation` | your sequence went backwards or repeated after a reconnect — **adopt `welcome.resume_after_sequence`** | handoff §5 |
| `malformed_timestamp` / `stale_timestamp` | more than the skew ahead, older than 24 h, or behind your own out-of-order window | handoff §4 |
| `unknown_source_type` / `unknown_observation_type` | the event type is outside the hub's policy vocabulary | handoff §6 |
| observations accepted, **no alert ever** | evidence does not corroborate: one detector class cannot clear `min_confidence = 0.5` (`confidence ≤ c·p²`) | handoff "Alert semantics" |
| no alert and the fusion says *detectors conflict* | agreement is below 0.6 — the detectors disagree; do not "fix" it by shouting louder with one instrument | handoff "Alert semantics" |
| `get_homeguardian_status` returns an authorization error | the bearer token did not authenticate, or the engine is not healthy — both fail closed by design | handoff §7 |

## Runtime notes for a headless hub

- No camera, microphone or speaker is required on the hub: sources are remote and the hub consumes derived
  observations. **The MCP API is not a media relay** and carries no frames or samples.
- The engine's own local acquisition switch is separate from a device's capture switch in the registry; a
  hub that only consumes network observations can leave its own acquisition off, which also makes alert
  provenance unambiguous.
- A single enrolled node cannot corroborate itself: enroll at least two distinct observation types when you
  want alerts rather than status.
