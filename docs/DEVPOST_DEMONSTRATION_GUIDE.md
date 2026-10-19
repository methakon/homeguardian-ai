# HomeGuardian AI — Devpost Demonstration & Judge Reproduction Guide

> [!NOTE]
> **HACKATHON CANDIDATE CERTIFIED (HG-NET-02 / HG-SEC-06D):** This release candidate (`release/hackathon-2026-mcp-candidate`) has completed all security remediation stages (HG-SEC-01 through HG-SEC-06D), out-of-band operator authorization, loopback network ingress hardening, outbound authenticated MCP coordinator bridge verification, and all independent regression test suites (Catch2 C++ tests, UI/device dashboard tests, MCP protocol tests, and E2E Java-C++ bridge tests). Certified for judge evaluation on local workstation loopback with zero external cloud or private VPN dependencies.

**Submission Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server with Alexa-Style Demonstration Client** (AmazonAppDev 2026)  
**Evaluator Target:** Hackathon Judges, Security Auditors & Community Evaluators  
**Candidate Version:** 1.0.3 (Release Candidate)  
**License:** Apache License 2.0 (see [LICENSE](LICENSE) and [NOTICE](NOTICE))  

---

## 1. Executive Summary & Truthfulness Declaration

> [!IMPORTANT]
> **Transparency Declaration for Hackathon Judges:**
> - **Real Native C++ MCP 2025-11-25 Server:** An authoritative, native C++ Model Context Protocol (MCP) server (spec 2025-11-25) is fully implemented, running locally on loopback with Streamable HTTP. It exposes strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`) with CSPRNG 256-bit token credential binding and is not an Amazon-hosted runtime.
> - **Alexa-Style Demonstration Client:** The web dashboard provides an interactive client-side Alexa-style conversational assistant connected directly to the C++ bridge and native MCP tools over loopback. There is **no live Amazon cloud identity** and no production Echo hardware required.
> - **Synthetic vs Real Cameras (Fail-Closed Hardware Boundary):** In accordance with the Project Manager's Hardware Authorization policy and safe testing standards, the judge build strictly isolates physical cameras (`camera-001`, `camera-002`) in an **offline and unverified** state. Viewing requests for physical cameras are refused with HTTP 400 (`physical_camera_unverified`). All live demonstrations use the hardware-isolated **synthetic test provider** (`camera-syn-001`). No physical camera or microphone is ever activated during evaluation.

---

## 2. Quick-Start Judge Reproduction (Single Command)

To verify the entire end-to-end integration across C++ core, native MCP server, device registry, and web gateway in a clean temporary sandbox:

```bash
# 1. Clone or extract release package
cd /path/to/homeguardian-ai

# 2. Run the end-to-end synthetic demonstration harness
bash scripts/verify_synthetic_mcp_demo.sh
```

**Expected Result:**
```text
=================================================================
DEMONSTRATION RESULTS: 9 PASSED, 0 FAILED
=================================================================
✔ Release Integration Synthetic Demonstration is CERTIFIED (97 Tests Passing: 71 Dashboard/WebRTC + 26 MCP Client).
Demonstration Dashboard URL: http://127.0.0.1:18880
Single-run operator credentials: user='admin', password configured via secure runtime environment
```

---

## 3. Interactive Web Demonstration Walkthrough (3-Minute Script)

Launch the unified demonstration server:

```bash
bash demo.sh
```

Open your browser to: `http://127.0.0.1:8199` (native) or `http://127.0.0.1:18880` (Node gateway).

| Timestamp | Interface Screen & Action | Judge Voiceover / Demonstration Rationale |
|---|---|---|
| **0:00 – 0:30** | **System Overview & Honesty Banner:** Point out the prominent banner indicating synthetic test feeds and loopback binding. View the **System Status Card** (`State: Running`, `Capture: Enabled (Simulated)`). | *"HomeGuardian AI provides edge-first residential safety monitoring. Notice the top banner: all observations are synthetic. No physical camera or microphone is activated, preserving total household privacy."* |
| **0:30 – 1:00** | **Native C++ MCP Conversational Chat:** Click the **Conversational Assistant** tab. Ask: *"Is everything okay right now?"* and *"Are physical cameras active?"* | *"The conversational interface translates operator queries into JSON-RPC 2.0 calls directly to our native C++ MCP server over Streamable HTTP. The server queries in-process state and generates dynamic, grounded responses."* |
| **1:00 – 1:30** | **Simulated Fall Scenario Execution:** In the Scenario selector, choose **Unusual Activity / Critical Impact**, click **▶ Run scenario**. | *"Watch the real C++ pipeline execute: synthetic sensor observations flow through validation, multi-detector fusion, and cooldown deduplication. An explainable alert is generated and persisted in SQLite."* |
| **1:30 – 2:00** | **Authoritative Device Registry & Consent Gate:** Navigate to the **Registered Devices** tab. View the list of cameras and sensors. | *"The device inventory is managed by the C++ DeviceRegistry. Notice that physical cameras are strictly marked offline and unauthorized. Viewing permission requires explicit user consent."* |
| **2:00 – 2:30** | **WebRTC Live Camera Viewer:** Select `camera-syn-001`, click **▶ Start Live View**. Observe the synthetic video test pattern stream. | *"Clicking Live View requests a scoped, single-use view token. WebRTC negotiates loopback SDP without cloud relays. Werift renders a hardware-isolated test pattern. Zero video frames pass over MCP or plain JSON."* |
| **2:30 – 3:00** | **Fail-Closed Privacy Boundary Demonstration:** Click **⏹ Stop Stream**. Select an unconsented physical device and attempt viewing. | *"Attempting to view an unconsented or physical camera fails closed immediately with HTTP 400. In addition, idle viewers automatically time out, stopping media tracks and releasing all peer connections."* |

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
| **C++ Core & Sanitizers** | `HG_SOURCE_DIR=$(pwd) ./build/homeguardian_tests` | Catch2 suite: contract bounds, SQLite WAL retention, CSPRNG stress loop | **526 / 526** (38,926 assertions) |
| **Independent Security Gate** | `bash scripts/test_hg_sec_01_independent.sh` | HG-SEC-01 threat model: method restriction, CSRF token replay, zero-mutation SQLite invariant | **15 / 15** |
| **MCP Hardening & Isolation** | `bash scripts/verify_mcp.sh` | 256-bit session entropy, Bob vs Alice session isolation, slowloris read deadlines, overload drain | **71 / 71** |
| **WebRTC & Devices Gate** | `node web_mcp/test_devices_dashboard.js` | Origin validation, fail-closed consent, CSPRNG device pairing, sequence replay protection | **132 / 132** |
| **Conversational Client** | `node web_mcp/test_client.js` | Streamable HTTP compliance, tool routing, conversation intent generation | **26 / 26** |
| **Alexa Skill Contract** | `cd alexa_skill && npm test` | Contract boundary tests, error encapsulation, payload size limits | **77 / 77** |
| **Startup & Footprint** | `bash scripts/bench_demo.sh` | Native bridge cold startup (0.278s) vs legacy Node (0.118s), zero credential leakage | **PASS** |
| **Alert Retention** | `HG_BRIDGE_PATH=build/hg_alexa_bridge bash scripts/verify_retention.sh` | SQLite 30-day age retention window, durable pruning on restart | **8 / 8** |
| **Build & Deploy Guards** | `bash scripts/verify_deploy_guards.sh` | Non-root container execution, unprovisioned mount refusal, absent image credentials | **12 / 12** |

---

## 6. Honest Hardware & Deployment Limitations

Absolute transparency is foundational to our submission. The exact status of hardware and deployment environments is documented below:

- **Xiaomi Mi A3 (Android Node):** Android camera streaming integration has been verified in lab adb/Wi-Fi test harnesses; in the judge release, physical camera streaming is locked fail-closed (`physical_camera_unverified`) to prevent unauthorized live capture during evaluation.
- **Panasonic Eluga:** Tested as an auxiliary edge sensor device in staging; not provisioned with live cryptographic credentials in the default judge inventory.
- **V380 Wi-Fi IP Camera:** RTSP ingestion pipeline designed for local LAN; intentionally held offline in the judge distribution pending user-authorized pairing.
- **Alexa Echo Hardware:** No production Alexa Echo physical hardware or Amazon cloud skill store publication is claimed; the project provides a local-first Alexa-style demonstration client and ASK v2-compatible native C++ bridge.
- **Remote-Access Readiness:** Deployed by default strictly on loopback (`127.0.0.1`); public WAN/VPS exposure requires operator configuration of TLS reverse proxy (nginx/caddy) and is not enabled by default.

---

## 7. Devpost Submission Checklist

Before submitting to Devpost, verify each item:

- [x] **Repository Public & Accessible:** https://github.com/methakon/homeguardian-ai
- [x] **Open-Source License:** Apache License 2.0 with [NOTICE](file:///home/swarna-sekhar-dhar/projects/homeguardian-ai-worktrees/antigravity-submission-packaging/NOTICE) file attributing third-party dependencies.
- [x] **Truthful System Description:** Positioned truthfully as a Real Native C++ MCP 2025-11-25 Server with an Alexa-Style Demonstration Client; no claims of live Amazon identity or cloud integrations.
- [x] **Hardware Privacy Compliance:** Documented that physical cameras are withheld in the judge build (fail-closed hardware boundary) and test feeds are synthetic.
- [x] **Clean Release Package:** Verifiable archive generated with `scripts/package_release.sh` containing clean source and SHA-256 checksums.
- [x] **Tested Out-of-Source Build:** Verified offline build using CMake 3.28+ and C++20.
- [x] **Working WebRTC Live View:** Verified WebRTC signaling and simulated canvas test pattern.
- [x] **Working Native MCP Tools:** Verified `get_homeguardian_status` and `get_alert_summary`.
