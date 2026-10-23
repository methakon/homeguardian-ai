# HomeGuardian AI — Devpost Demonstration & Judge Reproduction Guide

> [!NOTE]
> **Evidence scope:** PR #20 / `release/hackathon-2026-mcp-candidate` is historical, not current HEAD acceptance. Consult [live-path verification](ALEXA_LIVE_PATH_VERIFICATION.md), [Android physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md), and [current status](STATUS.md). The synthetic reproduction is loopback-only; the separate existing ASK lane uses AWS and a private Oracle/Android path. Video/publication and final submission remain unverified.

**Submission Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server with Alexa-Style Demonstration Client** (AmazonAppDev 2026)  
**Evaluator Target:** Hackathon Judges, Security Auditors & Community Evaluators  
**Candidate Version:** 1.0.3 (Release Candidate)  
**License:** Apache License 2.0 (see [LICENSE](../LICENSE) and [NOTICE](../NOTICE))  

---

## 1. Executive Summary & Truthfulness Declaration

> [!IMPORTANT]
> **Transparency Declaration for Hackathon Judges:**
> - **Real Native C++ MCP 2025-11-25 Server:** An authoritative, native C++ Model Context Protocol (MCP) server (spec 2025-11-25) is fully implemented, running locally on loopback with Streamable HTTP. It exposes strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`) with CSPRNG 256-bit token credential binding and is not an Amazon-hosted runtime.
> - **Working classic ASK lane, separate from local client:** A functional **classic ASK hosted skill → AWS SQS → Oracle private worker → Android bridge → native C++ tools** path exists. Voice replies are owner-attested; mailbox/native round trips independently verified in the linked record. This is **not production Alexa+ or certified monitoring**. Self-hosted MCP 2025-11-25 Streamable HTTP is the Alexa+ track route; the browser is a separate local demonstration client.
> - **Synthetic versus physical media:** Detector/demo/regression inputs and the test-pattern viewer are synthetic. Unverified physical devices fail closed. Explicit operator uploads may store media locally for simulated playback, **not live camera capture or detection**. Optional owner-controlled private-dashboard live feed remains pending verification, not claimed implemented. Never publish private camera streams.
> - **Privacy boundary:** No raw sensor media over AWS; aggregate read-only status/alerts do cross AWS. Sensor-pipeline metadata retention is separate from explicit local operator-upload storage.

---

## 2. Quick-Start Judge Reproduction (Automated Single Command)

To automatically verify that the genuine native C++ MCP server, Streamable HTTP coordinator, protocol 2025-11-25 handshake, strictly two read-only tools, and security boundaries build and pass cleanly on local workstation loopback:

```bash
# 1. Use the intended main submission checkout; record its revision
git rev-parse HEAD

# 2. Run the automated judge release verification harness
bash scripts/verify_judge_release.sh
```

**Historical candidate output example (not a current HEAD verdict):**
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

### Automated Verification Harness
To verify end-to-end integration (C++ Device Registry, WebRTC signaling, Native C++ MCP server, and Operator Authentication) in an automated test run:

```bash
bash scripts/verify_synthetic_mcp_demo.sh
```

---

## 3. Interactive Demonstration Walkthrough (3-Minute Script)

HomeGuardian AI provides two distinct local demonstration experiences:

### Primary: 3-Tab Model Context Protocol & Devices Gateway (Recommended)
Launches the genuine native C++ MCP server (`hg_mcp_server`) and the interactive 3-tab gateway (`http://127.0.0.1:18880`):

```bash
# Option A: One-command launch with automated preflight & persistent serve
bash scripts/verify_synthetic_mcp_demo.sh --serve

# Option B: Explicit two-terminal launch
# Terminal 1: Native C++ MCP Server
HG_MCP_PORT=18980 ./build/hg_mcp_server config/homeguardian.demo.json <credentials.json>
# Terminal 2: Demonstration Gateway
HG_WEB_PORT=18880 HG_MCP_PORT=18980 node web_mcp/server.js
```

Open your browser to: `http://127.0.0.1:18880` (Local Web Gateway).

### Alternative: Standalone Simulated Alexa+ Event Bridge (`demo.sh`)
Launches a single native C++ binary (`hg_alexa_bridge`) running an embedded HTTP server to demonstrate the simulated Alexa+ event bridge without Node.js dependencies:

```bash
bash demo.sh
```
*(Prints a dynamic loopback URL such as `http://127.0.0.1:<port>` for minimal single-page event simulation).*

---

### Demonstration Script (3-Tab Gateway on `http://127.0.0.1:18880`)

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

### 4.3 MCP Host Client Integration

Use a host client that supports **Streamable HTTP**, pointed at the already
running `http://127.0.0.1:18980/mcp` endpoint with operator-provisioned bearer
credentials. `hg_mcp_server` is an HTTP server, not a stdio MCP command;
command-spawn configuration alone does not connect a host to this transport.
Host-specific setup is not verified here; the curl sequence above is the
explicit reproduction path.

---

## 5. Historical Candidate Verification Evidence

The table below preserves earlier candidate results, **not current HEAD totals
or an all-green submission verdict**. Run the listed commands on the intended
revision and retain actual output. Current scoped acceptance and limitations are
in [live-path verification](ALEXA_LIVE_PATH_VERIFICATION.md),
[Android physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md), and
[current status](STATUS.md). The deployed coordinator predates source hardening;
recovery/reliability limitations in the live-path record remain open.

| Suite Name | Execution Command | Test Scope & Invariants Verified | Passed / Total |
|---|---|---|---|
| **Automated Judge Smoke** | `bash scripts/verify_judge_release.sh` | Deterministic one-command judge smoke: C++, MCP handshake, tools, security, coordinator | **PASS (27/27)** |
| **C++ Core & Sanitizers** | `HG_SOURCE_DIR=$(pwd) ./build/homeguardian_tests` | Catch2 suite: contract bounds, SQLite WAL retention, CSPRNG stress loop | **527 / 527** (38,929 assertions) |
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

- **Android node:** The existing private ASK lane reaches native tools via the Android bridge (see acceptance records above). Capture remained disabled; this transport proof is not physical sensor-to-alert acceptance. Permanent stale-session and reboot/changed-network recovery remain limitations.
- **Panasonic Eluga (CCTV Handset):** A separate legacy Android 8.1 / API 28 device attached in lab staging for auxiliary camera tests; strictly distinct from the Mi A3, unprovisioned with judge credentials, and isolated from the evaluation runtime.
- **LAN camera sources:** Native HTTP and bubble-protocol adapters are present. Private-dashboard live playback remains pending the camera worker's verification; offline protocol tests are not live-view acceptance. No public camera stream is part of judge reproduction.
- **Alexa hardware/publication:** Classic ASK hosted-skill replies are owner-attested. Production Echo hardware acceptance, production Alexa+, and skill-store publication are not claimed.
- **Private versus public access:** The existing Oracle worker uses a private path; no public MCP port or public camera stream is claimed. The synthetic judge demo remains loopback-only without cloud dependencies.

---

## 7. Devpost Submission Checklist

Before submitting to Devpost, verify each item:

- [x] **Repository Public & Accessible:** https://github.com/methakon/homeguardian-ai
- [x] **Open-Source License:** Apache License 2.0 with [NOTICE](../NOTICE) file attributing third-party dependencies.
- [x] **Truthful System Description:** Self-hosted native MCP plus a separate functional classic ASK/AWS/private-worker/Android lane, with owner-attested voice and independent mailbox/native evidence; no production Alexa+ or certified monitoring claim.
- [x] **Hardware Privacy Compliance:** Documented that physical cameras are withheld in the judge build (fail-closed hardware boundary) and test feeds are synthetic.
- [ ] **Current Release Package:** Generate and verify the intended submission archive and checksums; historical packaging is not current HEAD acceptance.
- [ ] **Current Out-of-Source Build:** Verify the intended submission revision and retain actual output.
- [x] **Working WebRTC Live View:** Verified WebRTC signaling and simulated canvas test pattern.
- [x] **Working Native MCP Tools:** Verified `get_homeguardian_status` and `get_alert_summary`.
