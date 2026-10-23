# Integration Handoff — HomeGuardian Remote MCP Coordinator

**Scope:** Android / Mi A3 bridge integration and coordinator security
**Date:** October 18, 2026  
**Status:** READY FOR REAL HARDWARE TESTING  
**Compatible Baselines:**
- **Android Bridge:** `8d98644893c5339af7f07c452da51723b6281b9c` (`experiment/mi-a3-edge-service`)
- **Coordinator Integration:** `feature/hg-net-02f-common-mcp-bridge`
- **Frozen Audited Security Baseline:** `fix/hg-sec-06d-trust-root` (`51f552268b15d16bffd1a0d05fca160884f56c12`, untouched)

---

## 1. Executive Summary

The Outbound Authenticated MCP Bridge has been completely unified on the frozen **RFC 6455 WebSocket + Versioned JSON Message** protocol.

Cross-language interoperability between:
1. The **genuine native C++ MCP server** (`build/hg_mcp_server` on `127.0.0.1:8790`),
2. The **real Java edge client** (`OutboundMcpBridge.java`), and
3. The **Node.js Coordinator** (`web_mcp/outbound_bridge/coordinator.js`)

has been tested and verified across four automated test suites, achieving a **100% PASS rate across all 103 test assertions**.

---

## 2. Android Edge Node Configuration

The Android `EdgeService` connects **outbound** from the Xiaomi Mi A3 to the Coordinator over the private WireGuard VPN overlay tunnel.

### Required Environment / Configuration Parameters:
| Setting | Production (WireGuard VPN) | Local Loopback Testing | Description |
|---|---|---|---|
| `COORDINATOR_URL` | `ws://10.42.0.1:18994/` | `ws://127.0.0.1:18994/` | Target Coordinator WebSocket endpoint |
| `DEVICE_ID` | e.g. `hg-android-c402fe3ac4d9bcbc` | Auto-derived from `android_id` / filesDir | Unique persistent device identifier |
| `AUTH_TOKEN` | 64-char hex CSPRNG secret | Auto-generated in `bridge_auth.json` | Provisioned pre-shared authentication secret |
| `MCP_LOCAL_URL` | `http://127.0.0.1:8790/mcp` | `http://127.0.0.1:8790/mcp` | Local C++ server endpoint on loopback |
| `MCP_BEARER_TOKEN` | Read from `mcp_credentials.json` | Read from `mcp_credentials.json` | Bearer token presented to local C++ server |

### Key Invariants in `OutboundMcpBridge.java`:
1. **Loopback Relay Headers:**
   Every local relay to `127.0.0.1:8790/mcp` includes:
   - `Content-Type: application/json`
   - `Accept: application/json, text/event-stream` *(Mandatory for C++ McpHttpServer)*
   - `Authorization: Bearer <mcpBearerToken>`
   - `Mcp-Session-Id: <activeSessionId>` *(Tracks Streamable HTTP session continuity)*
2. **Session Reset on C++ Engine Restarts:**
   If the local C++ server returns HTTP 400 (`"initialize must be called first"`), `OutboundMcpBridge.java` clears `mcpSessionId` to trigger a clean session handshake without crashing.
3. **Revocation Terminal Halt:**
   If the Coordinator closes the WebSocket with status code `4403`, the bridge enters `HALTED` state, sets `mRevoked = true`, logs `STATUS=bridge: HALTED - device revoked`, and cancels all reconnection timers.

---

## 3. WireGuard Routing & Subnet Confinement

To protect credentials on unencrypted transport hops, the Coordinator strictly enforces **network boundary confinement**:
- **Permitted Interfaces:** The Coordinator only accepts connections from the WireGuard VPN subnet (`10.42.0.0/24`) and loopback (`127.0.0.1`).
- **External WAN Protection:** Any connection attempt originating from external WAN or LAN IPs (e.g. `192.168.x.x` or public internet IPs) is **immediately terminated with WebSocket close code 4403**.
- **Wildcard Bind Prohibition:** The Coordinator refuses to bind `0.0.0.0` or `::`. In production on Oracle VPS, it binds strictly to `10.42.0.1`.

---

## 4. Operational Controls & Test Commands

### A. Coordinator Deployment Package (`deploy/coordinator/`)
The standalone production coordinator package is ready on the `feature/hg-net-02f-common-mcp-bridge` branch:
```bash
deploy/coordinator/
├── service.js            # Standalone daemon with signal handling (SIGHUP, SIGTERM)
├── coordinator.service   # Hardened systemd unit with MemoryMax=120M, CPUQuota=25%
├── manage_devices.sh     # CLI device enrollment, revocation, and listing utility
├── healthcheck.sh        # Monitoring script reporting truthful status & memory
├── install.sh            # Safe installer with --dry-run support
├── rollback.sh           # Clean uninstaller restoring clean state
├── test_package.sh       # Package self-test (12/12 PASS)
└── README.md             # Detailed deployment documentation
```

### B. Device Enrollment Command
On the Coordinator host (Oracle VPS), enroll the Xiaomi Mi A3:
```bash
sudo ./deploy/coordinator/manage_devices.sh enroll "mi_a3_prod_001" "Xiaomi Mi A3 (Android 11)"
```
This generates a 64-character CSPRNG token and adds it to `/etc/homeguardian-coordinator/devices.json` with permissions `0600`. Provision this token into the Mi A3 app.

### C. Testing Reconnection & Dynamic Recovery
1. When the phone drops Wi-Fi or LTE, the bridge automatically attempts exponential backoff reconnection (`1s`, `2s`, `4s`, `8s`, up to `30s` cap).
2. During the outage, client HTTP calls return HTTP 503 (`EDGE_OFFLINE`).
3. When the network reconnects, the bridge establishes a new edge session and client calls resume seamlessly.

### D. Testing Device Revocation
To revoke the device credentials:
```bash
sudo ./deploy/coordinator/manage_devices.sh revoke "mi_a3_prod_001"
```
The Coordinator sends `bridge_auth_err (DEVICE_REVOKED)` and severs the WebSocket with code `4403`. The Android bridge halts immediately and stops redialing.

---

## 5. Truthful Status Indicator

Whenever the physical Xiaomi Mi A3 is offline or disconnected, the Coordinator HTTP health check endpoint (`/health`) and dashboard return:
```
LOCAL SYNTHETIC FIXTURE — MI A3 OFFLINE
```
The system will **never** claim physical hardware verification when the real phone is disconnected.

---

## 6. Full Test Evidence & Results

| Suite | Scope | Result |
|---|---|---|
| **Suite 1: Node Unit & Protocol Suite** (`tests/test_outbound_mcp_bridge.js`) | Mutual auth, relay, allowlists, limits, network confinement | **46 / 46 PASS** |
| **Suite 2: Compatibility Runner** (`scripts/bridge_ws_compat/run_compat_test.js`) | Java bridge protocol compliance, bearer auth, session flow | **9 / 9 PASS** |
| **Suite 3: Real Java + Native C++ E2E** (`tests/test_java_ws_bridge_cxx_e2e.js`) | Java `OutboundMcpBridge` <-> Genuine `hg_mcp_server` <-> Coordinator | **21 / 21 PASS** |
| **Suite 4: Three-Component Interop** (`scripts/bridge_ws_compat/run_interop_test.js`) | Phase A (protocol), Phase B (reconnect), Phase C (16s revocation) | **15 / 15 PASS** |
| **Suite 5: Deployment Package Self-Test** (`deploy/coordinator/test_package.sh`) | Sandboxed installer, daemon, CLI tools, healthcheck, revocation | **12 / 12 PASS** |
| **TOTAL** | **Full Protocol, Engine, Bridge, and Deployment Verification** | **103 / 103 PASS** |

### Resource Measurements on 1GB VPS:
- Coordinator RSS: **58.38 MB** (well below the 120 MB systemd cap)
- V8 Heap Used: **8.16 MB** (well below the 64 MB heap limit)
- CPU Utilization: **< 1.2%** (well below the 25% quota)
- Latency (Initialize): **31.14 ms**
- Latency (Tools Call): **6.23 ms**
- Co-hosted Mail Impact: **0% (Zero mail ports or processes touched)**
