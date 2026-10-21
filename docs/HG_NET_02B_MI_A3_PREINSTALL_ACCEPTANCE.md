# HG-NET-02B — Mi A3 pre-installation acceptance checklist

Purpose: the gate that must pass **before** the reviewed EdgeService update is installed on the
physical Xiaomi Mi A3. Every item is an observation recorded in advance, so that after the install
we can tell an update apart from a replacement, a data-preserving upgrade apart from a wipe, and a
rollback that works apart from one that silently does not.

Nothing here installs, uninstalls, resets authorization, enrolls a VPN peer, or touches Oracle.

Companion tooling: `scripts/adb_stability_preflight.sh` (USB/ADB stability verdict) and
`scripts/adb_stability_fixtures/run_fixture_tests.sh` (its off-hardware proof).

---

## 0. Entry condition — do not skip

The install gate opens only when **all** of the following hold. Item 1 is the one that has actually
been failing in practice; the others are cheap but each has a real failure mode behind it.

| # | Check | Command | Expected |
|---|---|---|---|
| 1 | USB/ADB stability | `bash scripts/adb_stability_preflight.sh` | **PASS** — zero churn for a full 300 s window |
| 2 | Correct handset, not the Panasonic | preflight identifies vendor `2717`, device `laurel_sprout` | Mi A3 confirmed, Panasonic ignored |
| 3 | Platform | `getprop ro.build.version.release` / `ro.build.version.sdk` | `11` / `30` |
| 4 | Transport state | `adb devices` | target in state `device` (not `unauthorized`, not `offline`) |
| 5 | Owner authorization | explicit, in writing, for this specific install | recorded |

If item 1 FAILs, stop. Installing onto a link that re-enumerates every few seconds is how a partial
write happens, and that is the single largest risk in this whole procedure.

---

## 1. Existing application identity and version

Record **before** any install. These are the "before" values that the post-install comparison is
made against.

```bash
adb -s "$TARGET" shell dumpsys package org.homeguardian.edgeservice \
  | grep -E 'versionName|versionCode|firstInstallTime|lastUpdateTime|codePath|flags'
```

| Field | Recorded value | Why it matters |
|---|---|---|
| `packageName` | `org.homeguardian.edgeservice` | must be unchanged by the update |
| `versionName` | expect `1.0.0` | the frozen baseline |
| `versionCode` | expect `1` | must **increase** to 4; a decrease is refused by Android |
| `firstInstallTime` | record verbatim | **must be identical after** — this is the primary proof the OS updated in place rather than replacing |
| `lastUpdateTime` | record verbatim | should change to the install time |
| `codePath` | record | confirms the APK location |

Also record the running service, so recovery after install can be judged:

```bash
adb -s "$TARGET" shell dumpsys activity services org.homeguardian.edgeservice | grep -E 'ServiceRecord|isForeground|startRequested'
adb -s "$TARGET" shell pidof org.homeguardian.edgeservice
```

And the MCP listener, which must remain loopback-only:

```bash
adb -s "$TARGET" shell 'cat /proc/net/tcp | grep -i 2256'   # 0x2256 = 8790
```
Expected: local address `0100007F:2256` (127.0.0.1) only — never `00000000:2256`.

---

## 2. Signing certificate compatibility

The candidate must be signed by the same key as the installed app, or Android refuses the in-place
update outright. Both the candidate and the installed app are checked, and the values compared.

```bash
# candidate (host side)
apksigner verify --print-certs hg_service_apk_build/homeguardian-edge-service.apk

# installed app (device side)
adb -s "$TARGET" shell pm dump org.homeguardian.edgeservice | grep -A2 'signatures'
```

Expected signer SHA-256 (both, identical):

```
869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
```

| Check | Expected |
|---|---|
| signer SHA-256 matches installed app | **required** — mismatch means the update is refused |
| signature scheme | v3 verified; v1/v2 absent (correct for minSdk 30) |
| `debuggable` attribute | **absent** — the app is non-debuggable |

**Two consequences worth stating plainly.**

* `run-as org.homeguardian.edgeservice` **will fail**, because the app is non-debuggable. Any step
  that plans to read app-private files that way is wrong and must be removed from the procedure.
  Data preservation is proven by `firstInstallTime` and by the coordinator still recognising the
  device identity — not by filesystem inspection.
* The signing key is an **inherited debug key**, not a release key. Continuity holds today, which is
  what makes an in-place update possible at all. Replacing that key later would break in-place
  updates for every installed copy, **including the rollback path**. A key change and a rollback plan
  must therefore be decided together.

---

## 3. Available storage and battery

```bash
adb -s "$TARGET" shell df -h /data
adb -s "$TARGET" shell dumpsys battery | grep -E 'level|status|powered|temperature'
```

| Check | Threshold | Rationale |
|---|---|---|
| free space on `/data` | ≥ 4× APK size (~6.6 MB) | install staging plus the pre-existing copy |
| battery level | ≥ 40% | an install interrupted by power loss is worse than a refused one |
| charging state | on charge, or `USB powered: true` | the USB link is already the fragile part |
| temperature | record it (a churning link can run warm) | baseline for comparison |

---

## 4. Frozen APK integrity

The artifact must be re-hashed **on the day**, not trusted from an earlier report.

```bash
sha256sum hg_service_apk_build/homeguardian-edge-service.apk
```

Expected — and this file must not be rebuilt or overwritten after approval:

```
e3116ac18d70f070c9c6c6748ac2ded4c24a72bbe3a31ca3c7d9ebad70196879
```

| Field | Value |
|---|---|
| size | 1,647,152 bytes |
| package | `org.homeguardian.edgeservice` |
| version | versionCode 4 / versionName 1.0.3 |
| permissions | 4 — INTERNET, FOREGROUND_SERVICE, RECEIVE_BOOT_COMPLETED, ACCESS_NETWORK_STATE |
| signer | `869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b` |
| content digest | `de0bdb22261dcdb49cb9c8609c2249eb26a9e6c45c978e12ff73f8416f1529aa` |

A rebuild of the same source should reproduce this hash byte-for-byte
(`scripts/normalize_apk_zip.py` runs after packaging, before zipalign/signing). If it does not,
stop and treat the artifact as unreproduced rather than installing it.

---

## 5. Data preservation and rollback limitations

Preserved by an in-place update (`adb install -r`): app-private files, the provisioned credential,
the device identity, and package settings.

Given up / not available:

| Limitation | Consequence |
|---|---|
| No device backup exists | if the data directory is lost, there is no restore path |
| `run-as` unavailable (non-debuggable) | preservation cannot be proven by reading app files |
| Rollback needs `-d` | Android refuses a versionCode decrease; `adb install -r -d` is required or the rollback silently fails |
| Rollback APK is a single local copy | no off-host escrow — losing this workstation loses the rollback path |
| Rollback restores code, not necessarily data | moot for 1.0.3 (it writes no new persistent state), not guaranteed for a future release |
| Inherited debug signing key | a later key change would break in-place updates *including* rollback |
| No ADB authorization reset (by policy) | if authorization breaks, it is fixed physically, not by clearing keys |

**Rollback artifact:** `~/.homeguardian-artifacts/edgeservice-1.0.0-production.apk`
sha256 `2d6018e0afa4c7250b621efcb4194dc3d964c4895e18cc043738644d03a47246`, versionCode 1 /
versionName 1.0.0, same signer as the candidate, non-debuggable — re-verify its hash on the day.

**Strongest single preservation signal:** the device identity at the coordinator is unchanged after
the update. A wiped data directory produces a **new** device identity and an authentication
rejection. That signal needs no filesystem access, but it only exists if the update and an MCP
connectivity check happen in the same session — so sequence them together.

---

## 6. Local ADB reverse MCP acceptance procedure

**State this up front: `adb reverse` bypasses the network stack.** It exercises the bridge, the
coordinator and the MCP path. It proves **nothing** about VPN confinement or route security, and it
must never be reported as such. Confinement requires a real tunnel.

Run **after** the install gate, and only for a candidate that has already passed items 0–5.

```bash
# 1. reverse-forward the coordinator ports to the device
adb -s "$TARGET" reverse tcp:18994 tcp:18994     # coordinator (bridge)
adb -s "$TARGET" reverse tcp:18995 tcp:18995     # coordinator HTTP
adb -s "$TARGET" reverse --list

# 2. point the bridge at the reversed coordinator via the in-app control
#    (endpoint is operator-supplied at runtime; it is never compiled in)

# 3. establish the session, then exercise the real path
```

| # | Test | Expected |
|---|---|---|
| R1 | bridge starts against the reversed endpoint | authenticated session established |
| R2 | `initialize` | `serverInfo` from the **native C++** engine (`homeguardian-mcp`) |
| R3 | `tools/list` | 2 tools: `get_homeguardian_status`, `get_alert_summary` |
| R4 | `tools/call get_homeguardian_status` | real engine state, `read_only: true` |
| R5 | `tools/call get_alert_summary` | real engine state, `read_only: true` |
| R6 | wrong device token | refused (401 / 4401), bridge does not authenticate |
| R7 | unauthenticated coordinator client | 401 |
| R8 | revoked device | close 4403, bridge halts, **no redial** |
| R9 | kill the reverse forward mid-session | session drops; bridge holds and does **not** fall back to another interface |
| R10 | oversize frame (> 64 KiB) | connection closed 4413 |

Client caveat worth carrying into the procedure: the engine signals some refusals by **status code
with an empty body**, and a denied tool returns **HTTP 200 with `isError: true`**. Rows R6–R8 must
therefore assert on **both** status and body, or they can pass while measuring nothing.

---

## 7. Post-install verification

```bash
adb -s "$TARGET" shell dumpsys package org.homeguardian.edgeservice | grep -E 'versionName|versionCode|firstInstallTime'
adb -s "$TARGET" shell dumpsys activity services org.homeguardian.edgeservice | grep -E 'isForeground'
adb -s "$TARGET" shell pidof org.homeguardian.edgeservice
```

| Check | Expected |
|---|---|
| `versionCode` | `4` (increased from 1) |
| `firstInstallTime` | **identical to the recorded "before" value** |
| service | running and foreground under a new pid |
| `ACCESS_NETWORK_STATE` | granted (normal permission, granted at install, no prompt) |
| MCP listener | still `0100007F:2256` — loopback only |
| device identity at coordinator | unchanged |
| USB stability | re-run the preflight; the install itself must not have worsened the link |

Anything other than an unchanged `firstInstallTime` means the install was **not** a clean in-place
update. Stop and investigate before proceeding to the reverse-forward acceptance run.
