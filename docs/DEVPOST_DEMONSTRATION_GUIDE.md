# HomeGuardian AI — Devpost Demonstration & Judge Reproduction Guide

> [!NOTE]
> **HACKATHON CANDIDATE CERTIFIED (HG-NET-02 / HG-SEC-06D):** This release candidate (`release/hackathon-2026-mcp-candidate`) has completed all security remediation stages (HG-SEC-01 through HG-SEC-06D), out-of-band operator authorization, loopback network ingress hardening, outbound authenticated MCP coordinator bridge verification, and all independent regression test suites (Catch2 C++ tests, UI/device dashboard tests, MCP protocol tests, and E2E Java-C++ bridge tests). Certified for judge evaluation on local workstation loopback with zero external cloud or private VPN dependencies.

**Submission Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server with Alexa-Style Demonstration Client** (AmazonAppDev 2026)  
**Evaluator Target:** Hackathon Judges, Security Auditors & Community Evaluators  
**Candidate Version:** 1.0.3 (Release Candidate)  
**License:** Apache License 2.0 (see [LICENSE](../LICENSE) and [NOTICE](../NOTICE))  

---

## 1. Executive Summary & Truthfulness Declaration

> [!IMPORTANT]
> **Transparency Declaration for Hackathon Judges:**
> - **Real Native C++ MCP 2025-11-25 Server:** An authoritative, native C++ Model Context Protocol (MCP) server (spec 2025-11-25) is fully implemented, running locally on loopback with Streamable HTTP. It exposes strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`) with CSPRNG 256-bit token credential binding and is not an Amazon-hosted runtime.
> - **Alexa-Style Demonstration Client:** The web dashboard provides an interactive client-side Alexa-style conversational assistant connected directly to the C++ bridge and native MCP tools over loopback. There is **no live Amazon cloud identity** and no production Echo hardware required.
> - **Synthetic vs Real Cameras (Fail-Closed Hardware Boundary):** In accordance with the Project Manager's Hardware Authorization policy and safe testing standards, the judge build strictly isolates physical cameras (`camera-001`) in an **offline and unverified** state. Viewing requests for physical cameras are refused with HTTP 400 (`physical_camera_unverified`). All live demonstrations use the hardware-isolated **synthetic test provider** (`camera-syn-001`). No physical camera or microphone is ever activated during evaluation.

---

## 2. Quick-Start Judge Reproduction (Automated Single Command)

To automatically verify that the genuine native C++ MCP server, Streamable HTTP coordinator, protocol 2025-11-25 handshake, strictly two read-only tools, and security boundaries build and pass cleanly on local workstation loopback:

```bash
# 1. Clone repository or checkout release branch
git checkout release/hackathon-2026-mcp-candidate

# 2. Run the automated judge release verification harness
bash scripts/verify_judge_release.sh
```

**Expected Result (Machine-Readable Verdict):**
```text
================================================================================
HOMEGUARDIAN AI — JUDGE RELEASE VERIFICATION SUMMARY
================================================================================
VERDICT:                PASS
COMMIT_SHA:             d5cf13e...
BRANCH:                 release/hackathon-2026-mcp-candidate
DURATION_SECONDS:       5
BUILD_PREREQUISITES:    PASS (C++20, CMake, Node.js, npm packages)
NATIVE_MCP_PROTOCOL:    PASS (73/73 checks passed via verify_mcp.sh)
MCP_HANDSHAKE:          PASS (Protocol 2025-11-25, 256-bit CSPRNG token)
READ_ONLY_TOOLS:        PASS (get_homeguardian_status, get_alert_summary)
MUTATING_TOOLS_EXCLUDED:PASS (Zero mutating tools, zero camera frame tools)
SECURITY_BOUNDS:        PASS (401 Auth, 403 Session Hijack/Origin, 413 Payload)
OUTBOUND_COORDINATOR:   PASS (Loopback binding, auth token, 503 fail-closed offline)
CAMERA_SAFETY_BOUNDARY: PASS (media_capture_enabled=false, 0 physical video streams)
CLEAN_PROCESS_SHUTDOWN: PASS (All child processes exited cleanly, 0 leaks)
TOTAL_CHECKS_PASSED:    27
TOTAL_CHECKS_FAILED:    0
================================================================================
Judge evaluation ready. To reproduce, run: bash scripts/verify_judge_release.sh
```

### Judge Failure Guidance & Troubleshooting
If `scripts/verify_judge_release.sh` reports a failure:
1. **Missing Prerequisites:** Ensure `curl`, `jq`, `cmake` (>= 3.20), `make`, `g++` (or `clang++` with C++20), and `node` (>= 18) are installed on the host. The script will explicitly identify any missing tool.
2. **Missing Node Modules:** If npm packages are absent, run `npm install` once from repository root (installs `ws` and `werift` without cloud access).
3. **Stale Background Listeners:** If test ports are blocked by an existing daemon, run `pkill -f hg_mcp_server || true` and retry. All ports are selected dynamically from unprivileged ranges (`18800-19900`).
4. **Credential Permissions (0600):** The native C++ MCP server strictly enforces POSIX `0600` or `0400` permissions on secret files and refuses execution if world-readable. The script automatically sets `chmod 600` on generated test credentials.
5. **No Cloud or Network Required:** All checks execute strictly over `127.0.0.1` loopback; no internet access or VPN connection is needed during verification.

---

### Alternative: End-to-End Synthetic Web Demo Harness
To launch the interactive web gateway and synthetic test video streaming:

```bash
bash scripts/verify_synthetic_mcp_demo.sh
```

---

## 3. Interactive Web Demonstration Walkthrough (3-Minute Script)

Launch the unified demonstration server:

```bash
bash demo.sh
```

Open your browser to: `http://127.0.0.1:18880` (Local Web Gateway).

| Timestamp | Interface Screen & Action | Judge Voiceover / Demonstration Rationale |
|---|---|---|
| **0:00 – 0:30** | **System Overview & Honesty Banner:** Point out the prominent top banner (`SYNTHETIC SENSOR ENVIRONMENT (EVALUATION MODE)`). View the **System Status Card** (`State: Running`, `Capture: Enabled (Simulated)`). | *"HomeGuardian AI provides edge-first residential safety monitoring. Notice the top banner: all observations are synthetic. No physical camera or microphone is activated, preserving total household privacy."* |
| **0:30 – 1:00** | **Native C++ MCP Conversational Chat:** Click the **💬 MCP Conversational Client** tab. Click quick-chat button `"Is everything okay?"` or type a question. | *"The conversational interface translates operator queries into JSON-RPC 2.0 calls directly to our native C++ MCP server over Streamable HTTP. The server queries in-process state and generates dynamic, grounded responses."* |
| **1:00 – 1:30** | **Simulated Fall Scenario Execution:** In the **⚡ Synthetic Incident Testing** panel, click **⚠️ Trigger Simulated Critical Fall (Peak 0.95 -> Immobility 0.10)**. | *"Watch the real C++ pipeline execute: synthetic sensor observations flow through validation, multi-detector fusion, and cooldown deduplication. An explainable alert is generated and persisted in SQLite."* |
| **1:30 – 2:00** | **Authoritative Device Registry & Consent Gate:** Navigate to the **📹 Peripheral Devices & Live Camera** tab. View the **📡 Peripheral Device Registry** card listing paired devices (`camera-syn-001`, `camera-001`). | *"The device inventory is managed by the C++ DeviceRegistry. Notice that physical camera camera-001 is strictly marked offline and unverified. Viewing permission requires explicit user consent."* |
| **2:00 – 2:30** | **WebRTC Live Camera Viewer:** In the **📷 Live Camera Panel**, select `camera-syn-001` and click **▶ Start Live View**. Observe the synthetic video test pattern stream. | *"Clicking Live View requests a scoped, single-use view token. WebRTC negotiates loopback SDP without cloud relays. Werift renders a hardware-isolated test pattern. Zero video frames pass over MCP or plain JSON."* |
| **2:30 – 3:00** | **Fail-Closed Privacy Boundary & Wire Inspector:** Click **⏹ Stop**. Attempt to view unverified physical device `camera-001` (refused with HTTP 400). Switch to **🔬 WebRTC & MCP Wire Inspector** tab to view redacted wire frames. | *"Attempting to view an unverified physical camera fails closed immediately with HTTP 400. The wire inspector proves tokens are strictly redacted and zero raw media leaks."* |

---

## 4. Reproducing the Real Native C++ MCP Experience

HomeGuardian AI includes an industrial-grade C++ implementation of the **Model Context Protocol** (version `2025-11-25`, Streamable HTTP transport).

### 4.1 Running the Standalone C++ MCP Server

```bash
# Build the native MCP server binary
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hg_mcp_server -j4

# Provision an isolated 0600 credentials file
WORK="$(mktemp -d)"
cat <<EOF > "$WORK/mcp_creds.json"
{
  "judge-token-2026": {
    "principal": "judge",
    "allow_status": true,
    "allow_alert_summary": true,
    "expires_at_ms": 0,
    "revoked": false
  }
}
EOF
chmod 600 "$WORK/mcp_creds.json"

# Launch native MCP server on port 18980
HG_MCP_PORT=18980 ./build/hg_mcp_server config/homeguardian.demo.json "$WORK/mcp_creds.json"
```

### 4.2 Verifying MCP Wire Protocol via curl

In a separate terminal, execute the standard MCP handshake:

#### 1. MCP Initialization (`initialize`)
```bash
curl -i -X POST http://127.0.0.1:18980/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer judge-token-2026" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-11-25",
      "capabilities": {},
      "clientInfo": { "name": "judge-tester", "version": "1.0.0" }
    }
  }'
```
*Observe:* HTTP 200 response returning `Mcp-Session-Id` header (64 hex characters, OS CSPRNG entropy) and negotiated protocol version `2025-11-25`.

#### 2. MCP Tool Listing (`tools/list`)
```bash
# Save your session ID from the initialize response
SESSION_ID="<paste-session-id-here>"

curl -i -X POST http://127.0.0.1:18980/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer judge-token-2026" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list",
    "params": {}
  }'
```
*Observe:* Returns read-only tools: `get_homeguardian_status` and `get_alert_summary`.

#### 3. MCP Tool Call (`tools/call`)
```bash
curl -i -X POST http://127.0.0.1:18980/mcp \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer judge-token-2026" \
  -H "Mcp-Session-Id: $SESSION_ID" \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "get_homeguardian_status",
      "arguments": {}
    }
  }'
```
*Observe:* Returns real pipeline status with `read_only: true` and current capture status.

### 4.3 Claude Desktop / Host Client Integration Configuration

To connect Claude Desktop or an MCP-compliant host client directly to HomeGuardian AI:

Add the following to your `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "homeguardian": {
      "command": "/path/to/homeguardian-ai/build/hg_mcp_server",
      "args": [
        "/path/to/homeguardian-ai/config/homeguardian.demo.json",
        "/path/to/homeguardian-ai/config/mcp_creds.json"
      ],
      "env": {
        "HG_MCP_PORT": "18980",
        "HG_MCP_HOST": "127.0.0.1"
      }
    }
  }
}
```

---

## 5. Security & Verification Evidence

All security boundaries are continuously asserted by automated test suites:

| Suite Name | Execution Command | Test Scope & Invariants Verified | Passed / Total |
|---|---|---|---|
| **Automated Judge Smoke** | `bash scripts/verify_judge_release.sh` | Deterministic one-command judge smoke: C++, MCP handshake, tools, security, coordinator | **PASS (27/27)** |
| **C++ Core & Sanitizers** | `HG_SOURCE_DIR=$(pwd) ./build/homeguardian_tests` | Catch2 suite: contract bounds, SQLite WAL retention, CSPRNG stress loop | **527 / 527** (38,926 assertions) |
| **Independent Security Gate** | `bash scripts/test_hg_sec_01_independent.sh` | HG-SEC-01 threat model: method restriction, CSRF token replay, zero-mutation SQLite invariant | **15 / 15** |
| **MCP Hardening & Isolation** | `bash scripts/verify_mcp.sh` | 256-bit session entropy, Bob vs Alice session isolation, slowloris read deadlines, overload drain | **73 / 73** |
| **WebRTC & Devices Gate** | `node web_mcp/test_devices_dashboard.js` | Origin validation, fail-closed consent, CSPRNG device pairing, sequence replay protection | **161 / 161** |
| **Java WS Bridge E2E** | `node tests/test_java_ws_bridge_cxx_e2e.js` | Java OutboundMcpBridge $\leftrightarrow$ Node Coordinator $\leftrightarrow$ Native C++ Engine | **21 / 21** |
| **Outbound Bridge Gate** | `node tests/test_outbound_mcp_bridge.js` | Mutual auth, operation allowlist, 64KB bounds, 4403 revocation, reconnect backoff | **46 / 46** |
| **Conversational Client** | `node web_mcp/test_client.js` | Streamable HTTP compliance, tool routing, conversation intent generation | **26 / 26** |
| **Alexa Skill Contract** | `cd alexa_skill && npm test` | Contract boundary tests, error encapsulation, payload size limits | **77 / 77** |
| **Android Route Security** | `RouteSecurityPolicyTest.java` | /32 overlay routing, `Network.bindSocket()` pinning, zero LAN fallback | **44 / 44** |
| **Startup & Footprint** | `bash scripts/bench_demo.sh` | Native bridge cold startup (0.278s) vs legacy Node (0.118s), zero credential leakage | **PASS** |
| **Alert Retention** | `HG_BRIDGE_PATH=build/hg_alexa_bridge bash scripts/verify_retention.sh` | SQLite 30-day age retention window, durable pruning on restart | **8 / 8** |
| **Build & Deploy Guards** | `bash scripts/verify_deploy_guards.sh` | Non-root container execution, unprovisioned mount refusal, absent image credentials | **12 / 12** |

---

## 6. Honest Hardware & Deployment Limitations

Absolute transparency is foundational to our submission. The exact status of hardware and deployment environments is documented below:

- **Xiaomi Mi A3 (Android 11 Node):** Physical handset is recognized over ADB with baseline Edge Service v1.0.0 verified; however, its USB data link remains intermittent and the v1.0.3 update is not yet installed. Android node transport is therefore evaluated cleanly using synthetic test observations and isolated loopback fixtures, holding the unstable physical handset fail-closed (`physical_camera_unverified`) so hardware glitches never interrupt the judge demonstration.
- **Panasonic Eluga (CCTV Handset):** A separate legacy Android 8.1 / API 28 device attached in lab staging for auxiliary camera tests; strictly distinct from the Mi A3, unprovisioned with judge credentials, and isolated from the evaluation runtime.
- **V380 Wi-Fi IP Camera:** RTSP ingestion pipeline designed for local LAN; intentionally held offline in the judge distribution pending user-authorized pairing.
- **Alexa Echo Hardware:** No production Alexa Echo physical hardware or Amazon cloud skill store publication is claimed; the project provides a local-first Alexa-style demonstration client and ASK v2-compatible native C++ bridge.
- **Remote Access Readiness:** Public remote access is not deployed; services bind strictly to loopback (`127.0.0.1`). Public WAN/VPS exposure is unsupported in this candidate and deferred to future work.

---

## 7. Devpost Submission Checklist

Before submitting to Devpost, verify each item:

- [x] **Repository Public & Accessible:** https://github.com/methakon/homeguardian-ai
- [x] **Open-Source License:** Apache License 2.0 with [NOTICE](../NOTICE) file attributing third-party dependencies.
- [x] **Truthful System Description:** Positioned truthfully as a Real Native C++ MCP 2025-11-25 Server with an Alexa-Style Demonstration Client; no claims of live Amazon identity or cloud integrations.
- [x] **Hardware Privacy Compliance:** Documented that physical cameras are withheld in the judge build (fail-closed hardware boundary) and test feeds are synthetic.
- [x] **Clean Release Package:** Verifiable archive generated with `scripts/package_release.sh` containing clean source and SHA-256 checksums.
- [x] **Tested Out-of-Source Build:** Verified offline build using CMake 3.28+ and C++20.
- [x] **Working WebRTC Live View:** Verified WebRTC signaling and simulated canvas test pattern.
- [x] **Working Native MCP Tools:** Verified `get_homeguardian_status` and `get_alert_summary`.
