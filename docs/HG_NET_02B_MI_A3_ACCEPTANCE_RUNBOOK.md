# Mi A3 physical acceptance — operator-authorised installation and MCP connectivity

Status: PREPARED, NOT EXECUTED. Nothing is installed. This is the runbook for when the owner
authorises installation.

Contains no endpoint address, peer key or overlay subnet; concrete deployment values are supplied at
run time.

---

## 0. Preconditions (checked before anything is installed)

| # | Precondition | How it is checked |
|---|---|---|
| P1 | Owner has authorised the install in writing | explicit instruction, not inferred |
| P2 | Candidate is signed with the same identity as the installed app | `apksigner verify --print-certs` on both; SHA-256 must match |
| P3 | Candidate `versionCode` is higher than the installed one | `aapt dump badging` on both |
| P4 | Production APK is preserved and hash-stable | local copy, sha256 compared to the recorded value |
| P5 | Current EdgeService is healthy and its pid is recorded | `pgrep -f org.homeguardian.edgeservice` |
| P6 | Device data that must survive is identified | app-private `files/` credential + identity files |
| P7 | A WireGuard tunnel exists and is up on the device | owner-visible in the WireGuard app |

Do not proceed past P1–P4 on any failure. Any failure here is reported, not worked around.

---

## 1. Installation and data preservation

```
# record the "before" state
adb shell dumpsys package org.homeguardian.edgeservice | grep -E 'versionName|versionCode|firstInstallTime'
adb shell pgrep -f org.homeguardian.edgeservice              # expect a pid, and note it
adb shell ls -l /data/data/org.homeguardian.edgeservice/files/   # via run-as if debuggable

# install in place (never a fresh install: -r preserves data)
adb install -r homeguardian-edge-service.apk

# verify AFTER
adb shell dumpsys package org.homeguardian.edgeservice | grep -E 'versionName|versionCode|firstInstallTime'
adb shell pgrep -f org.homeguardian.edgeservice              # new pid expected (restart, not data loss)
adb shell dumpsys activity services org.homeguardian.edgeservice | grep isForeground
adb shell dumpsys package org.homeguardian.edgeservice | grep -A3 'declared permissions' | grep ACCESS_NETWORK_STATE
```

Expected: `versionName=1.0.3`, `versionCode=4`, `firstInstallTime` **unchanged** (proves it was an
update, not a replacement), a new pid, service foreground, and `ACCESS_NETWORK_STATE` **granted**
(it is a normal permission, so it is granted at install with no prompt).

Data preservation is proven by `firstInstallTime` staying constant plus the credential file still
being present and the engine still authenticating — not by the install command returning success.

**Rollback** (only if the update misbehaves): `adb install -r -d` the preserved production APK. The
`-d` is required because Android refuses a versionCode decrease. Expected effect: back to 1.0.0 with
data intact.

---

## 2. Genuine MCP connectivity — the actual acceptance

Isolation first: **unplug USB** before the connectivity runs, so nothing is being proven over adb.

```
# on the coordinator side (Antigravity's isolated local coordinator, approved lab path)
# verify the bridge authenticates, then drive MCP through it
```

| # | Check | Expected |
|---|---|---|
| 1 | Bridge starts from the in-app control, endpoint configured | status shows dialling, then connected |
| 2 | Coordinator accepts `bridge_auth` | edge session established, no 4401 |
| 3 | `initialize` | `serverInfo` from the **native C++** engine |
| 4 | `tools/list` | 2 tools |
| 5 | `tools/call get_homeguardian_status` | `capture_enabled` / `state` values from the real engine |
| 6 | `tools/call get_alert_summary` | real aggregate |
| 7 | Unauthorised client | 401 |
| 8 | Revoked device | 4403, and the bridge halts (no redial) |
| 9 | Screen off, wait, call again | still serving |
| 10 | Wi-Fi reconnect | bridge re-dials and resumes |

Camera and microphone stay disabled throughout: this is control-only acceptance.

---

## 3. Route-confinement checks (the security acceptance)

| # | Scenario | Expected |
|---|---|---|
| R1 | Coordinator on the overlay, VPN up | bridge dials; socket pinned to the VPN network |
| R2 | **VPN switched off while the bridge is connected** | session tears down; bridge holds; **no traffic on Wi-Fi** — verify the coordinator sees no new request |
| R3 | **VPN off, then start the bridge** | refuses with a routing reason; nothing dialled |
| R4 | Point the bridge at a LAN address | refused: routed by a non-VPN network |
| R5 | Point the bridge at a public address | refused: no VPN route covers it |
| R6 | VPN restored | bridge resumes by itself |

R2 is the one that cannot be proven off-device and is the reason for the pinning: the check-then-
connect window only exists on real hardware under a real network change.

**How to observe R2 without packet capture:** on the coordinator, watch the request counter before
and after switching the VPN off. It must not increase. That is the evidence that nothing fell back.

---

## 4. Remaining hardware blockers

| # | Blocker | Owner / dependency |
|---|---|---|
| B1 | **No running coordinator listener** on the Oracle host, and no reconciled destination | Hermes 1 + Antigravity |
| B2 | Install authorisation (P1) | owner |
| B3 | WireGuard tunnel on the device: key import, consent, handshake | owner taps; values from Hermes 1 |
| B4 | Per-app VPN exclusion is not enumerable from the sandbox — if the VPN is configured to include selected apps only, our traffic bypasses it | owner must confirm the app is included |
| B5 | Mi A3 USB enumeration is intermittent (observed three times: device drops mid-session, needs a re-plug) | physical |

Until B1 and B3 are resolved there is no destination to dial and no tunnel to confine to, so nothing
in section 2 or 3 can execute. The code is ready; the environment is not.

---

## 5. What is NOT claimed

No install, no data migration exercised, no VPN activated, no MCP call over a real tunnel, no
physical-device PASS. Off-device evidence for the policy and the pinning behaviour is in
`docs/HG_NET_02B_ROUTING_AND_UPDATE_READINESS.md` and
`scripts/bridge_ws_compat/RouteSecurityPolicyTest.java`.
