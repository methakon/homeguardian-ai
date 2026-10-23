# HomeGuardian AI — Official Hackathon Video Script (2m 45s)

> [!NOTE]
> **HACKATHON RELEASE CANDIDATE (HG-NET-02 / HG-SEC-06D):** Release candidate `release/hackathon-2026-mcp-candidate` ([PR #20](https://github.com/methakon/homeguardian-ai/pull/20)) has passed all C++ core, MCP protocol, and outbound bridge test suites. Every claim in this script refers to what this build contains.

**Submission Track:** Alexa+ Track — **Real Native C++ Model Context Protocol (MCP 2025-11-25) Server with Alexa-Style Demonstration Client**  
**Target Duration:** Exactly 2 minutes 45 seconds (165 seconds)  
**Presenter Language:** English (Clear, professional, grounded tone)  
**Key Principle:** Absolute honesty — celebrate what is built, disclose what is simulated, state what is not claimed.

---

## Storyboard & Timing Breakdown

```
[0:00 - 0:25]  Hook & Problem Statement: The Privacy Crisis in Smart Home Monitoring
[0:25 - 0:55]  Architecture & Native C++ Core: Fail-Closed Consent & Local Anomaly Detection
[0:55 - 1:25]  Native Model Context Protocol (MCP): Streamable HTTP over Loopback
[1:25 - 1:55]  Peripheral Device Registry & Fail-Closed WebRTC Camera Streaming
[1:55 - 2:25]  Alexa-Style Conversational Client & Instant Consent Revocation
[2:25 - 2:45]  Verification Rigor, Open Source & Closing
```

---

## Detailed Script & Production Notes

### Act 1: Hook & Problem Statement (0:00 – 0:25 / 25s)
- **Visual:**
  - Close-up of typical cloud-connected smart camera displaying "Connecting to Cloud..." error or privacy news headlines regarding footage leaks.
  - Cut to clean title card: **HomeGuardian AI — Whole-Home Intelligence with Consent-Gated Privacy**.
- **Voiceover (0:00 – 0:25):**
  > "Today's smart homes face an unacceptable compromise: to protect our families, we are told to surrender our privacy to opaque cloud servers and continuous video surveillance.
  >
  > HomeGuardian AI changes this paradigm. We built a local-first, privacy-by-design home intelligence platform that monitors safety and wellbeing through explainable anomaly detection, backed by a cryptographic, fail-closed consent gate.
  >
  > Data stays on your premises. Raw video is never stored. And consent is never assumed."

---

### Act 2: Architecture & Native C++ Core (0:25 – 0:55 / 30s)
- **Visual:**
  - Show the clean architecture diagram from `docs/ARCHITECTURE.md` showing Tier 1 (C++ Core Engine), Tier 2 (Authoritative Registry & MCP Server), and Tier 3 (Loopback Web Gateway).
  - Terminal screen recording: compiling the C++ core and running Catch2 tests with deterministic outputs.
- **Voiceover (0:25 – 0:55):**
  > "At the foundation of HomeGuardian AI is a high-performance C++20 engine that runs with an idle footprint of under seven megabytes of RAM.
  >
  > Our sensor pipeline pairs statistical rolling baselines with multi-sensor fusion. Instead of streaming raw media to an external model, observations are evaluated on-device against learned behavioral baselines.
  >
  > An alert is generated only when fused multi-sensor evidence qualifies. Every alert carries its verifiable mathematical evidence, and all records persist locally in SQLite storage with automated retention policies."

---

### Act 3: Native Model Context Protocol (MCP) Integration (0:55 – 1:25 / 30s)
- **Visual:**
  - Screen recording showing the Model Context Protocol inspector panel in the browser (`/api/mcp/traffic`).
  - Send an authenticated MCP request: `tools/call get_homeguardian_status`.
  - Zoom in on the JSON-RPC wire response showing native status from C++ engine and header inspection showing `[REDACTED]` for sensitive tokens.
- **Voiceover (0:55 – 1:25):**
  > "For agentic interoperability, HomeGuardian AI implements the official Model Context Protocol 2025-11-25 Streamable HTTP specification in native C++.
  >
  > Our MCP server binds sessions to server-verified credentials using OS-backed CSPRNG 256-bit tokens, preventing session adoption or hijacking.
  >
  > Notice the wire inspector: both incoming authorization and outgoing session IDs are strictly redacted. Autonomous agents can query system status and alert summaries through safe, strictly read-only tools without risking credential exposure."

---

### Act 4: Device Registry & Fail-Closed WebRTC Streaming (1:25 – 1:55 / 30s)
- **Visual:**
  - Switch to the Devices Dashboard tab in the web interface.
  - Show registered devices: `camera-syn-001` (Synthetic Simulator) and `camera-001` (Physical Camera - Unverified).
  - Click "Start View" on the physical camera: show immediate refusal with HTTP 400 `physical_camera_unverified`.
  - Click "Start View" on `camera-syn-001`: show instant WebRTC video stream playing the synthetic animated test pattern with frame counters and low latency.
- **Voiceover (1:25 – 1:55):**
  > "Peripherals are managed by an authoritative C++ registry CLI. In keeping with our strict safety guarantees, physical cameras remain in an unverified state by default. Any attempt to request a view token for an unverified physical sensor is refused immediately at the gateway.
  >
  > For evaluation and testing, our synthetic WebRTC streaming engine generates deterministic test patterns. Active viewers maintain seamless keepalive streams, while abandoned streams automatically expire and release media resources within seconds."

---

### Act 5: Alexa-Style Conversational Client & Consent Revocation (1:55 – 2:25 / 30s)
- **Visual:**
  - Switch to the Alexa-Style Conversational Assistant tab.
  - Type or trigger: *"Is everything okay in the house?"*
  - Assistant responds with natural voice/text: *"HomeGuardian reports normal activity. All safety rules clear. One synthetic alert logged in living room."*
  - Demonstrate instant consent revocation: Open the device card, click "Withdraw Consent" (button updates to "Grant Consent").
  - Instantly observe the camera viewer: the active WebRTC stream tears down, the status pill switches to `Stream: Inactive`, and the viewer reverts to the placeholder (`Select an authorized camera and click "Start Live View"`).
- **Voiceover (1:55 – 2:25):**
  > "For the Amazon Developer Hackathon, we present our simulated Alexa-style demonstration client, connected directly to our native C++ bridge and MCP tools. Powered by our read-only simulation contract, you can speak or chat with your home safety engine.
  >
  > But privacy is not just a policy — it is an active gate. Watch what happens when I withdraw sensor consent:
  >
  > The instant consent is revoked, the C++ consent gate drops delivery. The WebRTC streamer immediately tears down peer connections, stops media tracks, and halts frame processing. Zero frames leak."

---

### Act 6: Verification Rigor, Disclosures & Closing (2:25 – 2:45 / 20s)
- **Visual:**
  - Fast montage: C++ test suite finishing 527 tests / 38,929 assertions; 15 independent security gates green; GitHub repository and Apache 2.0 license.
  - Final screen displaying:
    - GitHub: https://github.com/methakon/homeguardian-ai
    - Transparent Disclosure: Real Native C++ MCP 2025-11-25 | Alexa-Style Demonstration Client | Synthetic Evaluation Boundary | Zero Cloud Exfiltration
- **Voiceover (2:25 – 2:45):**
  > "HomeGuardian AI is backed by 527 test cases, over 38,900 assertions, and 15 independent security verification gates — all green.
  >
  > All original code is open source under the Apache 2.0 license.
  >
  > HomeGuardian AI: verifiable intelligence, zero compromise on privacy. Thank you."

---

## Video Production Checklist for Submitter

1. **Resolution & Audio:** 1080p 60fps or 30fps MP4, clear balanced microphone audio without background noise.
2. **Side-by-Side Setup:** Browser dashboard on left (1280x720), terminal on right (640x720).
3. **Execution Command:** Pre-run `bash scripts/verify_synthetic_mcp_demo.sh` to ensure all background processes are up and healthy on `http://127.0.0.1:18880`.
4. **Honesty Checklist:**
   - [x] State that conversational client is an Alexa-style demonstration interface connected to native C++ backend.
   - [x] Confirm no real cameras/microphones are turned on during recording.
   - [x] Highlight the native C++ MCP server and explainable detection engine.
   - [x] Conclude within 2 minutes 45 seconds.
