# HomeGuardian AI — Official Devpost Submission Entry

> [!NOTE]
> **Current evidence, not a blanket release verdict:** PR #20 is a historical candidate, not current HEAD acceptance. See [live ASK/mailbox/native verification](ALEXA_LIVE_PATH_VERIFICATION.md), [Android physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md), and [current status](STATUS.md). Source-tested fixes are not necessarily deployed; the live-path record lists remaining recovery and deployed-coordinator limitations. Video recording/publication and final Devpost submission remain unverified.

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
1. **Consent-Gated Delivery:** Sensor delivery sits behind a fail-closed consent gate. Without valid consent and enabled capture, delivery is blocked. Sensor-pipeline persistence retains metadata, not raw sensor media; separately, explicit operator uploads may store media locally for simulated playback, not live camera capture or detection.
2. **Explainable Anomaly Detection & Fusion:** Sensor observations are evaluated on-device against learned statistical baselines (mean, standard deviation, warm-up intervals). Bounded numerical features from multiple sensor modalities are fused into actionable insights. Raw observations alone never trigger alarms; an alert is generated only when fused, multi-sensor mathematical evidence qualifies.
3. **Native Model Context Protocol (MCP) Server:** Fully implements the official Model Context Protocol (spec 2025-11-25) in native C++ over Streamable HTTP. Autonomous AI agents can query home status and alert histories through strictly read-only tools (`get_homeguardian_status`, `get_alert_summary`). Sessions are secured by OS-backed CSPRNG 256-bit tokens with strict credential binding.
4. **Peripheral Device Registry & WebRTC Viewer:** An authoritative C++ device registry manages paired peripherals. A built-in WebRTC streaming engine enables real-time, low-latency browser monitoring. In accordance with safe testing principles, physical cameras default to an unverified state (refusing viewing requests), while synthetic test streams allow comprehensive verification without privacy risk.
5. **Working classic ASK + local MCP client:** A functional hosted classic ASK skill routes through **AWS SQS → Oracle private worker → Android bridge → native C++ tools**. Voice replies are owner-attested; independent mailbox/native round trips are recorded in [live-path verification](ALEXA_LIVE_PATH_VERIFICATION.md). The local dashboard is a separate Alexa-style MCP demonstration client, not production Alexa+.
6. **Local Persistence & Retention:** Alert metadata (timestamps, rule IDs, mathematical evidence) is stored in local SQLite with age-based retention and migration. This sensor-pipeline policy is distinct from explicit local operator-upload storage.

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
- **No Raw Sensor Media over AWS:** The ASK mailbox carries aggregate read-only status/alerts through AWS, not raw sensor media. Detector demos use disclosed synthetic inputs; certified monitoring is not claimed.
- **Revision-scoped verification:** Historical candidate test totals are not current HEAD acceptance. Reproduction commands are in [the demonstration guide](DEVPOST_DEMONSTRATION_GUIDE.md); current acceptance evidence and limitations are linked above.
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

1. **Submission Track & Positioning:** This entry uses the **Alexa+ track self-hosted MCP route**: native C++ MCP 2025-11-25 over Streamable HTTP. Separately, the working **classic ASK hosted skill → AWS SQS → Oracle private worker → Android bridge → native C++ tools** lane has owner-attested voice replies and independent mailbox/native verification. This is not production Alexa+ integration, Echo hardware acceptance, or certified monitoring.
2. **Self-Hosted Native MCP Server:** We have built and verified an authoritative, native C++ Model Context Protocol server (spec 2025-11-25, Streamable HTTP, read-only tools). It runs locally on loopback and is not an Amazon-hosted runtime.
3. **Synthetic Evaluation Boundary:** The reproducible detector/demo/regression inputs and test-pattern viewer are synthetic (`camera-syn-001`). Unverified physical cameras fail closed. Explicit operator-upload playback is locally stored, simulated playback, not live camera capture or detection. An optional owner-controlled private-dashboard live feed is pending verification; it is not claimed implemented or exposed publicly.
4. **No Ring Integration / No Disaster Forecasting:** Ring integration, automated emergency 911 dispatch, and validated physical disaster predictions are not implemented and are not claimed.
5. **Open Source Verification:** All original code is licensed under the Apache License, Version 2.0. Clean-clone build and reproduction instructions are provided in `docs/DEVPOST_DEMONSTRATION_GUIDE.md`.

### Historical Physical Sensor Investigation (Separate from PR #20 Runtime)

- **Genuine USB UVC motion observed on external experiment branch:** On isolated branch `experiment/hg-v380-09-secure-camera-integration` (commit `b48254121474cb6eee69a63e9c39a73e90c96d35`), an owner-authorized USB UVC camera delivered 600 real frames under explicit operator consent (`--confirm`), where a sustained increase in motion in view was observed (peak motion 0.1252 against 0.0103 still-scene ceiling, confidence 0.849). Bounded numerical scalars only — zero frames stored, transmitted, or streamed.
- **Historical PR #20 candidate boundary:** That candidate used synthetic runtime observations (`camera-syn-001`), not physical camera integration. This is not a statement that the current ASK transport is simulated.
- **A real camera → fusion → delivered alert is NOT verified and not claimed:** Replaying the recorded scalars through the real rule and fusion engine is inconclusive (agreement 0.5913 vs 0.60 threshold). No alert from real frames has been observed, and no threshold was changed to manufacture one.
- **Animal or species recognition is not implemented:** There is no species classifier or animal model runtime, and no document claims one.
- **Camera-source code versus viewing acceptance:** Native HTTP and bubble-protocol camera adapters are now present on main. Offline parser/authentication tests do not prove private-dashboard playback or physical sensor-to-alert acceptance; those remain pending the camera worker's verified handoff.
- **Disaster prediction is not demonstrated:** Not implemented, and unrepresentable in the fusion framework.

### Honest Hardware & Deployment Limitations

We believe absolute transparency is the cornerstone of responsible engineering. We state the exact hardware boundaries of physical devices and infrastructure:

- **Android node:** The existing private ASK lane reaches native tools through the Android bridge; see [physical acceptance](ANDROID_CPP_MCP_PHYSICAL_ACCEPTANCE.md) and [live-path evidence](ALEXA_LIVE_PATH_VERIFICATION.md). Capture remained disabled, and stale-session/reboot/network recovery limitations remain open. This is transport evidence, not physical sensor-to-alert acceptance.
- **Panasonic Eluga (CCTV Handset):** A separate legacy Android 8.1 / API 28 device tested for auxiliary edge monitoring in lab staging; not provisioned with judge credentials and completely decoupled from the candidate runtime.
- **LAN camera sources:** Native HTTP and bubble-protocol adapters are available; private-dashboard live viewing remains pending verification. The reproducible detector demo does not require a physical camera.
- **Alexa hardware/publication:** Classic ASK hosted-skill replies are owner-attested; production Echo hardware acceptance, production Alexa+, and skill-store publication are not claimed.
- **Private versus public access:** The existing Oracle worker uses a private path; no public MCP port or public camera stream is claimed. The synthetic judge demo remains reproducible over loopback without cloud dependencies.

---

## 4. Product Feedback for Hackathon Organizers & Tool Builders

### Model Context Protocol (MCP) Specification (2025-11-25)
- **Strengths:** The Streamable HTTP transport specification provides a clean, modern alternative to stdio-only bridges, enabling remote or containerized AI agents to interact with IoT hubs over standard HTTP.
- **Opportunities:** Explicit security guidance regarding session-to-credential binding and prevention of session adoption in multi-tenant environments would benefit the ecosystem. Our implementation adds explicit principal validation to address this.

### Alexa Skills Kit & Alexa+ Developer Experience
- **Strengths:** The ASK SDK v2 provides a well-structured request/response contract. The mock-driven dispatcher enables rapid local unit testing without cloud dependencies.
- **Opportunities:** Providing standardized local testing emulators for next-generation Alexa+ agentic interactions would significantly lower the barrier for independent developers and edge-first privacy researchers.
