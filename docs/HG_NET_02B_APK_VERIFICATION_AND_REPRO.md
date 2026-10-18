# APK verification and the reproducibility finding

Contains no endpoint address, peer key or overlay subnet.

---

## 1. The discrepancy, and its cause

Antigravity reported v1.0.3 / versionCode 4 with SHA-256
`fb5ffd71ededde2fd6fcc61d90bb89b325a037570be3ee01db61c3768d15ecdc`.
My recorded v1.0.3 build was `87fbacb56f1a30c7ae5f1999376951c35b6c2250ddafababa5814868716549d4`.
**They differ, and neither is wrong.**

Two clean builds from the identical tree (HEAD `2221783`, working tree clean) gave two more different
hashes. Five hashes now exist for one source:

```
87fbacb56f1a30c7ae5f1999376951c35b6c2250ddafababa5814868716549d4   my first reported build
8573a434fc5bdf2cb3b8395c48ebe9e75f464e8bc0d9a48cd838006d581f6c4c   my rebuild, same source
6b9beff205c913d63bac9d450ba48edf346beceee2b1deafa1d8e1310ab9f5c6   reproducibility test, build A
8c883c8c6ff27f2c21b3ddd568be756ee1fbec07b70d9b69b05da46209600016   reproducibility test, build B
fb5ffd71ededde2fd6fcc61d90bb89b325a037570be3ee01db61c3768d15ecdc   Antigravity's build
```

**The APK build is not byte-reproducible.** The zip entries `classes.dex` and the two `.so` files
carry build timestamps, so the container differs on every run.

### What IS stable

All eight zip entries are **content-identical** across builds; only the container metadata varies.
A content-only digest (SHA-256 over the sorted entry names and their bytes) is stable:

```
content digest ................ 624fad10d8657bd505999321b3581b4896f799bee72a78684a060a0cfeed04ff
```

So the artifact has a stable identity, just not the one being quoted.

### Consequence — read this before gating anything on a hash

* **The APK SHA-256 must not be used as the artifact identity.** Comparing it across agents or builds
  tests zip metadata, not code. Two correct builds of identical source will "fail" a hash match.
* Antigravity's hash is **consistent with the same source** and cannot be confirmed or refuted by
  hash comparison alone. Confirming it needs the content digest, or a rebuild to the same content
  digest, from a named commit.
* What genuinely identifies a candidate: **package name, versionCode, signer certificate, and the
  content digest** — in that order. All four are stable here.

---

## 2. Exact branch HEAD and clean-tree proof

```
branch ........ experiment/mi-a3-edge-service
HEAD .......... 22217836f2561a8886d8f5a060b38960ba8c49c0
origin/... .... 22217836f2561a8886d8f5a060b38960ba8c49c0   (in sync)
working tree .. 0 modified, 0 untracked  -> clean
```

Both reproducibility builds were made from this commit with the tree clean, which is what makes the
differing hashes attributable to the build rather than to the source.

---

## 3. Manifest verification (packaged artifact)

```
package ............ org.homeguardian.edgeservice
versionCode ........ 4
versionName ........ 1.0.3
sdkVersion ......... 30      targetSdkVersion ... 30
native-code ........ arm64-v8a
application-label .. HomeGuardian Edge
launcher ........... org.homeguardian.edgeservice.MainActivity

uses-permission .... android.permission.INTERNET
                     android.permission.FOREGROUND_SERVICE
                     android.permission.RECEIVE_BOOT_COMPLETED
                     android.permission.ACCESS_NETWORK_STATE        <- the approved addition
                     (exactly four; unchanged from three plus one)

activity  .MainActivity   exported = TRUE   (launcher: required)
service   .EdgeService    exported = FALSE  (correct; only this app may start/stop the server)
receiver  .BootReceiver   exported = TRUE   (required to receive BOOT_COMPLETED and
                                             MY_PACKAGE_REPLACED; both are protected system
                                             broadcasts, so another app cannot forge them)
debuggable ................ absent
```

No component gained export permission; the service remains non-exported.

---

## 4. Signer verification

```
Signer #1 DN ....... CN=HomeGuardian Harness Debug, O=HomeGuardian, C=US
Signer #1 SHA-256 .. 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
installed app ...... 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
                     -> IDENTICAL, so an in-place update is signature-compatible
```

The signing key is named "...Debug" because the currently installed production app was itself signed
with it. Continuity holds; it is still not a genuine release key, and replacing the key later would
break in-place updates for every installed copy. That remains an owner decision, not a defect.

---

## 5. Data-preserving in-place update plan

**Correction to an earlier assumption of mine:** `run-as` does **not** work on this app. It is
non-debuggable, so `run-as org.homeguardian.edgeservice` fails regardless of build type. Any plan
that inspects `/data/data/.../files` that way is wrong. Data preservation must be shown another way.

```
# BEFORE — record the invariants
adb shell dumpsys package org.homeguardian.edgeservice | grep -E 'versionName|versionCode|firstInstallTime|lastUpdateTime'
adb shell pgrep -f org.homeguardian.edgeservice          # note the pid

# UPDATE — in place, never a fresh install
adb install -r homeguardian-edge-service.apk

# AFTER — verify
adb shell dumpsys package org.homeguardian.edgeservice | grep -E 'versionName|versionCode|firstInstallTime'
adb shell pgrep -f org.homeguardian.edgeservice          # new pid expected
adb shell dumpsys activity services org.homeguardian.edgeservice | grep isForeground
adb shell dumpsys package org.homeguardian.edgeservice | grep -A2 ACCESS_NETWORK_STATE
```

**How data preservation is actually demonstrated, without run-as:**

1. `firstInstallTime` is **unchanged** while `versionCode` increased — this is the OS saying it
   performed an update, not a replacement, and is the primary evidence.
2. The app's own UI reports the credential file's mode and the engine's state. If the stored
   credential had been lost, the engine would report a provisioning failure rather than a healthy
   running state — an externally visible signal that does not require filesystem access.
3. The bridge still authenticates with the device identity it had before, proving the identity file
   survived. A wiped data directory would produce a *new* device id and a coordinator-side auth
   rejection.

Point 3 is the strongest and is observable from the coordinator, which is why the update check
belongs in the same session as the MCP connectivity test.

**Rollback:** reinstall the preserved production APK with `adb install -r -d` (`-d` is required
because Android refuses a versionCode decrease). Rollback restores behaviour, not necessarily data
written by the newer version — this update writes no new state, so that caveat is currently moot.

**Gate before any install:** the candidate must be identified by content digest, not APK SHA-256
(section 1); signer must match (section 4); `firstInstallTime` recorded (section 5). Anything else
is a guess.

---

## 6. Local ADB reverse test — sequenced after the physical gate

Antigravity's local coordinator can be reached from the device over `adb reverse` without any VPN,
which makes it a useful *bridge* test but **not** a routing test: `adb reverse` bypasses the network
stack entirely, so it exercises nothing about VPN confinement. It must therefore run **after** the
validated physical-install gate and must not be reported as evidence for route security.

Sequence: physical install gate passes -> start the bridge against the reverse-forwarded coordinator
-> verify `initialize` / `tools/list` / `tools/call` against the real engine -> then, separately and
only if a real tunnel exists, run the confinement checks from
`docs/HG_NET_02B_MI_A3_ACCEPTANCE_RUNBOOK.md`.

---

## 7. Not claimed

No installation, no VPN activation, no Oracle change, no hardware PASS. Antigravity's APK was not
and cannot be confirmed or refuted by hash; the method for confirming it is given in section 1.
