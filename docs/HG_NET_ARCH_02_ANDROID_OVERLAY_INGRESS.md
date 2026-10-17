# HomeGuardian AI — Android Overlay Ingress & Cross-Device Security Architecture (HG-NET-02D)

## 1. System Topology & Role Separation

HomeGuardian AI operates on a tiered, zero-trust network topology designed to support edge inferencing on physical hardware while preserving strict isolation between control plane, management gateway, and sensory peripherals.

```
+-----------------------------------------------------------------------------------------+
|                                    PRIVATE OVERLAY NETWORK                              |
|                                       (WireGuard VPN)                                   |
|                                                                                         |
|   +----------------------------+                     +-------------------------------+  |
|   |      Xiaomi Mi A3          |                     |       Ubuntu Dev / Gateway    |  |
|   | (Android 11 / ARM64 Edge)  |<===================>|        (Host / Node UI)       |  |
|   |  - Native C++ Engine       |   Private Overlay   |  - Web Gateway / Dashboard    |  |
|   |  - Native C++ MCP Server   |   (e.g., 10.42.0.2) |  - Authoritative Registry CLI |  |
|   |  - Loopback / VPN Bind     |                     |  - Adaptive Demo Ingress      |  |
|   +----------------------------+                     +-------------------------------+  |
|                 ^                                                    ^                  |
+-----------------|----------------------------------------------------|------------------+
                  |                                                    |
                  |                +-----------------------+           |
                  +===============>|    Oracle VPS Pilot   |<==========+
                                   |   (Coordination Node) |
                                   |  - WireGuard Hub      |
                                   |  - Zero Public Ingress|
                                   |  - Mail Svc Protected |
                                   +-----------------------+
```

### Host Roles & Boundaries
1. **Primary Always-On Host (Xiaomi Mi A3 — `laurel_sprout`)**:
   - **OS / Hardware**: Android 11, Qualcomm Snapdragon 665, 4GB RAM, ARM64 (`aarch64-linux-android`).
   - **Responsibilities**: Runs native C++ HomeGuardian engine (`hg_mcp_server`), performs local anomaly detection, serves read-only Streamable HTTP MCP tools (`get_homeguardian_status`, `get_alert_summary`).
   - **Isolation**: No root required. Execution anchored in sandboxed runtime directory (`/data/local/tmp/hg_edge`).
2. **Development / Demonstration Gateway (Ubuntu Host / Node.js)**:
   - **Responsibilities**: Serves judge dashboard, operator session management, WebRTC signaling, and MCP conversational client.
   - **Ingress Strategy**: Employs adaptive detection—directly proxies authenticated queries to the Mi A3 when reachable over overlay/ADB forward, or falls back to an explicitly labeled local synthetic C++ fixture when unreachable.
3. **VPN Coordination Node (Oracle VPS)**:
   - **Responsibilities**: Acts purely as a point-to-point WireGuard routing hub between authorized peers.
   - **Strict Constraint**: Zero modifications to public firewall ports, production DNS, or mail server certificates. No raw camera streaming or application data stored on VPS.

---

## 2. Ingress Security & Zero-Trust Bind Policy

### The Wildcard Bind Vulnerability (`0.0.0.0` / `::`)
In a multi-homed mobile environment, binding a service to `0.0.0.0` or `INADDR_ANY` introduces critical vulnerabilities:
- Unsolicited exposure on untrusted public Wi-Fi networks (`wlan0`).
- Exposure on cellular carrier interfaces (`rmnet_data0`), which may have routable public IPv6 addresses or shared CGNAT pools.
- Potential circumvention of firewall controls on Android interfaces.

### Enforced Bind Rules (C++ Transport Hardening)
In `McpHttpServer.cpp`, binding to wildcard addresses is unconditionally prohibited at startup, regardless of whether `allow_remote` is configured:
```cpp
if (impl_->cfg.bind_address == "0.0.0.0" || impl_->cfg.bind_address == "::" || impl_->cfg.bind_address == "0") {
    error = "refusing to bind wildcard address (0.0.0.0); explicit interface or loopback required";
    return false;
}
if (!impl_->cfg.allow_remote && !is_loopback_literal(impl_->cfg.bind_address)) {
    error = "refusing to bind a non-loopback address without allow_remote";
    return false;
}
```
**Consequence**: The MCP daemon will exit with code `4` if configured with a wildcard address. It will only bind to `127.0.0.1` (default) or an explicit, administrator-designated private overlay IP address (e.g., `10.42.0.2`).

### Defense-in-Depth: Network vs. Application Authentication Separation
Network layer encryption (WireGuard VPN) does **not** replace application authentication. Even within the private overlay tunnel:
1. **Bearer Token Authentication**: Every MCP handshake requires an `Authorization: Bearer <token>` header verified against a local `0600`-permissioned credential file (`mcp_credentials.json`).
2. **CSPRNG Session Binding**: The server mints a unique 256-bit session token (`Mcp-Session-Id`) on `initialize`. All subsequent calls must provide this session header. Cross-principal session theft attempts return `403 Forbidden`.
3. **Origin Allowlisting**: If an `Origin` header is transmitted by a browser client across the overlay, it is strictly validated against the server's explicit origin allowlist (`HG_MCP_ORIGINS`), neutralizing DNS rebinding and cross-site scripting risks.
4. **Read-Only Confinement**: The MCP protocol surface is strictly confined to read-only status and alert reporting (`get_homeguardian_status`, `get_alert_summary`). Mutating tool calls fail closed with RPC code `-32602`.

---

## 3. Android Runtime Constraints: Inbound vs. Outbound Overlay

### Constraint A: Non-Root Android `VpnService` & Interface Isolation
- On unrooted Android 11, VPN applications leverage the Android `VpnService` API, creating a virtual `tun` device.
- Traffic routed into `tun` by the Android kernel is subject to Android per-app network permissions and SELinux policies (`isolated_app` vs `shell` vs `untrusted_app`).
- Background processes spawned via ADB shell (`u:r:shell:s0`) operate in a different network namespace/routing context than applications running in user profiles.
- Unsolicited inbound TCP SYN packets targeting non-standard ports (e.g., `:8790`) on the VPN interface are frequently dropped or rejected by Android packet filters unless explicit routing rules are injected.

### Constraint B: Doze Mode & Background Power Management
- Android aggressively terminates or suspends socket listeners when the device enters Doze mode:
  - **Light Idle** (screen off, battery power): Activated after ~3 minutes of inactivity. Network access is restricted to maintenance windows.
  - **Deep Idle** (device stationary on battery): Activated after ~30 minutes. Network access is fully suspended, CPU enters deep sleep, and incoming TCP handshakes timeout.
- While ADB-tethered development keeps the phone awake (`wake_lock`), an autonomous, battery-operated edge deployment requires an architecture resilient to sleep cycles.

### Architecture Comparison: Direct Inbound vs. Outbound Fallback

| Architectural Dimension | Direct Inbound Ingress (Over Overlay) | Authenticated Outbound-First / Reverse Proxy |
| :--- | :--- | :--- |
| **Connection Direction** | Controller/Browser initiates TCP SYN -> Phone | Phone initiates TLS/VPN connection -> Gateway |
| **Android Root Required** | No (with explicit VPN interface binding) | No |
| **NAT / Firewall Traversal** | Requires static overlay routing & IP reachability | Naturally traverses NAT, CGNAT, and Wi-Fi firewalls |
| **Doze Mode Behavior** | Drops incoming SYN when phone enters light/deep idle | Phone wakes during scheduled maintenance window or persistent socket keepalive |
| **Zero-Trust Posture** | Requires strict local port filtering on phone | Phone opens zero open inbound listening ports |

**Design Conclusion**: Direct inbound MCP listening is supported for local debugging (via loopback or ADB port forwarding `adb forward tcp:18992 tcp:8790`) and over point-to-point overlay interfaces where keepalives prevent socket teardown. For production edge deployments subject to battery management, an outbound multiplexing or message-polling architecture is recommended to prevent Doze disruption.

---

## 4. Adaptive Demo Gateway Implementation

To provide seamless verification for reviewers and hackathon judges regardless of whether physical Android hardware is actively connected to the testing machine, the HomeGuardian Gateway implements an **Adaptive Demo Ingress Mode**:

```javascript
// web_mcp/server.js adaptive resolution logic
const remoteEndpoint = process.env.HG_MI_A3_MCP_ENDPOINT || process.env.HG_MCP_ENDPOINT;

if (remoteEndpoint) {
  const probe = await probeMcpEndpoint(remoteEndpoint, targetToken);
  if (probe.ok) {
    // Verified real edge hardware
    activeRuntime = {
      mode: 'hardware',
      target: 'Xiaomi Mi A3 (Android 11 / ARM64)',
      verified: true,
      endpoint: remoteEndpoint,
    };
  } else {
    // Graceful fallback to verified local native C++ fixture
    activeRuntime = {
      mode: 'synthetic_fixture',
      target: 'Local Synthetic Fixture (Mi A3 Offline)',
      verified: false,
      endpoint: MCP_URL,
    };
  }
}
```

### Strict Honesty Invariant
Under no circumstances does the system claim the Mi A3 is online or active if it cannot complete a cryptographically valid MCP handshake (`initialize` -> `2025-11-25`). 
- When Mi A3 is offline, the judge dashboard displays:
  - Header: `⚠️ LOCAL SYNTHETIC FIXTURE (MI A3 OFFLINE)`
  - Badge: `○ FIXTURE: Local Synthetic Fixture (Mi A3 Offline) (2025-11-25)`
  - Verification: `hardwareVerified: false`
- When Mi A3 is online:
  - Header: `🛡️ AUTHENTICATED EDGE RUNTIME: Xiaomi Mi A3 (Android 11 / ARM64)`
  - Badge: `● EDGE: Xiaomi Mi A3 (Android 11 / ARM64) (2025-11-25)`
  - Verification: `hardwareVerified: true`

---

## 5. Verification Commands & Regression Suites

The following test suites assert all overlay security invariants and can be executed independently:

```bash
# 1. Overlay Ingress Security & Wildcard Rejection Suite (24 tests)
bash scripts/test_mcp_overlay.sh

# 2. C++ Unit Tests (Catch2, 33,100+ assertions including wildcard tests)
./build/homeguardian_tests "[mcp]"

# 3. Streamable HTTP MCP Protocol & Session Binding Suite (73 tests)
bash scripts/verify_mcp.sh

# 4. Full Gateway & Peripheral Dashboard Suite (161 tests)
node web_mcp/test_devices_dashboard.js

# 5. Full End-to-End Synthetic Verification (187 checks)
bash scripts/verify_synthetic_mcp_demo.sh
```
