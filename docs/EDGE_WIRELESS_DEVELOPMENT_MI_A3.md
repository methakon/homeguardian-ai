# Wireless development & debugging — Mi A3 edge node (HG-EDGE-04)

**Objective:** remove the permanently-attached USB cable. The manager disconnects USB
and HomeGuardian can still be deployed, debugged, restarted and tested from the Ubuntu
workstation over authorized Wi-Fi.

**Branch:** `experiment/mi-a3-edge-service` (isolated; the release/security worktrees
were not touched).

**Status at time of writing:**
* The toolkit is **written and proven over USB** (12/12 MCP checks, log collection,
  identity verification) — see §8.
* The wireless pairing itself **is not yet done**: Android 11 exposes no
  adb-reachable toggle, so it needs one action on the phone screen. See §5. Nothing in
  this document should be read as claiming wireless debugging has been demonstrated
  until §9 says so.

---

## 1. What this phase must not do

| Constraint | How it is honoured |
|---|---|
| No legacy `adb tcpip 5555` | Never used. Only Android 11's native TLS pairing (`adb pair`) + `adb connect`. |
| MCP stays loopback-only | `0100007F:2256` == 127.0.0.1:8790. The scripts assert the **absence** of an all-interfaces listener and fail if one appears. |
| No LAN/public exposure, no port forwarding, no tunnelling | `adb forward` binds the HOST's loopback only. No router changes, no VPS proxy. |
| No root / ROM / bootloader / factory reset | None attempted. |
| No sensitive permission without separate authorization | The app still requests only INTERNET, FOREGROUND_SERVICE, RECEIVE_BOOT_COMPLETED. |
| No unattended charging, no 48–72 h soak | Not started. |
| Pairing codes never logged or committed | The code is read from a 0600 file, never from argv; the file is shredded afterwards; all output passes a redaction filter. See §5. |
| Don't modify the release-security worktree | Only `hermes-mi-a3-edge-service` was edited; `antigravity-release-integration`, `hermes-sec-01`, `qa-sec-audit` and `hermes-mi-a3-edge-sec02` were untouched. |

---

## 2. Tooling verification (done)

```
adb 1.0.41 / Version 34.0.4-debian   at /usr/lib/android-sdk/platform-tools/adb
  pair HOST[:PORT] [PAIRING CODE]    present   (needs >= 30.0.0)
  connect / disconnect               present
  mdns check / mdns services         present, exit 0
```

Wireless Debugging is an Android 11 (API 30) feature and this device is API 30, so the
native TLS path is available — no legacy plaintext mode is needed.

---

## 3. Device identification — and why it must be multi-identifier

Verified identifiers for the genuine target:

| Identifier | Value |
|---|---|
| `ro.serialno` / `ro.boot.serialno` | `edd53dbd04de` |
| USB `iSerial` (kernel descriptor, independent of Android props) | `edd53dbd04de` |
| `ro.product.device` | `laurel_sprout` |
| `ro.board.platform` | `trinket` (Snapdragon 665 / SM6125) |
| `ro.product.manufacturer` | `Xiaomi` |
| `ro.product.cpu.abi` | `arm64-v8a` |
| `ro.build.version.sdk` / release | `30` / Android 11 |
| `MemTotal` | 5,738,336 kB (≈5.6 GB) |
| security patch | 2022-08-01 |

**The model string is not sufficient, and the USB descriptor actively lies.** This phone
enumerates as:

```
idVendor 0x2717 Xiaomi Inc.
idProduct 0xff08 Redmi Note 3 (ADB Interface)      <-- NOT this device
```

Xiaomi ships a generic ADB descriptor across models, so anything keying on the USB
product string would identify a Mi A3 as a *Redmi Note 3*. Likewise a single property
can be spoofed — the other connected phone (an MT6580 clone) fakes
`persist.sys.fakeandroidversion=15.0`.

`_common.sh: hg_assert_mi_a3()` therefore requires **all** of
`device=laurel_sprout`, `platform=trinket`, `manufacturer=Xiaomi`, `abi=arm64-v8a` and
`MemTotal > 4 GB`, and explicitly recognises the clone's signature
(`platform=mt6580` / `abi=armeabi-v7a`) to report *which* wrong phone it found. There is
**no "use the only attached device" fallback anywhere** — a bogus serial exits 3
(verified). This is what stops a script from silently deploying onto the wrong hardware.

---

## 4. Network prerequisites

| Item | Value |
|---|---|
| Phone Wi-Fi | `Airtel_Dhar`, 2.4 GHz channel 6 (2437 MHz), RSSI −53…−54 dBm, link 65 Mbps |
| Phone LAN address | `192.168.1.14/24` |
| Host address | `192.168.1.6/24` on `wlp6s0` — **same /24**, so a direct connect is possible |
| Host mDNS | `avahi-daemon` active, 5 sockets on :5353 |
| Host → phone reachability | reachable, **but RTT 249–1289 ms with ~520 ms jitter** (measured) |

That latency is worth flagging now: it is unusually poor for a LAN and will make the
wireless shell feel sluggish. It is a property of this AP/2.4 GHz link and the phone's
Wi-Fi power management, not of adb. Values are re-measured as part of the demonstration.

---

## 5. Pairing procedure (the one step needing the phone screen)

**Why it needs a human.** `settings put global adb_wifi_enabled 1` was tried over USB
and the framework reset it to `0` immediately — Android 11 only enables Wireless
Debugging through its own UI. This is a genuine platform limitation, not a missing trick.

**On the phone (manager):**
1. `Settings → System → Developer options → Wireless debugging` → **toggle ON**.
2. Stay on that screen. Note the **IP address & Port** shown under *Wireless debugging*
   (this is the *connect* port).

**Then either** consult §5a (pairing) **or** §5b (already-trusted key).

### 5a. If pairing is required

3. Tap **"Pair device with pairing code"** — a dialog appears with an IP:PORT and a
   6-digit code. *Leave it open; it expires.*
4. Write the dialog's values to a private file **on the workstation** (not to chat):

```bash
umask 077
printf '%s %s\n' '<IP:PORT from the PAIRING dialog>' '<6-digit code>' > /run/user/$(id -u)/hg_pair
chmod 600 /run/user/$(id -u)/hg_pair
```

5. Run `scripts/edge_dev/edge_dev_pair.sh`. It refuses a code file that is not
   mode 600, never takes the code on the command line, redacts all output, and shreds
   the file whether or not pairing succeeds.
6. Then connect: `scripts/edge_dev/edge_dev_connect.sh`

### 5b. The likely shortcut — the already-trusted key

The device already trusts this workstation's adb key, because USB debugging is
authorized (`~/.android/adbkey` ↔ device `adb_keys`). Android 11's wireless debugging
uses **the same key store**, so `adb connect` to the wireless port may succeed with **no
pairing code at all**. `edge_dev_connect.sh` is written to try exactly that first: if it
prints a verified identity it has connected without a code, and §5a can be skipped
entirely. If it reports `not paired`, fall back to §5a.

This is worth attempting first precisely because a pairing code is the only secret this
phase would otherwise have to handle.

---

## 6. Toolkit

All scripts live in `scripts/edge_dev/`, source a shared `_common.sh`, take timeouts on
every adb call, target one explicitly verified device, and exit non-zero on failure.

| Script | Purpose | Exit codes |
|---|---|---|
| `edge_dev_connect.sh` | discover via mDNS (or `--port N`), connect, verify identity, report state (`--status`) | 3 not found, 4 not paired, 8 no network |
| `edge_dev_pair.sh` | one-time pairing with the code kept private (see §5a) | 3 no endpoint, 4 refused |
| `edge_dev_deploy.sh` | install/update the APK over Wi-Fi, verify the installed SHA-256 against the built artifact, confirm the service came back | 5 deploy failed |
| `edge_dev_status.sh` | `status` / `start` / `stop` / `restart` / `probe` | 6 action failed |
| `edge_dev_test.sh` | MCP functional tests over `adb forward` (§8) | 7 any check failed |
| `edge_dev_logs.sh` | sanitized evidence collection (redaction is mandatory) | 3 device gone |
| `edge_dev_disconnect.sh` | drop the wireless transport; `--off` also tries to disable Wireless Debugging | 3 nothing to do |
| `_common.sh` | identity verification + timeouts + redaction | 2 identity mismatch, 3 not found |

**Service control note.** `EdgeService` is `android:exported="false"` — deliberately, so
no other app can start this server — which means `am start-service` is correctly refused
and there is no control broadcast. Two supported paths exist:

* **restart via redeploy** — the `BootReceiver` also handles `MY_PACKAGE_REPLACED`, so
  installing an update starts the service. **Deployment is the restart path**, and it
  needs no screen interaction. Preferred.
* **restart via the UI** — wake the screen (`input keyevent KEYCODE_WAKEUP`), dismiss
  the keyguard, launch `MainActivity`, locate the button by label and tap it. This
  device has no secure lock (`locksettings get-disabled: true`, `deviceLocked=0`), so
  this works; it is the only way to reach *start* or *probe now* after a plain stop.

---

## 7. Daily workflow

```bash
cd scripts/edge_dev

./edge_dev_connect.sh                 # 1. connect + prove identity (or --port N)
./edge_dev_status.sh status           # 2. service state, loopback assertion, probes
./edge_dev_deploy.sh                  # 3. deploy; the install also restarts the service
./edge_dev_test.sh                    # 4. MCP functional tests over adb forward
./edge_dev_logs.sh                    # 5. sanitized logs + evidence
./edge_dev_disconnect.sh --off        # 6. wind down; best-effort disable Wireless Debugging
```

`HG_EDGE_HOST`, `HG_EDGE_SERIAL`, `HG_EDGE_TARGET`, `HG_EDGE_FWD_PORT` and
`HG_EDGE_DEBUGGABLE` override the defaults; the phone's DHCP address is the most likely
thing to change.

---

## 8. Evidence already obtained (over USB)

The toolkit was exercised end-to-end over USB before any wireless step, so that the
wireless change is isolated to the transport and nothing else:

```
== A. host-side surface (no token; production build safe) ==
  PASS  GET /mcp unauthenticated -> 401 (auth precedes method dispatch)
  PASS  POST with no Authorization -> 401
  PASS  401 advertises WWW-Authenticate: Bearer
  PASS  POST with a WRONG bearer -> 401
  PASS  DELETE unauthenticated -> 401 (session check is post-auth)
  PASS  oversized body (70 KiB) -> 413 without needing auth (size guard precedes it)
== loopback-only binding (on the device) ==
  PASS  MCP bound to 0100007F:2256 (127.0.0.1 only)
  PASS  no all-interfaces MCP listener (LAN exposure absent)
== B1. authenticated exchange, as performed by the service's own watchdog ==
  PASS  watchdog completed 306 authenticated MCP cycles, 0 failures
  PASS  negotiated MCP protocol version 2025-11-25
  PASS  tools/list returned both real tools
  PASS  a real tools/call result was returned (capture_enabled=false as configured)
== RESULT: 12 passed, 0 failed ==
```

Sanitized collection verified: 11 artifacts, **0** pairing-code shapes and **0** token
shapes survived redaction.

### 8.1 A correction this phase produced

The first run of `edge_dev_test.sh` reported 2 failures (expected `405` for GET, `400`
for an unauthenticated DELETE). Those expectations were **wrong, not the server**. The
handling order in `src/mcp/McpHttpServer.cpp` is:

```
413 size (l.431) -> 501 chunked (l.441) -> 400 malformed (l.448/453)
  -> 401 auth (l.480-504) -> 405 method (l.512) / 400 session (l.528)
```

so an *unauthenticated* request can only ever produce 413/501/400/401 — method and
session are validated **after** auth, which is the stronger property (an unauthenticated
caller learns nothing about the method surface). The script now asserts the real
behaviour, and the two post-auth codes are checked in the debuggable-build branch where
a token actually exists. Recorded because the earlier HG-EDGE-01/02 reports describe
`GET -> 405`, which was true of that SHA and is not true of this one.

---

## 9. Wireless demonstration — PENDING

Not yet demonstrated. To be filled in after the §5 action, and each item will be
reported as PASS/FAIL with the actual command output:

| # | Check | Status |
|---|---|---|
| 1 | `adb pair` / `adb connect` to the TLS endpoint | PENDING |
| 2 | `adb devices -l` over Wi-Fi shows the authorized Mi A3, identity-verified | PENDING |
| 3 | Shell + property inspection over Wi-Fi | PENDING |
| 4 | APK install/update over Wi-Fi with SHA-256 match | PENDING |
| 5 | Service startup/status/restart over Wi-Fi | PENDING |
| 6 | Logcat + native crash buffer collection over Wi-Fi | PENDING |
| 7 | File transfer in both directions | PENDING |
| 8 | MCP initialize/tools/auth over Wi-Fi (host-local adb forward) | PENDING |
| 9 | Reconnect after Wi-Fi drop, screen lock, Ubuntu restart, device reboot | PENDING |
| 10 | Screen-off behaviour | PENDING |
| 11 | Bluetooth assessment | **DONE — see §11** |

---

## 10. Security limitations (stated, not glossed)

* **A host-side *authenticated* MCP exchange is impossible against the production
  build without root.** The bearer token is in the app's private `filesDir`
  (`/data/data/org.homeguardian.edgeservice/files/mcp_credentials.json`, mode 0600) and
  `adb shell` gets `Permission denied`. This is the correct design and **this work does
  not weaken it** by copying a token to shared storage.
  Two honest consequences:
  * the host-side layer can therefore only test the *unauthenticated* surface
    (401/413/etc.), which it does thoroughly;
  * the *authenticated* protocol exchange is evidenced by the service's own watchdog,
    which performs initialize → tools/list → tools/call with the real token every 20 s
    and logs the result, and that log is pulled over Wi-Fi.
  * full host-side authenticated testing requires the **debuggable test build** the
    brief asks for, where `run-as` can read the token (`HG_EDGE_DEBUGGABLE=1`). That
    build must never be the deployed artifact. **Not yet built — see §9.**
* **Wireless Debugging cannot be turned on or off from adb.** Both directions need the
  phone screen. `edge_dev_disconnect.sh --off` attempts it and reports honestly when it
  cannot. The intended end state is *off when not developing*.
* **The TLS endpoint is reachable by anything on the trusted LAN.** It still requires
  this workstation's key, but on an untrusted network the correct move is to disable
  Wireless Debugging, not to rely on the key alone.
* **No mDNS verification yet.** Multicast is commonly filtered by consumer APs, so
  `_adb-tls-connect._tcp` may not be discoverable from the host. `--port N` exists for
  that case, and §9 item 9 will record which route actually worked.
* **Rotating ports.** The connect port is not fixed across toggles; scripts must
  re-discover it rather than cache it.
* **Poor link quality** (RTT up to 1289 ms) will slow large transfers; not a security
  issue, but it is a usability ceiling worth stating.

---

## 11. Bluetooth assessment (complete)

**Verdict: not a viable substitute for ADB over Wi-Fi. Recommend not pursuing it.**

| Question | Finding |
|---|---|
| Is there a Bluetooth transport for adbd? | **No.** AOSP adbd has usb, tcp and (emulator) vsock transports. `adb over Bluetooth` does not exist on any Android version. |
| Could Bluetooth still carry IP? | Only via PAN/tethering (bnep). Measured: no `bnep`/`pan0` interface present, and `dumpsys bluetooth_manager` shows `enabled: false`, `state: BLE_ON`, `Bonded devices:` empty. |
| Bandwidth | Bluetooth PAN is ~1–2 Mbps against the current Wi-Fi link's **65 Mbps** — roughly a 30–60× penalty on APK deployment. |
| Would it avoid the forbidden `adb tcpip`? | No. Even over PAN, adb would still need a TCP endpoint on the phone, which means `adb tcpip` — the plaintext mode this phase is required to avoid. |
| Reconnect/auth behaviour demonstrated? | Nothing to demonstrate: the transport does not exist. |

Bluetooth is therefore **not** substituted for working ADB, as the brief requires. Its
only conceivable role is an out-of-band *network* path of last resort, and even then it
would not give secure adb without the pairing/TLS machinery that Wi-Fi already provides
better.

## 12. Documentation impact

* This file is the wireless-development manual.
* `docs/EDGE_DEVICE_MI_A3.md` records the device's discovery, the managed service and
  reliability results; §8.1 here is a correction that applies to how its earlier MCP
  status-code expectations should be read.
* `docs/ROADMAP.md`'s HG-EDGE block is updated with an HG-EDGE-04 line. No numbered
  control-plane row is invented (AGENTS.md rule 9).
