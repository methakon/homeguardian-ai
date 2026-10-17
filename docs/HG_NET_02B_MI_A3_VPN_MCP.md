# HG-NET-02B (Issue #15) — Mi A3 always-on MCP over private VPN: findings

**Device:** Xiaomi Mi A3, `laurel_sprout`, Android 11 / API 30, arm64-v8a
**Branch:** `experiment/mi-a3-edge-service` (base `cca7716`)
**Transport:** wireless ADB, USB absent
**Issue:** methakon/homeguardian-ai#15 · dependency: #14 (Hermes 1, Oracle WireGuard hub)

## 1. Headline

Present-day service operation and the MCP authentication surface are **verified**. The
VPN-remote access path is **BLOCKED**, and the blocker is now **proven on hardware rather than
assumed** — which is the single most important result here, because it determines the architecture.

| milestone | status |
|---|---|
| 1. Oracle WireGuard service running | **NOT MINE** — Issue #14 (Hermes 1), still open |
| 2. Mi A3 VPN handshake established | **BLOCKED** on #14 |
| 3. Authenticated remote MCP request works | **BLOCKED** — needs #14 *and* a token decision (§5) |
| 5. Screen-off / reboot / network-recovery acceptance | **PARTIAL** — see §6 |

## 2. The critical finding: inbound VPN traffic cannot reach this MCP server

The issue warns "do not assume the loopback-only listener is remotely accessible". Here is the
measurement, taken on the device:

    (a) curl http://127.0.0.1:8790/            -> http=404     (the listener answers)
    (b) curl http://<device-LAN-address>:8790/ -> http=000     CONNECTION REFUSED
    (c) /proc/net/tcp                          -> 0100007F:2256  LISTEN   (127.0.0.1 only)
        non-loopback bindings for port 8790    -> 0

**Why this is decisive and not a LAN quirk.** A socket bound to `127.0.0.1` accepts only
connections whose *destination* is `127.0.0.1`. A packet arriving through a VPN/TUN interface has
a destination of the **VPN address**, never `127.0.0.1`. Test (b) demonstrates exactly that
mechanism using the LAN address as a stand-in for any non-loopback destination: the same server,
the same port, same host, and the connection is refused purely because the destination address
differs.

Therefore: **no amount of VPN configuration will deliver inbound connections to this MCP server.**
The kernel-level binding check (c) closes the argument — there is no non-loopback binding to reach.

The obvious "fix" — rebinding the listener to `0.0.0.0` — is **forbidden** by the brief ("no
unrestricted network listening", "no public MCP endpoint"). That leaves exactly one shape that
satisfies both constraints, which is why the issue anticipated it.

## 3. Required architecture: an outbound authenticated control bridge

Since inbound is impossible and wide binding is forbidden, control must be **dialled out**. The
bridge is the architecture, not a convenience fallback:

- **Outbound-initiated only.** The bridge opens a connection to the authorized hub over the VPN and
  never listens on any interface. No public endpoint, no Wi-Fi-wide listener, nothing to scan.
- **Runs inside the existing `EdgeService`** so it inherits the verified foreground-service lifetime
  and survives screen-off. It must not depend on the activity.
- **Relays to the loopback MCP server as a client.** The local MCP stays bound to `127.0.0.1:8790`
  and keeps its own bearer auth; the bridge is an *additional* authenticated peer, not a
  replacement for it. Application authentication and tunnel encryption stay independent — an
  encrypted tunnel does not substitute for the HG-SEC-06D auth work, exactly as the issue states.
- **Bounded and fail-closed.** Hard frame-size cap validated before allocation; bounded request
  queue; a slow or vanished hub must shed requests, not grow memory; no request is executed that
  fails authentication.
- **Device identity from a durable key, not an address.** Peer identity comes from the WireGuard
  keypair plus the app-layer authentication. IPv4/IPv6 addresses are observations only, never
  identity — the brief forbids hardcoding development LAN addresses as identity.
- **Independently armed.** Starting the bridge must not imply opening camera or microphone, and
  neither sensor should open on a bridge start.

**Not implemented in this pass.** The bridge is untestable end-to-end until Issue #14 provides a hub
to dial into, and claiming a working bridge against no peer would be exactly the false claim the
issue warns against. §7 gives the concrete next steps.

## 4. Verified present-day (not historical)

The issue states that historical PDF logs do not establish present-day availability. These
measurements are from this session, on the running device.

**Device identity and platform integrity**

    model Mi A3 | device laurel_sprout | Android 11 | API 30 | arm64-v8a
    verifiedbootstate=green | flash.locked=1        (verified boot intact, bootloader locked)

This is the genuine Mi A3 — not the unrelated MT6580 clone — and it is unrooted, un-flashed, with
ordinary Android 11.

**Service**

    package          org.homeguardian.edgeservice  versionName 1.0.0  versionCode 1
                     minSdk 30  targetSdk 30   uid 10226   debuggable=NO (production)
    foreground       EdgeService isForeground=true  foregroundId=4711  with notification
    boot startup     BootReceiver for BOOT_COMPLETED and MY_PACKAGE_REPLACED
    uptime           11h 54m continuous, same pid throughout this audit (26444)

A non-debuggable production build is the correct posture: it is why the bearer token cannot be read
over adb (§5).

**MCP authentication surface** — exercised from the host through `adb forward`, production build:

    GET     /mcp -> 401        POST   /mcp -> 401
    DELETE  /mcp -> 401        OPTIONS /mcp -> 401
    POST with a wrong bearer  -> 401
      headers: HTTP/1.1 401 Unauthorized / WWW-Authenticate: Bearer

The rejection is corroborated **on the device by the engine's own log**:

    [hg_edge] [warning] mcp: rejected an unauthenticated request

**Authenticated exchange** — the service's own watchdog, observed live every 20 s:

    PROBE PROBE_OK 27ms proto=2025-11-25 tools=get_homeguardian_status,get_alert_summary
        status={"capture_enabled":false,"read_only":true,"state":"Running"}

Thirteen consecutive `PROBE_OK` at 20 s intervals, 17–28 ms each, over the real token, exercising
`initialize` → `tools/list` → `tools/call`. This establishes that the **authenticated request path
works today**, and that the engine correctly reports `capture_enabled:false, read_only:true` rather
than claiming capture.

**Power and thermal**

    battery 100%, AC powered, health GOOD, 33.9 C

## 5. Blocker: the remote peer needs a credential I must not extract

Milestone 3 requires an *independent VPN peer* to make authenticated MCP calls. That peer needs the
bearer token, and the token is:

- generated on-device with `SecureRandom` into `files/mcp_credentials.json`, mode 0600,
  **never displayed and never logged**, with only a SHA-256 **fingerprint** exposed to identify it;
- unreadable via `adb shell` in the production build — confirmed first-hand: `run-as` reports
  `package not debuggable`.

Three routes exist, and all three need a decision that is not mine to make unilaterally:

1. **The `HG_EDGE_DEBUGGABLE=1` test build.** Documented as the sanctioned path for full host-side
   authenticated testing, and it "must never be the deployed artifact". It does not exist yet — the
   build script has no such switch. Building *and installing* it means replacing the production
   artifact on the designated always-on host, which is a production change.
2. **The owner provisions the token to the peer** through whatever process they intend.
3. **The app gains an operator-visible provisioning step** — a deliberate design change to the
   security model, not a testing convenience.

I stopped here rather than taking route 1 unilaterally. The brief permits stopping for "an unsafe
production change [or] unresolved secrets", and installing a debuggable variant over the production
build on the always-on host is both. **This is the one owner interaction I am requesting.**

## 6. Screen-off / reboot / recovery — partial, and what is missing

- **Screen-off: previously PASSED** for service lifetime and MCP staying loopback-only
  (`docs/EDGE_WIRELESS_DEVELOPMENT_MI_A3.md` §9), and the 11h54m continuous uptime above
  corroborates it. I did **not** re-run a screen-off test in this pass.
- **Reboot recovery: NOT TESTED.** The `BootReceiver` for `BOOT_COMPLETED` is configured, but
  configuration is not evidence. Testing it means rebooting the always-on host, which interrupts the
  service the owner is relying on; it should be scheduled, not done casually.
- **Network recovery: NOT TESTED here.** The documented §9 result is PARTIAL — a Wi-Fi drop needs
  patience or a phone action, and the wireless ADB port changes on reconnect (41639 → 40815).
- **24-hour soak: NOT STARTED.** Required before any 24/7 claim.
- **Charging safety is unresolved:** the device is on AC at 100%. Continuous unattended charging at
  100% has not been approved, and the issue defers 48–72 h runs until charging safety is cleared.

## 7. Concrete next steps, in order

1. **Owner decision on §5** — debug build, token provisioning, or neither. Nothing in milestone 3
   can proceed without it.
2. **Issue #14 lands the hub** (Hermes 1). Until then there is no peer to dial.
3. **Implement the outbound bridge** inside `EdgeService` per §3, with: outbound-only socket, hard
   frame cap before allocation, bounded queue, per-request authentication, no payload logging, and
   independent arming from the camera/microphone.
4. **Then** test from an independent VPN peer: `initialize`, `tools/list`, `tools/call`,
   unauthenticated rejection, status/alert read-back — verifying the request path and auth
   semantics, not merely reachability.
5. **Then** the scheduled acceptance runs: reboot recovery, Wi-Fi roam, screen-off with the
   workstation disconnected, Doze/app-standby, and a supervised 24 h soak.

## 8. Reproducing this pass

    M=<device-transport>
    adb -s $M shell getprop ro.product.model          # Mi A3
    adb -s $M shell getprop ro.boot.verifiedbootstate # green
    adb -s $M shell pm list packages | grep homeguardian
    adb -s $M shell cat /proc/net/tcp | grep 2256     # 0100007F = loopback only
    adb -s $M shell 'ip -4 addr show wlan0'           # read the LAN address for the binding test
    adb -s $M shell "curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8790/"
    adb -s $M shell "curl -s -o /dev/null -w '%{http_code}' http://<LAN>:8790/"   # expect 000
    adb -s $M forward tcp:18790 tcp:8790
    curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:18790/mcp      # expect 401
    adb -s $M logcat -d -s HGEdge | tail                # PROBE_OK + rejection notices

## 9. Scope compliance

No root, no reset, no flash, no partition change; verified boot and the bootloader lock untouched.
The MCP listener stays loopback-only and no port forward, tunnel or LAN exposure was introduced. No
change to Oracle, mail services or certificates, Cloudflare records, V380 devices, or any
release-security branch. No camera or microphone capture was started; `capture_enabled:false`
throughout. No credential, token or fingerprint value is recorded in this document.
