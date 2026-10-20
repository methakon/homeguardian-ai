# HomeGuardian AI — Official Devpost Submission Entry

> [!NOTE]
> **HACKATHON RELEASE CANDIDATE (HG-NET-02 / HG-SEC-06D):** This submission release candidate (`release/hackathon-2026-mcp-candidate` @ [PR #20](https://github.com/methakon/homeguardian-ai/pull/20)) has completed all security remediation stages (HG-SEC-01 through HG-SEC-06D), loopback ingress hardening, outbound MCP coordinator bridge verification, clean-clone validation, and independent regression test suites (527 C++ tests / 38,929 assertions, 161 UI/dashboard tests, 73 MCP protocol tests, 21 Java E2E bridge tests, 46 outbound bridge tests, 44 route policy tests). Prepared for judge evaluation and independent audit on local workstation loopback.

**Project Title:** HomeGuardian AI — Privacy-First Whole-Home Intelligence  
**Tagline:** Local-first, consent-gated home safety intelligence with native C++ core, Model Context Protocol (spec 2025-11-25) Streamable HTTP server, and Alexa-style demonstration client.  
**Primary Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server & C++ Home Safety Engine with an Alexa-Style Demonstration Client** (AmazonAppDev 2026)  
**GitHub Repository:** https://github.com/methakon/homeguardian-ai  
**License:** Apache License 2.0 (Sha256 verified against apache.org)  

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
3. **Native Model Context Protocol (MCP) Server:** Fully implements the official Model Context Protocol (spec 2025-11-25) in native C++ over Streamable HTTP. Autonomous AI agents can query home status and alert histories through strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`). Sessions are secured by OS-backed CSPRNG 256-bit tokens with strict credential binding.
4. **Peripheral Device Registry & WebRTC Viewer:** An authoritative C++ device registry manages paired peripherals. A built-in WebRTC streaming engine enables real-time, low-latency browser monitoring. In accordance with safe testing principles, physical cameras default to an unverified state (refusing viewing requests), while synthetic test streams allow comprehensive verification without privacy risk.
5. **Alexa-Style Conversational Assistant (Simulated Client):** Using a hardened, read-only simulation contract, the dashboard provides a voice- and text-driven demonstration client for Alexa-style interactions. Operators can query household status (*"Is everything okay?"*, *"Are there any alerts?"*) and inspect live consent states.
6. **Local Persistence & Retention:** All alert metadata (timestamps, rule IDs, mathematical evidence) is stored in a local SQLite database with automated age-based retention and schema migration. Raw sensor media is never persisted.

---

### How We Built It
We engineered HomeGuardian AI as a high-performance, three-tier architecture:

- **Tier 1 — Native C++ Core Engine (`homeguardian`, `hg_alexa_bridge`):** Written in modern C++20 with zero bloat. Utilizes `spdlog` for structured logging, `nlohmann/json` for schema serialization, and an embedded SQLite amalgamation for zero-external-dependency persistence. Compiles cleanly across x86_64 and ARM64 with an idle memory footprint under 7 MB RAM.
- **Tier 2 — Authoritative C++ Services (`hg_mcp_server`, `hg_device_registry_cli`):**
  - Native MCP server implementing 2025-11-25 Streamable HTTP with `/dev/urandom` CSPRNG session IDs, origin allowlists, worker limits, and fail-closed credential verification.
  - Peripheral device registry CLI managing device pairing, cryptographic credentials, watermarked heartbeat replays, and consent status.
- **Tier 3 — Local Web Gateway & Signaling (`web_mcp/server.js`):** Node.js gateway that isolates browser interactions. Provides HttpOnly cookie session management, CSRF double-submit validation, conversational MCP translation, WebRTC peer connection signaling (`werift`), and bidirectional header redaction (`[REDACTED]` for sensitive authorization headers and session IDs).

---

### Challenges We Ran Into
1. **Adversarial WebRTC Stream Lifecycles:** Preventing connection leaks and lingering peer connections when browser tabs abruptly close required implementing both active client keepalives (`POST /api/webrtc/touch`) and proactive 500ms server-side sweeps that automatically terminate abandoned tracks.
2. **Preventing Session Adoption in MCP:** The MCP specification allows flexible transports, but naive implementations can permit unauthorized clients to adopt or delete another principal's session. We solved this by binding every MCP session strictly to server-verified credentials in native C++ and rejecting any cross-principal session mutation.
3. **Fail-Closed Hardware Boundaries:** We designed strict policy gates to prevent unverified physical hardware from accidentally streaming unconsented footage. Synthetic simulators and physical devices are strictly differentiated by type, ensuring that client-submitted JSON cannot manipulate synthetic flags or bypass pairing locks.
4. **Redaction in Debugging Inspector Panels:** Enabling hackathon evaluators to inspect live JSON-RPC MCP wire traffic without leaking bearer tokens or session IDs required building bidirectional sanitization middleware in the web gateway.

---

### Accomplishments We're Proud Of
- **Zero Cloud Exfiltration:** Achieved complete home monitoring and anomaly detection without sending a single byte of telemetry or media to external cloud infrastructure.
- **Extensive Test Coverage & Rigor:** The system is validated by **527 native C++ test cases with 38,929 assertions**, 161 WebRTC and dashboard tests, 26 MCP client tests, 73 native MCP protocol tests, 21 Java E2E bridge tests, 46 outbound bridge tests, and 44 Android route security tests — 100% passing with zero failures.
- **Clean Memory Footprint:** The combined idle footprint of the C++ core and MCP server is under 15 MB RSS, making it fully deployable on edge devices, low-power Raspberry Pis, and low-spec cloud instances.
- **Open Standards Alignment:** Built against open, royalty-free standards: Apache-2.0 licensing, Model Context Protocol 2025-11-25, WebRTC, and SQLite.

---

### What We Learned
- **Privacy Must Be Structural, Not Legalistic:** Privacy guarantees cannot rely on user agreements or "settings" checkboxes; they must be enforced at the hardware and socket abstraction boundaries through fail-closed gates that refuse execution when consent records are missing or expired.
- **Local-First Agentic Architecture Is Feasible:** Modern edge hardware is capable of running lightweight, explainable anomaly detection and serving MCP endpoints locally, eliminating the latency and privacy vulnerabilities of round-trip cloud calls.

---

### What's Next for HomeGuardian AI
- **On-Device Neural Inference:** Integrating OpenCV and ONNX Runtime for optional, privacy-preserving on-device computer vision models (e.g., pose estimation for fall detection) while maintaining our strict consent gate.
- **Production Alexa+ Cloud Integration:** When production Alexa+ developer access becomes publicly available, implementing secure Amazon Login with Amazon (LWA) account linking and official Alexa smart home skills.
- **Distributed Micro-Nodes:** Expanding multi-node mesh networking across low-cost Android and Linux Wi-Fi edge nodes using our watermarked mutual-TLS ingress protocol.

---

## 2. Built With (Tags)

`c++20`, `cmake`, `sqlite`, `spdlog`, `nlohmann-json`, `catch2`, `model-context-protocol`, `mcp`, `webrtc`, `nodejs`, `javascript`, `docker`, `privacy-first`, `edge-ai`, `apache-2.0`

---

## 3. Transparent Disclosures & Hackathon Honor Code

In keeping with the rules of the Amazon Developer Hackathon (AmazonAppDev 2026), we declare the following exact project boundaries:

1. **Submission Track & Positioning:** This entry competes in the **Alexa+ Track**, featuring a **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server and C++ Home Safety Engine with an Alexa-Style Demonstration Client**. The conversational interface provides an interactive, client-side Alexa-style experience interacting with the authoritative C++ MCP server and bridge over loopback. There is **no live Amazon cloud identity**, no production Echo device connection, and no AWS backend services utilized.
2. **Self-Hosted Native MCP Server:** We have built and verified an authoritative, native C++ Model Context Protocol server (spec 2025-11-25, Streamable HTTP, read-only tools). It runs locally on loopback and is not an Amazon-hosted runtime.
3. **Synthetic Evaluation Boundary:** All live video streams and sensor inputs in the demonstration are generated by our deterministic **synthetic test provider** (`camera-syn-001`). Physical cameras (`camera-001`) are intentionally held in an unverified state and refuse streaming requests with HTTP 400 (`physical_camera_unverified`). No physical camera or microphone is activated during judging.
4. **No Ring Integration / No Disaster Forecasting:** Ring integration, automated emergency 911 dispatch, and validated physical disaster predictions are not implemented and are not claimed.
5. **Open Source Verification:** All original code is licensed under the Apache License, Version 2.0. Clean-clone build and reproduction instructions are provided in `docs/DEVPOST_DEMONSTRATION_GUIDE.md`.

### Physical Sensor Status (Disclosed & Separately Investigated; Not Part of PR #20 Release Runtime)

- **Genuine USB UVC motion observed on external experiment branch:** On isolated branch `experiment/hg-v380-09-secure-camera-integration` (commit `b48254121474cb6eee69a63e9c39a73e90c96d35`), an owner-authorized USB UVC camera delivered 600 real frames under explicit operator consent (`--confirm`), where a sustained increase in motion in view was observed (peak motion 0.1252 against 0.0103 still-scene ceiling, confidence 0.849). Bounded numerical scalars only — zero frames stored, transmitted, or streamed.
- **Clean PR #20 release candidate contains no physical camera integration:** In this judge candidate release, no physical camera or microphone is activated or connected; all runtime observations are generated by the synthetic test provider (`camera-syn-001`).
- **A real camera → fusion → delivered alert is NOT verified and not claimed:** Replaying the recorded scalars through the real rule and fusion engine is inconclusive (agreement 0.5913 vs 0.60 threshold). No alert from real frames has been observed, and no threshold was changed to manufacture one.
- **Animal or species recognition is not implemented:** There is no species classifier or animal model runtime, and no document claims one.
- **V380 camera media remains BLOCKED:** No supported authenticated interface exists; camera 1's unauthenticated ONVIF/RTSP port remains closed.
- **Disaster prediction is not demonstrated:** Not implemented, and unrepresentable in the fusion framework.

### Honest Hardware & Deployment Limitations

We believe absolute transparency is the cornerstone of responsible engineering. We state the exact hardware boundaries of physical devices and infrastructure:

- **Xiaomi Mi A3 (Android 11 Node):** Physical handset recognized over ADB with baseline Edge Service v1.0.0 verified; however, due to an intermittent physical USB link and uninstalled v1.0.3 upgrade, Android node transport is evaluated using synthetic test observations and loopback fixtures. Physical hardware streaming is held in an unverified, fail-closed state (`physical_camera_unverified`) so judge evaluation remains deterministic and fully decoupled from physical USB instability.
- **Panasonic Eluga (CCTV Handset):** A separate legacy Android 8.1 / API 28 device tested for auxiliary edge monitoring in lab staging; not provisioned with judge credentials and completely decoupled from the candidate runtime.
- **V380 Wi-Fi IP Camera:** RTSP ingestion pipeline designed for local LAN; intentionally held offline in the judge distribution pending user-authorized pairing.
- **Alexa Echo Hardware:** No production Alexa Echo physical hardware or Amazon cloud skill store publication is claimed; the project provides a local-first Alexa-style demonstration client and ASK v2-compatible native C++ bridge.
- **Remote Access Readiness:** Public remote access is not deployed; services bind strictly to loopback (`127.0.0.1`). Public WAN/VPS exposure is unsupported in this candidate and deferred to future work.

---

## 4. Product Feedback for Hackathon Organizers & Tool Builders

### Model Context Protocol (MCP) Specification (2025-11-25)
- **Strengths:** The Streamable HTTP transport specification provides a clean, modern alternative to stdio-only bridges, enabling remote or containerized AI agents to interact with IoT hubs over standard HTTP.
- **Opportunities:** Explicit security guidance regarding session-to-credential binding and prevention of session adoption in multi-tenant environments would benefit the ecosystem. Our implementation adds explicit principal validation to address this.

### Alexa Skills Kit & Alexa+ Developer Experience
- **Strengths:** The ASK SDK v2 provides a well-structured request/response contract. The mock-driven dispatcher enables rapid local unit testing without cloud dependencies.
- **Opportunities:** Providing standardized local testing emulators for next-generation Alexa+ agentic interactions would significantly lower the barrier for independent developers and edge-first privacy researchers.
