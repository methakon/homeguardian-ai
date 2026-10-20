# HomeGuardian AI — Release Proof & Verification Register

**Document Version:** 1.0.0  
**Candidate SHA:** `13471a90abed022b57bb626672ea237650b6f81a` (`13471a9`)  
**Parent SHA:** `7c3c32307092f61d80a6048d58442d994dbec32c`  
**License:** Apache License 2.0  

---

## 1. Overview & Verification Standards

This register provides a one-to-one mapping between every technical capability claimed in the HomeGuardian AI submission materials (README, Devpost, architecture documentation, video demonstration) and its corresponding implementation source, automated test suite, execution command, and verified evidence.

Every test referenced below executes cleanly with exit code 0 under standard Linux environments.

---

## 2. Master Proof Matrix

| # | Product Claim | Implementation Source | Test File / Harness | Verification Command | Expected & Verified Outcome |
|---|---|---|---|---|---|
| **1** | **Fail-Closed Consent Gate** | `src/core/ConsentGate.cpp`<br>`src/core/ConsentGuardedDevice.cpp` | `tests/core/test_consent_gate.cpp`<br>`tests/core/test_consent_guarded_device.cpp` | `./build/homeguardian_tests "[consent]"` | 100% fail-closed: sensor acquisition refused without active grant; withdrawal forces immediate stop. 13 cases / 45 assertions pass. |
| **2** | **Statistical Anomaly Detection** | `src/core/AnomalyDetector.cpp`<br>`src/core/FusionEngine.cpp` | `tests/core/test_detection.cpp` | `./build/homeguardian_tests "[detection]"` | Rolling z-score baselines calculated; warm-up intervals observed; raw deviations advisory; multi-sensor fusion required for alert qualification. 20 cases / 67 assertions pass. |
| **3** | **Event Correlation & Alert Dedup** | `src/core/TimeWindowCorrelation.cpp`<br>`src/core/Pipeline.cpp` | `tests/core/test_pipeline.cpp`<br>`tests/core/test_rules.cpp` | `./build/homeguardian_tests "[pipeline]"` | Bounded event buffers; time-window correlation; duplicate alerts suppressed until condition clears and re-arms. Pass. |
| **4** | **SQLite Persistence & Migration** | `src/persistence/SchemaManager.cpp`<br>`src/persistence/AlertRepository.cpp` | `tests/persistence/test_sqlite.cpp`<br>`tests/persistence/test_migration.cpp` | `./build/homeguardian_tests "[persistence]"` | Additive schema migration v1→v2; transactional inserts; retention cleanup removes expired alert metadata; zero raw media. Pass. |
| **5** | **Read-Only Alexa Contract** | `src/core/AlexaContract.cpp` | `tests/core/test_alexa_contract.cpp` | `./build/homeguardian_tests "[alexa]"` | Exposes strictly `get_status` and `get_alert_summary`. Immutable outputs, zero mutation endpoints. Pass. |
| **6** | **Hardened Credential Store** | `src/core/AlexaCredentialStore.cpp` | `tests/core/test_credentials.cpp` | `./build/homeguardian_tests "[creds]"` | File permission checks (0600 required, world-readable rejected with exit 3); bearer token hashing; fail-closed authentication. Pass. |
| **7** | **Native MCP Server (2025-11-25)** | `src/mcp/McpHttpServer.cpp`<br>`src/mcp/McpProtocol.cpp` | `tests/mcp/test_mcp_server.cpp`<br>`scripts/verify_mcp.sh` | `bash scripts/verify_mcp.sh` | 73/73 protocol conformance checks pass: Streamable HTTP POST/GET/DELETE, JSON-RPC 2.0, tools list, tools call, idle timeout. |
| **8** | **OS-Backed CSPRNG Session IDs** | `src/mcp/McpHttpServer.cpp:44`<br>`web_mcp/auth.js:8` | `tests/mcp/test_mcp_server.cpp` (collision loop) | `./build/homeguardian_tests "[mcp]"` | 256-bit entropy via `/dev/urandom` and `crypto.randomBytes(32)`; 500-iteration collision stress test passes (33,001 assertions). |
| **9** | **MCP Principal-Session Binding** | `src/mcp/McpHttpServer.cpp`<br>`src/mcp/McpProtocol.cpp` | `scripts/test_hg_sec_01_independent.sh` | `bash scripts/test_hg_sec_01_independent.sh` | Gate 1 & 14 verify that session minted by Principal A cannot be adopted, read, or deleted by Principal B (returns 403/404). Pass. |
| **10** | **WebRTC Active Keepalive & Expiry** | `web_mcp/server.js:630`<br>`web_mcp/webrtc_streamer.js:140` | `web_mcp/test_devices_dashboard.js` | `node web_mcp/test_devices_dashboard.js` | Active stream with `touch` survives >60s idle limit; abandoned stream automatically terminated in 500ms sweep; media tracks stopped. 161/161 assertions pass. |
| **11** | **Physical Hardware Lockout** | `web_mcp/server.js:460`<br>`src/net/DeviceRegistry.cpp:110` | `web_mcp/test_devices_dashboard.js` | `node web_mcp/test_devices_dashboard.js` | Physical camera (`camera-001`) viewing requests return HTTP 400 `physical_camera_unverified`. Synthetic derivation strictly enforced. Pass. |
| **12** | **Bidirectional Redaction in Inspector** | `web_mcp/mcp_adapter.js:115`<br>`web_mcp/server.js:290` | `scripts/verify_synthetic_mcp_demo.sh` | `bash scripts/verify_synthetic_mcp_demo.sh` | `GET /api/mcp/traffic` returns `[REDACTED]` for `mcp-session-id`, `authorization`, `set-cookie`, and `x-csrf-token`. 9/9 demo checks pass. |
| **13** | **Authoritative Device Registry CLI** | `src/main_device_registry_cli.cpp`<br>`src/net/DeviceRegistry.cpp` | `tests/net/test_device_registry.cpp` | `./build/homeguardian_tests "[registry]"` | Heartbeat authenticates against stored credential ID; unexpected arguments rejected with exit 1; zero argument shifts. Pass. |
| **14** | **Replay Protection Watermark** | `src/net/DeviceIngressServer.cpp:75` | `tests/net/test_ingress_server.cpp` | `./build/homeguardian_tests "[ingress]"` | Message sequence watermark persisted in atomic database transaction; out-of-order and duplicate sequences rejected. Pass. |
| **15** | **Low Memory Footprint & No Leaks** | `src/main.cpp`<br>`src/bridge/hg_alexa_bridge.cpp` | `scripts/bench_demo.sh` | `bash scripts/bench_demo.sh` | C++ core RSS < 6.4 MB; Node gateway RSS < 65 MB; zero orphaned child processes or zombie tasks upon exit. Pass. |
| **16** | **Clean-Clone Source Reproducibility** | `scripts/package_release.sh` | `scripts/package_release.sh` | `bash scripts/package_release.sh` | Generates clean archive, validates 47 mandatory release components, confirms zero file/key leaks, and extracts cleanly in sandbox. Pass. |

---

## 3. Summary of Aggregate Evidence

- **Total C++ Test Cases:** 527
- **Total C++ Assertions:** 38,929
- **Dashboard / WebRTC Integration Tests:** 161
- **MCP Client Tests:** 26
- **Native MCP Protocol Conformance Tests:** 73
- **Independent HTTP Security Gates:** 15
- **End-to-End Synthetic Demo Gates:** 9
- **Overall Suite Pass Rate:** **100% (Zero test failures across all targets)**
- **Sanitizers:** ASan / UBSan Clean
