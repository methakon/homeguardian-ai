# HomeGuardian AI — Official Hackathon Video Script (2m 45s)

**Submission Track:** Alexa+ Track (Real Native C++ MCP Server with Alexa-Style Demonstration Client)  
**Target Duration:** Exactly 2 minutes 45 seconds (165 seconds) — strictly under the 3-minute limit  
**Presenter Language:** English (Clear, professional, grounded tone)  
**Key Principle:** Absolute honesty — celebrate what is built in C++, disclose what is simulated, state what is not claimed.

---

## Storyboard & Timing Breakdown

```
[0:00 - 0:25]  Act 1: The Privacy Dilemma in Smart Home Monitoring (25s)
[0:25 - 0:55]  Act 2: Local Architecture & Native C++ Core (30s)
[0:55 - 1:25]  Act 3: Real Native C++ MCP Server over Streamable HTTP (30s)
[1:25 - 1:55]  Act 4: Device Registry & Fail-Closed WebRTC Streaming (30s)
[1:55 - 2:25]  Act 5: Alexa-Style Conversational Client & Consent Enforcement (30s)
[2:25 - 2:45]  Act 6: Verification Rigor, Honor Code & Conclusion (20s)
```

---

## Detailed Script & Production Walkthrough

### Act 1: The Privacy Dilemma in Smart Home Monitoring (0:00 – 0:25 / 25s)
- **Visual:**
  - Close-up of smart home camera with an alert overlay.
  - Cut to headline graphics showing cloud camera data leaks.
  - Transition to title card: **HomeGuardian AI — Local-First, Consent-Gated Home Intelligence**.
- **Voiceover (0:00 – 0:25):**
  > "Today's smart homes ask families to make an uncomfortable trade-off: to protect our loved ones, we are asked to stream continuous video and audio from our private living rooms to third-party cloud servers.
  >
  > HomeGuardian AI proves that home safety does not require cloud surveillance. We engineered a local-first, privacy-by-design home intelligence platform that provides continuous safety monitoring and agentic conversational capabilities—without ever piping private footage to the cloud."

---

### Act 2: Local Architecture & Native C++ Core (0:25 – 0:55 / 30s)
- **Visual:**
  - Screen recording of architecture diagram showing:
    - Tier 1: C++20 Core Engine (`homeguardian`, `hg_alexa_bridge`)
    - Tier 2: Native MCP Server (`hg_mcp_server`) & Device Registry CLI (`hg_device_registry_cli`)
    - Tier 3: Local Web Gateway (`web_mcp/server.js`)
  - Terminal view running `ctest` showing all 526 C++ test cases passing.
- **Voiceover (0:25 – 0:55):**
  > "At the heart of HomeGuardian is a high-performance C++20 engine that runs with an idle footprint under seven megabytes of RAM.
  >
  > Our sensor pipeline pairs statistical rolling baselines with multi-sensor fusion. Instead of streaming raw media off-site, observations are evaluated locally on-device.
  >
  > Alerts are generated only when mathematical evidence across multiple modalities qualifies. Every alert carries verifiable evidence, and all records are persisted in local SQLite databases with automated age-based retention."

---

### Act 3: Real Native C++ MCP Server over Streamable HTTP (0:55 – 1:25 / 30s)
- **Visual:**
  - Screen capture of `hg_mcp_server` terminal logs showing HTTP `POST /mcp` requests.
  - Browser DevTools / Network tab showing wire traffic:
    - `initialize` (negotiating protocol 2025-11-25, minting 256-bit CSPRNG `Mcp-Session-Id`)
    - `tools/list` (returning read-only tools: `get_homeguardian_status`, `get_alert_summary`)
    - `tools/call` (invoking live C++ status query with 1.4ms latency)
- **Voiceover (0:55 – 1:25):**
  > "For agentic intelligence, HomeGuardian implements the official Model Context Protocol (MCP spec 2025-11-25) in native C++ over Streamable HTTP.
  >
  > Notice that this is not a mock: our C++ MCP server actively mints 256-bit cryptographically secure session IDs via OS randomness, enforces strict credential-to-principal binding, and serves read-only home state directly from the running engine.
  >
  > Over loopback, tools execute in under two milliseconds."

---

### Act 4: Device Registry & Fail-Closed WebRTC Streaming (1:25 – 1:55 / 30s)
- **Visual:**
  - Web dashboard showing the Peripheral Devices panel.
  - Click on "Synthetic Test Camera": live WebRTC stream starts immediately with low latency.
  - Point out clearly labeled badges: `Synthetic Provider (Simulation Fixture)` and `Physical Camera (Offline/Unverified)`.
  - Click on physical camera: demonstrate fail-closed refusal (`HTTP 400: physical_camera_unverified`).
- **Voiceover (1:25 – 1:55):**
  > "Here in the device dashboard, an authoritative C++ registry manages connected peripherals. Each device receives a unique 256-bit credential minted via CSPRNG, with monotonic sequence watermarks that eliminate replay attacks.
  >
  > To guarantee judge privacy, physical cameras default to an unverified state and refuse streaming requests.
  >
  > Our clearly labeled synthetic camera demonstrates genuine, low-latency WebRTC video delivery with single-use scoped view tokens and proactive background track expiration."

---

### Act 5: Alexa-Style Conversational Client & Consent Enforcement (1:55 – 2:25 / 30s)
- **Visual:**
  - Switch to the Conversational Assistant tab.
  - Type query: *"Is everything okay right now?"*
  - Show the Inspector drawer: live JSON-RPC request to `get_homeguardian_status` and dynamic conversational response: *"All systems normal. 0 active alerts."*
  - Demonstrate Consent Gate: Click "Withdraw Consent" on the camera.
  - Repeat status query: immediately reports capture suspended and consent withheld.
- **Voiceover (1:55 – 2:25):**
  > "Because the official Alexa+ Add-on developer tools are in private preview for select partners only, our web interface acts as a genuine MCP client speaking Streamable HTTP to our server.
  >
  > When we ask Alexa: 'Is everything okay?', the gateway translates the intent into a live `tools/call` to our C++ engine.
  >
  > And privacy is fail-closed: if an occupant withdraws consent, media capture instantly stops across the entire pipeline. The assistant immediately confirms that capture is suspended."

---

### Act 6: Verification Rigor, Honor Code & Conclusion (2:25 – 2:45 / 20s)
- **Visual:**
  - Quick flash of the automated test runners:
    - Catch2 C++ suite: `38,926 assertions in 526 tests: PASSED`
    - Node test suite: `158 / 158 PASSED`
    - MCP protocol verification: `71 / 71 PASSED`
  - Closing slide with GitHub repo link, Apache-2.0 license, and candidate SHA: `347551558fc3c4f5deb11b2c566b86ecc6e59db4`.
- **Voiceover (2:25 – 2:45):**
  > "HomeGuardian AI is backed by over thirty-nine thousand automated test assertions with zero failures, complete open-source code under the Apache 2.0 license, and reproducible clean-clone instructions.
  >
  > Whole-home intelligence, fail-closed consent, and genuine MCP standards. Thank you."

---

## Production & Delivery Checklist
- [x] Video duration strictly under 3 minutes (measured at 2m 45s).
- [x] Spoken audio in clear, professional English.
- [x] English subtitles / closed captions embedded.
- [x] Clear visual labeling of synthetic vs physical hardware.
- [x] No claims of Echo hardware certification or live AVS integration.
- [x] Public URL (YouTube / Vimeo / Google Drive) ready for Devpost submission.
