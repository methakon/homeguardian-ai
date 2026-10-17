# HG-CAM-09 — Wi-Fi ADB Development Access to the Panasonic ELUGA I7

Branch: `experiment/hg-cam-eluga-cctv`. Continued from `8f854a6cb2bfbbfb9351d973329dad91b478ca40`.
Device: Panasonic ELUGA I7 EE, USB serial `LRW86SC6RO8TZ99H`, Android 9 / API 28, arm64-v8a.

**Summary: Wi-Fi ADB is working and verified end-to-end over the Wi-Fi transport alone. The
router DHCP reservation is NOT in place because router admin credentials are not available.
USB was not physically unplugged — that one step needs the owner's hands.**

## 1. Device identity (verified)

    USB serial    LRW86SC6RO8TZ99H
    model         ELUGA_I7_EE        board k39tv1_64_a27
    abi           arm64-v8a          android 9 / API 28
    wlan0         192.168.1.16/24
    Wi-Fi MAC     8c:c5:e1:ee:18:f5
    SSID          Airtel_Dhar   BSSID 08:aa:89:a7:7a:e6  (2.4 GHz, ch 6, RSSI -49, 65 Mbps)
    gateway       192.168.1.1  (Airtel F670LV9.0 ONT)
    wifi_static_ip  null  -> the phone uses DHCP; no manual static config on Android

The Mi A3 (`192.168.1.14:40815`, `laurel_sprout`, API 30) was visible throughout and was
**never selected**: every command in this milestone carried an explicit `-s` selector, and the
identity of the Wi-Fi transport was verified as `ELUGA_I7_EE` / `k39tv1_64_a27` / `arm64-v8a`
before it was used for anything.

## 2. Stage 1 — DHCP reservation: NOT VERIFIED (blocked, not failed)

The router is reachable and is an **Airtel F670LV9.0** ONT at `192.168.1.1` (HTTP 200 on 80 and
443, login page with an `SID` cookie; ports 22/23/8080 closed). Direct admin paths
(`/portforward`, `/forward`, `/nat`, `/firewall`) return 404 without a session, i.e. the admin
surface is authenticated.

**No admin credentials are available**: `browser_vault` holds no saved logins, and nothing on
this host stores router credentials. Credentials will not be guessed. Therefore no reservation
could be created, and per the brief no Android-side static configuration was entered either —
`wifi_static_ip` remains `null`, exactly as found.

### The IP is nevertheless stable, measured twice

| event | result |
|---|---|
| Wi-Fi disabled then re-enabled | reassociated in ~10 s, **same IP 192.168.1.16** |
| full phone reboot | came back as **192.168.1.16** |

So the address has been stable across both a reassociation and a reboot without any reservation.
A reservation would still be the correct long-term control (it removes the dependency on the
router's lease behaviour), but it is not currently blocking development.

**Owner action to close this:** sign in to the F670LV9.0 and reserve `192.168.1.16` for MAC
`8c:c5:e1:ee:18:f5`. A reservation also survives a router reboot and a factory reset of the
router's lease table, which is the real reason to have one.

## 3. Stage 2 — isolation assessment

Android 9 has no paired Wireless Debugging, so this is traditional ADB-over-TCP. Before enabling
it, the following was checked (all read-only):

| check | result |
|---|---|
| router admin surface requires authentication | YES — direct admin paths 404 without a session |
| UPnP / IGD present (would allow automatic port forwarding) | **NO responder found** — no automatic NAT-traversal device on this LAN |
| port 5555 listening before we started | **0 listeners**, `service.adb.tcp.port` empty |
| ADB host-key authorization | enforced — an unauthorised host cannot obtain a shell |
| address scope | private RFC1918 (`192.168.1.0/24`) behind NAT |

**Residual risk, stated plainly:** I could **not** verify that no *manual* port-forwarding rule
for 5555 exists, because that requires the router login I do not have. Two things bound that
risk: no UPnP/IGD responder exists, so forwarding cannot be created silently; and 5555 had never
been served by this phone (`service.adb.tcp.port` was empty before today), so a pre-existing rule
would have been inert. **This is the one item the owner should confirm on the router.**

No Oracle VPS, Cloudflare, mobile data or public VPN was used to expose anything.

## 4. Stage 3 — activation method (as performed)

    adb -s LRW86SC6RO8TZ99H shell getprop ro.product.model      -> ELUGA_I7_EE
    adb -s LRW86SC6RO8TZ99H shell getprop ro.build.version.sdk  -> 28
    adb -s LRW86SC6RO8TZ99H shell ip -4 addr show wlan0         -> inet 192.168.1.16/24
    adb -s LRW86SC6RO8TZ99H tcpip 5555                          -> restarting in TCP mode port: 5555
    adb connect 192.168.1.16:5555                               -> connected

Identity re-verified over the Wi-Fi transport before use: `model=ELUGA_I7_EE`,
`board=k39tv1_64_a27`, `abi=arm64-v8a`, `sdk=28` — **VERIFIED, and not the Mi A3**.

## 5. Stage 4 — Wi-Fi transport verification (USB still attached)

Everything below was run with `-s 192.168.1.16:5555` explicitly, so it genuinely travelled over
Wi-Fi rather than falling back to USB:

| operation | result |
|---|---|
| `adb shell` | PASS — model, API level and wlan0 address all correct |
| `adb install -r` (package update) | **PASS** — `Success`, 2.9 s wall time |
| `adb logcat` | PASS — 1050 lines retrievable |
| `adb push` | PASS — 1,103,664 bytes to `/data/local/tmp` (app-private path) |
| sha256 device vs local | **IDENTICAL** (`a0265d9e…`) — transfer integrity verified |
| `adb pull` equivalent / on-device verify | PASS |
| service start via launcher over Wi-Fi | PASS — `isForeground=true` |
| service stop over Wi-Fi | PASS — service records 0 |

**Not done: the physical USB unplug.** Unplugging a cable is not something a remote process can
do. With USB still attached, the Wi-Fi transport is proven to work independently, but the
"USB removed" claim cannot be made until the owner unplugs it. Until then this is
**USB-present Wi-Fi ADB**, not USB-free.

## 6. Stage 5 — stability results

| test | result |
|---|---|
| ten `disconnect` / `connect` cycles | **10/10 reconnected with verified identity, 0 failures** |
| Wi-Fi off then on | reassociated in ~10 s, **same IP**, identity verified again |
| screen off | `mWakefulness=Asleep`, and **Wi-Fi ADB remained usable while asleep** |
| screen on | `mWakefulness=Awake`, connection intact |
| application restart (force-stop, relaunch) | PASS — relaunched over Wi-Fi, `pid` returned, `isForeground=true` |
| development computer reconnect | covered by the 10-cycle test |
| **phone reboot** | see below — **does not survive** |

### Reboot behaviour: verified, and it does NOT persist

    before : service.adb.tcp.port = 5555, boot_count 168, uptime 52087s
    after  : boot_count 169, uptime 61s  (it genuinely rebooted)
             service.adb.tcp.port = ''      <- back to USB mode
             listeners on 5555     = 0
             wlan0                 = 192.168.1.16   (IP unchanged)
             adb connect           -> Connection refused
    after re-running `adb tcpip 5555`      -> connected, identity ELUGA_I7_EE

**ADB-over-TCP does not survive a reboot and must be re-armed with `adb tcpip 5555` after every
restart.** This matches the brief's expectation and is recorded as a hard limitation. No attempt
was made to force boot-time TCP ADB through root, firmware or insecure persistent settings, and
none should be.

## 7. Stage 6 — security posture and shutdown procedure

Traditional ADB-over-TCP is privileged developer access with no encrypted pairing. It is kept to
this supervised development network and nothing else. The HomeGuardian foreground service does
**not** depend on ADB: it is started by an in-app operator action, is `START_STICKY`, and was
verified running with no ADB involvement in HG-CAM-07.

**To shut the wireless listener down properly** (running `adb disconnect` on the computer is NOT
sufficient — it only drops the client, leaving adbd listening):

    adb -s LRW86SC6RO8TZ99H usb            # returns adbd to USB mode
    adb -s LRW86SC6RO8TZ99H shell getprop service.adb.tcp.port    # must print nothing

Then confirm from the network side that nothing answers on 5555. A reboot also clears it, as
verified above.

## 8. Recovery

- **If Wi-Fi ADB stops responding:** reconnect USB, `adb -s LRW86SC6RO8TZ99H tcpip 5555`,
  `adb connect 192.168.1.16:5555`. Requires USB physical access.
- **After any reboot:** re-arm `tcpip 5555`; the Wi-Fi endpoint will refuse connections until then.
- **If the IP changes:** read it with `adb -s LRW86SC6RO8TZ99H shell ip -4 addr show wlan0`.
  Do not assume `192.168.1.16` — it has been stable but is not reserved.
- **Never fall back to another device**: always pass `-s` explicitly and verify `ELUGA_I7_EE`
  before acting. Another phone is routinely reachable on this LAN.

## 9. Capability classification

| capability | classification |
|---|---|
| Router DHCP reservation | **NOT VERIFIED** — no admin credentials; owner action required |
| IP stability (observed, twice) | **VERIFIED** across Wi-Fi cycle and reboot |
| Wi-Fi ADB connection + identity | **PASS** |
| APK install over Wi-Fi | **PASS** |
| logcat / shell / push-pull over Wi-Fi | **PASS** |
| Service start/stop over Wi-Fi | **PASS** |
| Ten reconnect cycles | **10/10 PASS** |
| Screen-off ADB usability | **PASS** |
| USB physically removed | **NOT DONE** — owner action required |
| Reboot persistence | **VERIFIED ABSENT** (by design of Android 9) |
| ADB access isolation | **PARTIAL** — no UPnP/IGD, authenticated router, host-key auth; no manual-forward confirmation possible without router login |
| Service independence from ADB | **PASS** |

No Mi A3, Oracle VPS, Cloudflare, mail infrastructure or security release branch was modified.

## 10. Final USB-free acceptance — ATTEMPTED, item 1 FAILS

The owner reported the USB cable was physically disconnected. **The device disagrees.** Verified
four independent ways, because this contradicts the report:

| check | result |
|---|---|
| `adb devices -l` | `LRW86SC6RO8TZ99H  device  usb:2-3` — **USB transport present** |
| `adb -s LRW86SC6RO8TZ99H get-state` | `device` — live, not a stale adb entry |
| real command over the USB serial | returned `USB_TRANSPORT_ALIVE` and `ELUGA_I7_EE` |
| `lsusb` | `Bus 002 Device 071: ID 0e8d:201c MediaTek Inc. ELUGA_I7_EE` |
| `/sys/bus/usb/devices/2-3/serial` | `LRW86SC6RO8TZ99H`, manufacturer `PANASONIC` |

So **"USB transport is absent" is FALSE on this run.** The USB device number moved from `067`
(HG-CAM-07) to `071`, so something re-enumerated — consistent with a plug event, so the cable may
have been unplugged and re-seated, or a different cable/end was pulled. USB-free acceptance is
therefore **NOT CLAIMED**.

What the run *did* establish, and it is still worth having:

| item | result |
|---|---|
| 1. USB transport absent | **FAIL** — cable is in (evidence above) |
| 2. Wi-Fi identity matches Panasonic | **PASS** — `ELUGA_I7_EE` / `k39tv1_64_a27` / `arm64-v8a` / API 28, MAC `8c:c5:e1:ee:18:f5` |
| 3. Shell / bounded logcat | **PASS** — 14,006 logcat lines retrievable over Wi-Fi |
| 3. APK deployment over Wi-Fi | **PASS** — `Success`, 4.1 s wall time |
| 3. App launch + restart over Wi-Fi | **PASS** — `pid=4669`, `isForeground=true`, stop -> 0 service records |
| 4. Mi A3 never selected | **PASS** — every command used `-s 192.168.1.16:5555`; Mi A3 read only to confirm it is a different device |
| 5. No persistent insecure ADB config | **PASS** — see below |

### Why the Wi-Fi results are still meaningful with USB attached

`-s 192.168.1.16:5555` can only match the TCP transport: a TCP device's adb serial *is* the
`host:port` string, while the USB device's serial is `LRW86SC6RO8TZ99H`. Those are different
serials, so **no Wi-Fi command could silently fall back to USB.** Independence was also shown in
the other direction: dropping the Wi-Fi endpoint left the USB transport working on its own.

### Item 5 — no persistent insecure configuration was introduced

    persist.adb.tcp.port   (would SURVIVE reboot)  -> ''      correct: not set
    service.adb.tcp.port   (runtime only)          -> '5555'  cleared by reboot, as measured
    adb_wifi_enabled       (Android 11+ setting)   -> null    not applicable on Android 9
    router configuration changed                   -> NONE    (no router login available)

So the TCP listener is a *transient runtime state*, not a persisted one, and it does not survive a
reboot — re-confirmed earlier by a real reboot (`boot_count` 168->169, `service.adb.tcp.port`
empty, 0 listeners on 5555, `Connection refused`). No root, firmware, or insecure persistent
setting was used to change that.

**To close item 1:** unplug the Panasonic's USB data cable, confirm `adb devices` no longer lists
`LRW86SC6RO8TZ99H`, and re-run the four checks above. Only then is USB-free operation claimable.
