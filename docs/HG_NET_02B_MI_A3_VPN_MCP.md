# HG-NET-02B (Issue #15) — Mi A3 always-on MCP over private VPN: findings

**Device:** Xiaomi Mi A3, `laurel_sprout`, Android 11 / API 30, arm64-v8a
**Branch:** `experiment/mi-a3-edge-service` (base `cca7716`)
**Transport:** wireless ADB, USB absent
**Issue:** methakon/homeguardian-ai#15 · dependency: #14 (Oracle WireGuard hub)

## 1. Headline

Present-day service operation and the MCP authentication surface are **verified**. The
VPN-remote access path is **BLOCKED**, and the blocker is now **proven on hardware rather than
assumed** — which is the single most important result here, because it determines the architecture.

| milestone | status |
|---|---|
| 1. Oracle WireGuard service running | **SEPARATE WORKSTREAM** — Issue #14, still open |
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

## 5. Blocker: the remote peer needs a credential that must not be extracted

Milestone 3 requires an *independent VPN peer* to make authenticated MCP calls. That peer needs the
bearer token, and the token is:

- generated on-device with `SecureRandom` into `files/mcp_credentials.json`, mode 0600,
  **never displayed and never logged**, with only a SHA-256 **fingerprint** exposed to identify it;
- unreadable via `adb shell` in the production build — confirmed first-hand: `run-as` reports
  `package not debuggable`.

Three routes exist, and all three need an owner decision:

1. **The `HG_EDGE_DEBUGGABLE=1` test build.** Documented as the sanctioned path for full host-side
   authenticated testing, and it "must never be the deployed artifact". It does not exist yet — the
   build script has no such switch. Building *and installing* it means replacing the production
   artifact on the designated always-on host, which is a production change.
2. **The owner provisions the token to the peer** through whatever process they intend.
3. **The app gains an operator-visible provisioning step** — a deliberate design change to the
   security model, not a testing convenience.

Work stopped here rather than taking route 1 without approval. The brief permits stopping for "an unsafe
production change [or] unresolved secrets", and installing a debuggable variant over the production
build on the always-on host is both. **An owner decision is required.**

## 6. Screen-off / reboot / recovery — partial, and what is missing

- **Screen-off: previously PASSED** for service lifetime and MCP staying loopback-only
  (`docs/EDGE_WIRELESS_DEVELOPMENT_MI_A3.md` §9), and the 11h54m continuous uptime above
  corroborates it. A screen-off test was **not** re-run in this pass.
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
2. **Issue #14 lands the hub**. Until then there is no peer to dial.
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

---

# Part 2 — HG-NET-02B execution: Phase 1 preparation and Phase 2 bridge

## 10. Phase 1 — preparation

**Production application preserved.** The signed, non-debuggable production APK was copied out of
the build tree before any rebuild, because the build script `rm -rf`s that directory:

    ~/.homeguardian-artifacts/edgeservice-1.0.0-production.apk
    sha256 2d6018e0afa4c7250b621efcb4194dc3d964c4895e18cc043738644d03a47246

That hash matches the value recorded in `EDGE_WIRELESS_DEVELOPMENT_MI_A3.md` §9 as the SHA installed
on the device, so a restore is byte-identical. The APK on the always-on host was **not replaced**.

**Unique WireGuard device key generated** (private key 0600 under `~/.homeguardian-vpn/`, never
printed):

    public key    IGk9g7lwAk2pWn8DwJpQuh+Mzgq3yUEqBW/I32cvIFY=
    fingerprint   04fdac4b4c0c6614

The public key is the device's overlay identity. A LAN address is never used as identity.

**Android's supported VPN mechanism is the only path**: the official WireGuard client via
`VpnService`, which prompts on-device and cannot be satisfied from adb. `com.android.vpndialogs` is
present on the device, so that consent flow exists. No root, no kernel module, no bypass.

## 11. Phase 2 — the outbound authenticated control bridge (implemented)

`apk_service/src/org/homeguardian/edgeservice/ControlBridge.java`, wired into `EdgeService`.

**Outbound only.** The bridge contains **zero** `ServerSocket`/`bind()` calls — verified by scanning
the packaged artifact. It dials toward the authorized coordinator and never listens on any
interface, so there is no public endpoint and nothing Wi-Fi-wide to scan.

**Default OFF.** `ACTION_START` does not start it. It requires both an explicit
`BRIDGE_START` action and a coordinator endpoint supplied by the operator. An unconfigured
production install dials nowhere. The endpoint is passed in, never compiled in, so no development
LAN address can become a fixed destination.

**The production MCP bearer token is never exported.** The bridge reads it in-process through
`Provisioner.readToken()` — the same legitimate same-uid access the app's own probe uses — and uses
it only on the loopback hop. It is never logged and never sent to the coordinator. Authorized
remote clients authenticate with a **separate** enrolment secret, so the issue's requirement not to
extract or expose the production token is satisfied.

**Supported provisioning mechanism for authorized remote clients.** A 32-byte enrolment secret is
generated on-device with `SecureRandom` into a 0600 file, identified outward only by its SHA-256
fingerprint. The operator reads it once through an explicit on-device action and configures the
coordinator with it. It can be revoked (`revokeEnrolment`). This is a deliberate second credential
with its own lifecycle, not a copy of the MCP token.

**Peer identity and authorization, both directions.** The device presents
`hg-android-<16 hex of SHA-256(device key)>`, derived from a locally generated key — durable, not an
address. The coordinator must present the enrolment secret, compared in **constant time**.

**Bounded relay.** Frame is `[4B big-endian length][payload]`; the length is **validated before any
allocation** and refused above 64 KiB in both directions. One request in flight at a time, which
gives natural backpressure and bounded memory with no queue to overflow. Handshake lines are capped
at 256 bytes.

**Method allowlist, not a blocklist.** Only `initialize`, `notifications/initialized`, `tools/list`,
`tools/call` and `ping` are relayed. Anything else is answered with a JSON-RPC error and logged.

**Cannot reach anything else.** Every relayed request is POSTed to one **fixed constant** loopback
URL. The destination is never derived from the request, so the bridge cannot be turned into a
generic proxy to other local services. Camera video and microphone audio do not traverse it at all.

**Reconnect without duplicates.** A single session loop closes the previous socket before dialing
again, with exponential backoff from 1 s to 60 s, reset on a clean session. A second `start()` while
running is refused rather than opening a second session.

**Build evidence**

    APK      hg_service_apk_build/homeguardian-edge-service.apk
    size     1,639,032 bytes
    sha256   39ece8d41ffda9ba131b942f9e29657b2f8ccc239d1037ac5a09ee7e5b0503bf
    ABI      arm64-v8a   minSdk 30   targetSdk 30
    native   lib/arm64-v8a/libhg_edge_service.so 2,794,800 B (+ libc++_shared.so)
    dex      ControlBridge / bridge_identity / bridge_enrol / BRIDGE_START / BRIDGE_STOP / HGB1
    manifest debuggable attribute ABSENT (production posture kept)
    perms    unchanged - INTERNET, FOREGROUND_SERVICE, RECEIVE_BOOT_COMPLETED only

The bridge added **no new permission** and no new listener.

## 12. Test harness for Phase 3 (ready, not yet run)

`scripts/bridge_test_coordinator.py` stands in for the authorized coordinator so the bridge can be
exercised without the Oracle hub. It is deliberately adversarial — beyond `initialize`,
`tools/list` and `tools/call` it asserts that a **disallowed method is refused**, that **non-JSON-RPC
input is refused**, and that an **oversized frame closes the connection**, because a bridge that
relays whatever it is handed has not been tested. It needs only the bridge's enrolment secret; it
never receives the MCP token.

## 13. Blocked, and why

- **Device unreachable.** The Mi A3 dropped off the network mid-session: 100% packet loss to its
  LAN address, `No route to host` on all three previously documented adb ports, and no mDNS
  advertisement. Its wireless ADB port rotates on reconnect (documented 41639 → 40815), but this is
  not a port change — the host is not answering ARP. **No on-device test of the bridge could run.**
  The Panasonic returned on USB at the same time; it is a different device and was not substituted.
- **Oracle hub (#14) still open**, so there is no real coordinator and no VPN underlay.
- **Android VPN consent** needs an on-device prompt; it cannot be satisfied from adb and must be
  the owner's action.
- **Installation onto the always-on host** is deliberately not done. The bridge is built and
  packaged as an isolated artifact; replacing the production app needs the reviewed update and safe
  installation plan the brief requires, and the restore path is ready (§10).

## 14. What is verified vs not

**Verified:** the production APK is preserved with a matching hash; the bridge compiles, packages
and is present in the dex; it has no listening socket; no new permission; production
non-debuggable posture retained; the device keypair exists and the public key is recorded.

**Not verified (and not claimed):** the bridge has **never executed**. No handshake has occurred, no
MCP request has crossed it, and no reconnect has been observed. Everything in §11 is a property of
the code plus the packaged artifact — it is an implementation, not a tested bridge. 24/7 readiness
is not claimed and cannot be until an adequate continuous run exists.

**Next, in order:** Mi A3 back on the network → approve the installation path → device VPN consent
→ run `bridge_test_coordinator.py` against the installed bridge → handshake, `initialize`,
`tools/list`, `tools/call`, refusals → then the remote-peer run with the workstation disconnected.

