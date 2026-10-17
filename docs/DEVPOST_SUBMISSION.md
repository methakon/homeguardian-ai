# HomeGuardian AI — Official Devpost Submission Entry

**Project Title:** HomeGuardian AI — Privacy-First Whole-Home Intelligence  
**Tagline:** Local-first, consent-gated home safety intelligence with native C++ core, MCP 2025-11-25 Streamable HTTP, and Alexa-style conversational demonstration client.  
**Primary Track:** Alexa+ Track — **Real Native C++ MCP Server with Alexa-Style Demonstration Client** (per official Hackathon FAQ for entrants without preview partner access)  
**GitHub Repository:** https://github.com/methakon/homeguardian-ai  
**Release Candidate SHA:** `347551558fc3c4f5deb11b2c566b86ecc6e59db4`  
**License:** Apache License 2.0  

---

## 1. Project Description (Markdown for Devpost)

### Inspiration
Modern smart home technology asks families to accept an uncomfortable trade-off: in exchange for safety alerts and convenience, we must invite proprietary, cloud-tethered surveillance cameras into our private living spaces. Video streams and audio recordings are piped off-premises, processed by black-box cloud models, and stored indefinitely on third-party servers. When network connectivity fails or remote servers suffer outages, home safety is compromised.

We asked ourselves: **Can we build a whole-home intelligence platform that delivers safety, wellbeing insights, and agentic conversational capabilities without ever compromising household privacy or relying on cloud surveillance?**

HomeGuardian AI is our answer.

---

### What It Does
HomeGuardian AI is a local-first, privacy-by-design home intelligence platform designed to observe household wellbeing patterns, detect statistical anomalies, and provide conversational safety status through an agentic interface.

Key features include:
1. **Consent-Gated Delivery:** All media and sensor processing sits behind a fail-closed cryptographic consent gate. If consent is absent, expired, or withdrawn, sensor acquisition and delivery immediately stop. Zero raw media ever reaches storage or memory pipelines.
2. **Explainable Anomaly Detection & Fusion:** Sensor observations are evaluated on-device against learned statistical baselines (mean, standard deviation, warm-up intervals). Bounded numerical features from multiple sensor modalities are fused into actionable insights. Raw observations alone never trigger alarms; an alert is generated only when fused, multi-sensor mathematical evidence qualifies.
3. **Native Model Context Protocol (MCP) Server:** Fully implements the official Model Context Protocol (spec 2025-11-25) in native C++ over Streamable HTTP. Autonomous AI agents can query home status and alert histories through strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`). Sessions are secured by OS-backed CSPRNG 256-bit tokens with strict credential binding and principal validation.
4. **Peripheral Device Registry & WebRTC Viewer:** An authoritative C++ device registry manages paired peripherals with CSPRNG credentials, monotonic sequence watermarks for replay prevention, strict POSIX permissions (0600/0700), and trusted operator authorization gates (`--operator-auth`, `--operator-recovery`). A built-in WebRTC streaming engine enables real-time, low-latency browser monitoring with single-use scoped view tokens.
5. **Real MCP Demonstration with Alexa-Style Client:** In full compliance with the hackathon rules and official FAQ, the web gateway acts as an actual MCP client (issuing `initialize`, `tools/list`, and `tools/call` over Streamable HTTP). Evaluators can converse in natural language (*"Is everything okay?"*, *"Show me recent alerts"*), inspect live MCP wire frames, and verify that tool execution queries the live C++ engine without cloud intermediaries.
6. **Encrypted Local Persistence & Retention:** All alert metadata (timestamps, rule IDs, mathematical evidence) is stored in a local SQLite database with automated age-based retention and schema migration.

---

### How We Built It
We engineered HomeGuardian AI as a high-performance, three-tier architecture:

- **Tier 1 — Native C++ Core Engine (`homeguardian`, `hg_alexa_bridge`):** Written in modern C++20 with zero bloat. Utilizes `spdlog` for structured logging, `nlohmann/json` for schema serialization, and an embedded SQLite amalgamation for zero-external-dependency persistence. Compiles cleanly across x86_64 and ARM64 with an idle memory footprint under 7 MB RAM.
- **Tier 2 — Authoritative C++ Services (`hg_mcp_server`, `hg_device_registry_cli`):**
  - Native MCP server implementing 2025-11-25 Streamable HTTP with `/dev/urandom` CSPRNG session IDs, origin allowlists, worker limits, and fail-closed credential verification.
  - Peripheral device registry CLI managing device pairing, CSPRNG credential provisioning, sequence watermarks, and consent status.
- **Tier 3 — Local Web Gateway & Signaling (`web_mcp/server.js`):** Node.js gateway that isolates browser interactions. Provides HttpOnly cookie session management, CSRF double-submit validation, conversational MCP translation, WebRTC peer connection signaling (`werift`), and bidirectional header redaction (`[REDACTED]` for sensitive authorization headers and session IDs).

---

### Technical Separation: Real MCP vs. Simulated Frontend vs. Future Alexa Camera Hardware

In strict accordance with the hackathon honor code and official rules:
- **What is Real & Implemented in C++:**
  - Real native C++ Model Context Protocol (MCP 2025-11-25 Streamable HTTP) server.
  - Real MCP tool calling (`get_homeguardian_status`, `get_alert_summary`) with real SQLite database backing and state derivation.
  - Real WebRTC signaling, media track generation, and active keepalive lifecycle sweeps.
  - Real peripheral device registry with CSPRNG credential minting, sequence replay protection, POSIX 0600 storage, and operator recovery authorization.
- **What is Simulated for Hackathon Evaluation:**
  - The Alexa-style conversational web client: Because official Alexa+ Add-on developer tools (Category SDK, MCP Toolkit, CLI, Web Simulator) are in preview for select partners only (see Incident 75 and official FAQ), our web interface acts as a genuine MCP client speaking Streamable HTTP to our server.
  - The video stream: All active video streams in the demo use our clearly labeled **synthetic test camera** (`camera-syn-001`). Physical cameras default to an unverified state and refuse streaming requests with HTTP 400.
- **What is Not Claimed (Future Roadmap):**
  - Production Echo hardware certification, AVS device firmware, production Amazon LWA cloud account linking, and physical V380/Panasonic RTSP camera integration over public WAN are explicitly excluded from this release.

---

### Accomplishments We're Proud Of
- **Zero Cloud Exfiltration:** Achieved complete home monitoring and anomaly detection without sending a single byte of telemetry or media to external cloud infrastructure.
- **Extensive Test Coverage & Rigor:** The system is validated by:
  - **38,926 Catch2 assertions across 526 C++ test cases** (100% passing).
  - **158 Device Dashboard & WebRTC integration tests** (100% passing).
  - **71 Native MCP Streamable HTTP protocol tests** (100% passing).
  - **26 MCP Conversational Client tests** (100% passing).
  - **15 Independent adversarial HTTP security gates** (100% passing).
  - Total: **39,196 automated checks with zero failures**.
- **Clean Memory Footprint:** The combined idle footprint of the C++ core and MCP server is under 15 MB RSS, making it fully deployable on edge devices, low-power Raspberry Pis, and low-spec cloud instances.
- **Open Standards Alignment:** Built against open, royalty-free standards: Apache-2.0 licensing, Model Context Protocol 2025-11-25, WebRTC, and SQLite.

---

## 2. Built With (Tags)

`c++20`, `cmake`, `sqlite`, `spdlog`, `nlohmann-json`, `catch2`, `model-context-protocol`, `mcp`, `webrtc`, `nodejs`, `javascript`, `docker`, `privacy-first`, `edge-ai`, `apache-2.0`

---

## 3. Product Feedback for Amazon & Tool Builders

### 1. Alexa+ Developer Program Tooling & Access Transparency
- **Observation:** The hackathon rules encouraged the Alexa+ Track, but the official Alexa+ Add-on developer tools (Category SDK, MCP Toolkit, CLI, and Web Simulator) are in preview for select partners only without an application path for general developers. Individual setup guides read as though access is open, causing initial confusion before reading the FAQ (Incident 75).
- **Suggestion:** Add a prominent banner to all Alexa+ developer docs stating preview/partner-only status, and publish a unified table distinguishing self-hosted MCP servers, partner-only tooling, classic Smart Home skills, and client-side web simulations.

### 2. Model Context Protocol (MCP 2025-11-25) in IoT Environments
- **Observation:** The Streamable HTTP transport specification provides excellent lightweight flexibility for IoT bridges. However, standard specifications leave multi-principal session binding and session adoption unaddressed.
- **Suggestion:** Standardize session-to-credential binding in the MCP specification to prevent unauthorized callers from adopting or terminating active sessions created by other authenticated principals.

### 3. Smart Home Camera Privacy Standards
- **Observation:** Testing budget consumer cameras (e.g. V380) revealed that enabling ONVIF opened unauthenticated RTSP/ONVIF streams exposing live video and administrative accounts to the local network (Incident 76).
- **Suggestion:** Advocate for mandatory local authentication standards in smart home device certification programs, ensuring that local camera streaming cannot be activated without user-set credentials.
