# HomeGuardian AI — Master Component Index & Architecture Guide

## 1. System Overview

HomeGuardian AI is an edge-first, privacy-preserving residential monitoring system. It provides real-time ambient fall and safety detection, sensory fusion, verifiable cryptographic device authentication, and isolated streaming interfaces without sending user audio or video to third-party cloud services.

```
+-------------------------------------------------------------------------+
|                              Host Browser                               |
|               (Local UI / WebRTC Viewer / MCP Operator)                 |
+-------------------------------------------------------------------------+
       |                                             |
       | HTTP / SSE (Bearer Token / Cookie + CSRF)   | WebRTC (SDP / ICE)
       v                                             v
+-----------------------------+               +---------------------------+
|    Node Gateway & MCP       |               |    WebRTC Media Engine    |
|   (web_mcp/server.js)       | <-----------> | (webrtc_streamer.js)      |
+-----------------------------+               +---------------------------+
       |                                             |
       | Subprocess / Loopback                       | Subprocess / IPC
       v                                             v
+-----------------------------+               +---------------------------+
|      C++ MCP Service        |               |    C++ Device Registry    |
|       (hg_mcp_server)       |               | (hg_device_registry_cli)  |
+-----------------------------+               +---------------------------+
       |                                             |
       +----------------------+----------------------+
                              |
                              v
              +-------------------------------+
              |   Core C++ Pipeline Engine    |
              |     (Native Alexa Bridge)     |
              |       (hg_alexa_bridge)       |
              +-------------------------------+
                              |
                              v
              +-------------------------------+
              |   SQLite Durable Audit Log    |
              | (homeguardian_bridge_audit.db)|
              +-------------------------------+
```

---

## 2. Component Directory Index

### Core C++ Components (`src/core/`, `src/net/`, `src/mcp/`, `src/bridge/`)

| Path | Binary / Component | Description |
|------|--------------------|-------------|
| `src/core/AlexaContract.h/.cpp` | Contract Interface | Enforces read-only Alexa integration contract (version 1), strict request bounds (4KB max, 512 char max token), and fail-closed error categories. |
| `src/core/AlexaCredentialStore.h/.cpp` | Credential Store | Constant-time bearer token verifier reading 0600-permissioned credential files. Binds credentials to SHA-256 hashes and enforces non-empty principals. |
| `src/mcp/McpHttpServer.h/.cpp` | MCP HTTP Server | Multi-threaded HTTP transport for Model Context Protocol. Enforces OS-backed CSPRNG 256-bit session IDs, strict principal/credential session binding, slow-client timeouts, and overload load-shedding with graceful socket drain. |
| `src/mcp/McpProtocol.h/.cpp` | MCP Protocol | JSON-RPC 2.0 dispatcher for MCP lifecycle (`initialize`, `tools/list`, `tools/call`, notifications). Exposes strictly read-only tools. |
| `src/net/DeviceRegistry.h/.cpp` | Device Registry | Manages peripheral device lifecycle, cryptographic credential binding, consent recording, sequence replay watermarks, and authenticated liveness. |
| `src/main_device_registry_cli.cpp` | `hg_device_registry_cli` | CLI utility for listing, getting, consenting, pairing, revoking, and unpairing peripheral devices in HG-UI-03 format. |
| `src/bridge/hg_alexa_bridge.cpp` | `hg_alexa_bridge` | Native bridge process hosting simulator runtime, status provider, and native HTTP dashboard/API. |

### Web & Integration Gateway (`web_mcp/`)

| Path | Component | Description |
|------|-----------|-------------|
| `web_mcp/server.js` | Node Gateway | Authenticated HTTP server exposing operator dashboard, device management, MCP proxy, and WebRTC signaling. Requires HttpOnly session cookies and CSRF tokens for all state mutations. |
| `web_mcp/auth.js` | Auth Engine | Scrypt/SHA-256 constant-time operator authentication engine managing session tokens, rate limiting, and CSRF token lifecycles. |
| `web_mcp/webrtc_streamer.js` | WebRTC Streamer | Manages WebRTC PeerConnections for live camera viewing. Enforces Origin validation, pre-allocation stream ownership checks, proactive 1s background cleanup, 60s idle timeout, and absolute stream lifetimes. |
| `web_mcp/device_adapter.js` | Device Adapter | Bridges C++ `hg_device_registry_cli` output to Node.js gateway with strict verification of pairing identity and synthetic provider status. |
| `web_mcp/mcp_adapter.js` | MCP Adapter | Mediates communication between web interface and C++ MCP server with strict redaction of credentials in inspector logs. |

---

## 3. Security Model & Defensive Gates

The system enforces six independent security boundaries:

1. **MCP Session Ownership (B-1):**
   - Session identifiers are generated using OS-backed cryptographic entropy (`/dev/urandom`, 256 bits).
   - Sessions are bound to both a verified principal and a deterministic SHA-256 credential identity.
   - Cross-principal session adoption, `tools/list`, `tools/call`, and `DELETE` attempts are rejected with HTTP 403 Forbidden.
   - Empty principals are rejected at load time; omitted principals default to deterministic derived identities (`cred:<sha256>`).

2. **WebRTC PeerConnection Leak Prevention (B-2):**
   - Stream ownership is checked **before** allocating `RTCPeerConnection` resources. If an active stream exists for a device under a different session, the offer is rejected immediately with HTTP 400 (`forbidden: Cannot stop another session's active stream`) with zero resource allocation.
   - All subsequent SDP negotiation and tracking paths are wrapped in a comprehensive try/catch block ensuring `pc.close()` is called on any failure.

3. **Proactive Stream Expiration & Idle Timeout (B-3):**
   - Streams are monitored by a proactive background timer running every 1 second (`cleanupIntervalMs = 1000`).
   - Streams sit subject to three independent expiration conditions:
     1. Absolute stream lifetime (`maxStreamLifetimeMs`, default 30 min).
     2. Idle/inactivity timeout (`idleTimeoutMs`, default 60 sec).
     3. Immediate teardown upon session logout or session death.

4. **Command-Line & Log Secret Hygiene (B-4):**
   - Secrets are never passed via command-line arguments (`--token` or `--secret`).
   - All runtime scripts (`demo.sh`, `test_hg_sec_01_independent.sh`, `bench_demo.sh`, `verify_retention.sh`, `verify_integration_dashboard.sh`, `deploy/docker/entrypoint.sh`) pass secrets via 0600-permissioned files or environment variables (`HG_HTTP_SECRET`, `HG_HTTP_TOKEN`).
   - Command-line arguments in `hg_alexa_bridge` emit a prominent security warning if legacy flags are used.

5. **Device Registry Integrity & Persistence (B-5):**
   - Heartbeats from unpaired, revoked, or unauthenticated devices are strictly rejected and exit non-zero (exit 3), never marking the device `online: true` on disk or in memory.
   - Device loading preserves durable `last_sequence` watermarks and `last_heartbeat_ms` timestamps across all devices.
   - Devices start offline by default; no fake liveness is written to disk.
   - Full CLI support for `pair`, `revoke`, and `unpair` operations.

6. **HTTP Overload Shedding Correctness (B-6):**
   - Load-shedding at maximum concurrency limits sends HTTP 503 (`Retry-After: 1`), initiates graceful shutdown (`::shutdown(client, SHUT_WR)`), and drains unread client bytes before closing the socket.
   - Prevents Linux kernel TCP `RST` packets, ensuring the client reliably receives the status code.

---

## 4. Offline Build & Test Procedure

The build is fully self-contained and operates in air-gapped / offline environments:

### Prerequisites
- CMake 3.16+
- C++17 compliant compiler (GCC 9+ or Clang 10+)
- Node.js 18+ (for web gateway tests)
- Pre-cached dependencies for `nlohmann_json` and `spdlog`

### CMake Offline Configuration
```bash
cmake -B build \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<path_to_nlohmann_json_src> \
  -DFETCHCONTENT_SOURCE_DIR_SPDLOG=<path_to_spdlog_src> \
  -DHOMEGUARDIAN_BUILD_TESTS=ON

cmake --build build -j$(nproc)
```

### Running Test Suites
When executing tests directly, supply `HG_SOURCE_DIR` to resolve relative test fixtures:

```bash
# 1. Full C++ test suite (Catch2)
HG_SOURCE_DIR=$(pwd) ./build/homeguardian_tests

# 2. Independent HTTP Security & Replay Harness
bash scripts/test_hg_sec_01_independent.sh

# 3. Web Dashboard & WebRTC Integration Suite
node web_mcp/test_devices_dashboard.js

# 4. MCP Protocol & Client Verification
node web_mcp/test_client.js
bash scripts/verify_mcp.sh

# 5. Full Synthetic Multi-Component Demo
bash scripts/verify_synthetic_mcp_demo.sh
```

---

## 5. Test Suite Metrics & Breakdown

The test suite provides comprehensive coverage across units, integration points, and security gates:

- **C++ Test Suite:** 526 distinct test cases (38,926 assertions).
- **Assertion Count Breakdown:**
  - Standard Unit & Contract Assertions: ~5,921 assertions across lifecycle, protocol, encryption, and registry logic.
  - CSPRNG Uniqueness & Entropy Stress Loop: 33,005 assertions generated by a 500-iteration collision test verifying length, hex character validity, and uniqueness.
- **Integration Test Cases:**
  - Independent security gate: 14 strict HTTP security gates.
  - WebRTC & dashboard gate: 71 multi-operator and streaming checks (`test_devices_dashboard.js`).
  - MCP verification gate: 71 protocol conformance checks (`verify_mcp.sh`).
  - MCP client gate: 26 client communication checks (`test_client.js`).
  - Alexa skill & bridge contract: 77 specification checks (`npm test` in `alexa_skill`).
