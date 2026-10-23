# Wireless development & debugging — Mi A3 edge node (HG-EDGE-04)

**Objective:** remove the permanently-attached USB cable. The manager disconnects USB
and HomeGuardian can still be deployed, debugged, restarted and tested from the Ubuntu
workstation over authorized Wi-Fi.

**Branch:** `experiment/mi-a3-edge-service` (isolated; the release/security worktrees
were not touched).

**Status at time of writing:**
* **Cable-free development is demonstrated and working.** USB was physically removed
  (proven electrically: `lsusb` showed **0** phones and `/sys/bus/usb/devices/2-3` was
  gone), and over Wi-Fi alone: identity verification, APK deploy with SHA-256 match,
  service auto-restart, MCP functional tests and file transfer all passed. See §9.
* No pairing code was ever needed — Android 11 auto-trusts the USB-connected host's
  key. See §5b.
* Two findings materially constrain the workflow and are **not** worked around:
  the debugging **port rotates** on a Wi-Fi reconnect, and after a Wi-Fi outage the
  listener is absent for a window, so recovery needs patience and/or a phone action.
  See §13 and §14.
* mDNS works **intermittently**, not reliably, and `adb mdns services` never works
  here. See §15.

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
| `edge_dev_enable_wireless.sh` | **recovery**: drives the UI to re-enable Wireless Debugging (needs an existing adb transport); re-applies the operator's trust choice, never touches the checkbox | 1 could not drive UI, 3 no transport |
| `edge_dev_mdns_resolve.py` | real mDNS resolution (PTR → SRV) to `ip:port`; stdlib only, because `adb mdns services` finds nothing here | 0 resolved, 1 none |
| `edge_dev_discover_port.py` | **fallback**: bounded TCP probe of one host's ephemeral range | 0 found, 1 none |
| `edge_dev_mdns_probe.py` | raw multicast probe used to prove whether mDNS answers at all | 0 answered, 1 none |
| `_common.sh` | identity verification + timeouts + redaction + target resolution | 2 identity mismatch, 3 not found |

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

## 9. Wireless demonstration — RESULTS

All items below were executed over the **wireless transport only** (`adb -s
192.168.1.14:<port>`), with USB physically removed for the primary run at 10:55.

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | `adb connect` to the TLS endpoint | **PASS** | `connected to 192.168.1.14:41639`; no pairing step needed (§5b) |
| 2 | `adb devices -l` shows the authorized Mi A3, identity-verified | **PASS** | only `192.168.1.14:41639` present; reports `laurel_sprout`/`trinket`/`arm64-v8a` |
| 3 | Shell + property inspection over Wi-Fi | **PASS** | identity check, `getprop`, socket table all read remotely |
| 4 | APK install/update over Wi-Fi with SHA-256 match | **PASS** | 1,634,936 B in 4 s; device SHA-256 == local `2d6018e0…47246`; `1.0.0 (code 1, minSdk 30)`, `arm64-v8a` |
| 5 | Service startup/status/restart over Wi-Fi | **PASS** | service auto-restarted by `MY_PACKAGE_REPLACED` on install (pid 9266→24322→25036→26444) with no screen interaction |
| 6 | Logcat + native crash buffer over Wi-Fi | **PASS** | 11 sanitized artifacts; crash buffer empty; 0 redaction leaks |
| 7 | File transfer both directions | **PASS** | push 200 KB SHA-256 match; pull of the 57 KB probe log succeeded |
| 8 | MCP initialize/tools/auth over host-local adb forward | **PASS** | **12 passed, 0 failed**; 410 authenticated cycles by the watchdog, 0 failures |
| 9 | Reconnect after Wi-Fi drop / screen lock / adb restart / reboot | **PARTIAL** | screen lock + adb-stack restart PASS; Wi-Fi drop requires patience or a phone action (§13); reboot not tested |
| 10 | Screen-off behaviour | **PASS** | screen `Dozing`, `adb shell` still alive, service up, MCP still loopback-only |
| 11 | Bluetooth assessment | **DONE** | see §11 — rejected as a substitute |

### 9.1 The port rotates, and there is a dead window

Two distinct behaviours, both measured:

1. **The port changes on a Wi-Fi reconnect.** 41639 before the drop → 40815 after.
   "A fixed IP does not mean a fixed ADB port" is exactly right.
2. **Immediately after the reconnect nothing is listening at all.** For several
   minutes, every port on the phone returned an immediate `ConnectionRefusedError`
   (0.1 s, i.e. an RST from a live stack — *not* a timeout). A 2.5 s-timeout scan of the
   whole ephemeral range at that moment found nothing while a control scan found the
   router's `:80`. Later the listener reappeared on the new port with no user action.

So a single probe right after a reconnect is a **false negative**. Retries are
mandatory, and `edge_dev_connect.sh --wait N` exists for that.

### 9.2 A correction I had to make mid-investigation

I first reported that "wireless debugging STOPPED" after the Wi-Fi toggle, and that
mDNS was "definitively dead". **Both were wrong**, and for the same reason: the tests
ran during the dead window and after the state had changed.

* `adb_wifi_enabled` still read **1**, and the listener did come back (new port). So the
  setting survived; only the listener took time to rebind. "Requires a manual
  re-enable" is therefore accurate only for a *sustained* outage — a transient drop can
  recover by itself. §13 states this precisely rather than overclaiming either way.
* A raw stdlib multicast query **did** get 8 records from the phone once wireless
  debugging was verified listening, which disproves "mDNS is dead". The actual behaviour
  is intermittent — see §15.

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

---

## 13. Recovery after a Wi-Fi interruption — stated accurately

**Do not claim unattended ADB reconnection where Android requires user interaction.**
Here is exactly what is and is not automatic:

| Situation | Behaviour | Automatic? |
|---|---|---|
| `adb kill-server` (Ubuntu/adb restart) | the wireless transport is **not** auto-restored; `edge_dev_connect.sh` re-establishes it from the remembered endpoint | **Yes**, by the helper |
| Screen off / locked | unaffected; shell and service keep working | **Yes** |
| Transient Wi-Fi blip | the listener returns on a **new port** after a window of minutes | **Yes**, but only if the caller retries (`--wait`) |
| Sustained Wi-Fi outage | `adb_wifi_enabled` survives, but the listener did **not** come back on its own within the observed window in the T4 run | **NO** — may need a phone action |
| Phone reboot | **not tested** — see below | **UNKNOWN** |

**The manual step, when needed** (Android 11 exposes no adb-callable toggle; `settings
put global adb_wifi_enabled 1` is reset by the framework — verified twice):

```
Settings > System > Developer options > Wireless debugging > toggle OFF, then ON
```

`scripts/edge_dev/edge_dev_enable_wireless.sh` automates exactly that navigation and
re-applies the operator's standing network-trust choice (ALLOW, with *Always allow on
this network* deliberately **unticked**). It needs an existing adb transport, so it is
the "reconnect the USB cable briefly" recovery path — which is precisely why the phone
must remain independently useful without ADB (§16).

**Phone reboot is UNVERIFIED and deliberately not tested here**, because the operator's
standing instruction for this phase was no further interruption tests. It should not be
assumed either way: Android's wireless-debugging toggle is not guaranteed to survive a
reboot, and the honest expectation is that it will need re-enabling.

---

## 14. Layered discovery — resilient to both an IP change and a port change

A fixed address alone was never going to be enough, and it is not implemented here. The
discovery is layered, and each candidate is **identity-verified before acceptance**:

| # | Source | Covers |
|---|---|---|
| 1 | `--port N` | the operator reads the port off the phone |
| 2 | remembered endpoint (`~/.hermes/cache/scratch/hg-edge/target`, mode 600) | the common case where nothing changed |
| 3 | `edge_dev_mdns_resolve.py` | IP **and** port rotation, when mDNS answers |
| 4 | `edge_dev_discover_port.py` | port rotation when mDNS is silent — the fallback |
| — | `--wait N` retries | the post-reconnect dead window (§9.1) |

**Verified against the actual failure mode.** With the wireless transport dropped and a
deliberately **stale** port (41639) planted in the remembered endpoint, the helper:

```
trying 192.168.1.14:41639  -> failed to connect ... Connection refused
mDNS/remembered candidates did not verify - probing the phone's ports (fallback 4)
trying 192.168.1.14:40815  -> identity OK: device=laurel_sprout platform=trinket abi=arm64-v8a
wireless ADB established      remembered : 192.168.1.14:40815
```

So a rotated port is recovered without operator input and without a fixed IP, and the
corrected endpoint is remembered for next time. The probe is scoped to **one host** and
its ephemeral range — never a `/24` sweep — and is validated with a control (it finds
`192.168.1.1:80`), because "no port found" is otherwise indistinguishable from a broken
scanner.

---

## 15. mDNS: what actually happens here

Re-tested with wireless debugging **verified listening** (the operator was right to
demand this — the first pass was confounded):

| Method | Result |
|---|---|
| `adb mdns services` | **never returns anything**, even while the phone answers raw queries |
| `resolvectl service _adb-tls-connect._tcp` | "Name not found" |
| raw stdlib multicast query to `224.0.0.251:5353` | **answered on one attempt — 8 records** from 192.168.1.14: `_adb-tls-connect._tcp.local` PTR → `adb-edd53dbd04de-hLZcH2._adb-tls-connect._tcp.local`; nothing on the next four attempts |
| PTR browse for the service type (QU bit set and clear, 12 s window) | **no answer** |
| direct SRV for the known instance name | **answered once**, then stopped |

Conclusions, stated at the strength the evidence supports:

1. **Multicast mDNS does traverse this LAN** — the phone answered a real query. It is not
   filtered. My earlier "mDNS is dead" verdict was wrong.
2. **It is not reliable enough to be the only mechanism.** It answers intermittently,
   and it did not answer PTR browse queries in the later runs. `init.svc.mdnsd` is
   `running` on the device, so this is responder behaviour, not a daemon failure.
3. **`adb mdns services` is the wrong tool here**, regardless of the network: it returns
   nothing even when the phone demonstrably answers. That is why the resolver is written
   in Python against the real protocol rather than shelling out to adb.
4. The instance name **embeds the serial** (`adb-edd53dbd04de-…`), so mDNS results can be
   filtered to the intended handset — useful on a LAN with more than one phone.

Therefore mDNS is candidate source 3 of 4, and the port probe is the fallback that
actually carried the recovery in testing.

---

## 16. Address stability, router DHCP, and why no static IP was applied

**LAN facts (all verified, none guessed):**

| Item | Value |
|---|---|
| Subnet | `192.168.1.0/24`, mask `255.255.255.0` |
| Gateway / DHCP server / DNS | `192.168.1.1` (MAC `08:aa:89:a7:7a:e6`) |
| DHCP lease time | 86400 s (24 h) |
| Router | **ZTE F670LV9.0** Airtel Xstream ONT — `©2008-2025 ZTE Corporation`, `<span id="pdtVer">F670LV9.0</span>` |
| Router firmware build | **not exposed** pre-auth (`/cgi-bin/luci`, `/getpage.gch`, `/version`, `/sysinfo` all 404) |
| Host | `192.168.1.6` (DHCP) |
| Mi A3 | `192.168.1.14`, **randomized** per-network MAC `ba:ba:6b:94:0c:03` (locally-administered bit set) |

**Address stability observed: the phone kept `192.168.1.14` and the same randomized MAC
straight through a Wi-Fi disconnect/reconnect.** With a 24 h lease and the same MAC, the
address is stable in practice without any reservation.

**Why a DHCP reservation is not in place.** It requires router admin access, and the
operator has no credentials. The documented factory defaults (modem.tools: `admin`/`admin`
for F670LV9.0; Airtel's support site: `admin`/`password`) may be tried **once**, and the
operator is checking the sticker on the router, which Airtel documents as carrying the
credentials. Separately, I will not type a password into a login form — that is a hard
constraint regardless of authorization — so the login is entered by the operator (masked
prompt) or performed by them directly.

**Why no Android static IP was applied.** Per the standing instruction, a static address
must not be assigned until the DHCP range and collision risk are verified and approved.
The host cannot enumerate the LAN reliably (its ARP table held only 2 hosts before an
active probe created misleading entries), so the DHCP pool must come from the router's
own lease list — which needs the login. **Nothing has been changed on the phone or the
router.** The reservation also interacts with the MAC below.

**MAC policy.** The phone uses a *randomized per-network* MAC, which is Android's privacy
feature and must not be disabled globally. Two legitimate options, neither applied:
* set this one network to use the device MAC (a **per-network** setting, leaving
  randomization intact everywhere else), then reserve against that stable identity; or
* reserve against the current randomized MAC, accepting that Android may rotate it
  (which would silently break the reservation).

The first is the correct approach if a reservation is wanted; the second is the trap.
