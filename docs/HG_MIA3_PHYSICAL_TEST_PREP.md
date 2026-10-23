# HG-MI-A3-PHYSICAL-TEST — preparation and preconditions

Status: **PREPARED, NOT EXECUTED.** Nothing has been installed and no sensor has been activated.
This document exists so the mutating step, when authorised, is a single mechanical action against a
set of preconditions that have already been verified.

## 1. The signing conflict does not exist

A carrier of this lane's earlier notes claimed:

> Mi A3 EdgeService APK update — signature mismatch (`869b0a27…` vs installed `adc75fa8` →
> `INSTALL_FAILED_UPDATE_INCOMPATIBLE`; uninstall would delete app data, prohibited).

**That was wrong, in two independent ways, and both are corrected here.**

**(a) `adc75fa8` is not a different key — it is `dumpsys`'s abbreviated rendering.** Pulling the
installed APK off the device and reading its certificate directly:

```
$ adb -s edd53dbd04de shell pm path org.homeguardian.edgeservice
/data/app/~~8jDNA8pL7Dz0pdR-jf_K_g==/org.homeguardian.edgeservice-.../base.apk
$ adb -s edd53dbd04de pull <that path> /tmp/installed_edgeservice.apk
$ apksigner verify --print-certs /tmp/installed_edgeservice.apk
  Signer #1 certificate DN: CN=HomeGuardian Harness Debug, O=HomeGuardian, C=US
  Signer #1 certificate SHA-256 digest: 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
  Signer #1 certificate SHA-1   digest: d5312397a76bac57560bc6e949f209743ad7879c
  Signer #1 certificate MD5     digest: 87bf3413059994a4086d29c50656ec07
```

The repo artifact reports the **same** DN, the **same** SHA-256, the **same** SHA-1 and the **same**
MD5. There is one key, not two. The real rule this establishes: **a short hex string out of `dumpsys`
is not a certificate digest and must never be compared as one** — pull the APK and read it.

**(b) The two packages are different applications, so no update conflict was ever possible.**

```
Mi A3 installed : org.homeguardian.edgeservice  v1.0.0  "HomeGuardian Edge"  minSdk 30  targetSdk 30
repo artifact   : org.homeguardian.harness      v0.2.0  "HomeGuardian Node"  minSdk 27  targetSdk 28
```

Comparing the signer of an installed package against the signer of a *different* package's artifact,
and calling the result an update conflict, was a category error. The harness is **not installed on the
Mi A3 at all**, so installing it there is a **fresh install** with no upgrade-signature check to fail.

**Conclusion: there is nothing to resolve. The blocker was a note, not a mechanism.**

## 2. What is actually true about the two designated phones

| | Panasonic ELUGA I7 | Xiaomi Mi A3 |
|---|---|---|
| serial | `LRW86SC6RO8TZ99H` | `edd53dbd04de` |
| Android | 9 / API 28 | 11 / API 30 |
| abi | arm64-v8a | arm64-v8a |
| harness installed | **yes**, byte-identical `8e293ed4…` | **no** |
| signer | `869b0a27…` | — |
| reachability | **not on the ADB transport** | device |

The Panasonic already runs the harness. The Mi A3 does not. **Any physical test must therefore state
which phone it means**, because the two are in different states.

## 3. Preconditions, all machine-checked

`scripts/prepare_physical_test.sh` verifies every precondition and prints a GO/NO-GO:

```
$ bash scripts/prepare_physical_test.sh --serial edd53dbd04de
  OK   self-check: no runnable install/uninstall/push/reboot command outside the printed block
  OK   toolchain present (adb, aapt)
  OK   artifact: homeguardian-harness.apk  1033643 bytes  sha256 47f98357…
  artifact package : org.homeguardian.harness   minSdk 27  targetSdk 28
  artifact signer  : 869b0a270fd855a2cd9eb66ce3f8043d0edff9f4bf2eb6552ac3f40447d0281b
  OK   device: Mi A3  API 30  arm64-v8a
  OK   device API 30 >= minSdk 27
  OK   abi supported by the bundled native library
  OK   'org.homeguardian.harness' is not installed on this device — fresh install, no upgrade-signature check
  GO — every precondition met. The remaining step needs owner authorization to run.
```

It checks: toolchain; the artifact exists and is not a truncated file; the artifact's package is the
one this test means; the device's API satisfies `minSdk`; the abi matches the native library; and —
if the package IS present — it **pulls the installed APK and compares full certificate digests**
rather than trusting a short string from `dumpsys`.

### Negative controls (each verified to fail as intended)

```
wrong package artifact   -> FAIL (package mismatch)                 rc 3
missing artifact         -> FAIL                                    rc 3
0-byte artifact          -> FAIL ("a hash of this is meaningless")  rc 3
unattached serial        -> FAIL (device not found)                 rc 3
transient disconnect     -> FAIL (observed for real, then cleared)  rc 3
```

## 4. The preflight cannot install, and that is enforced

The script prints the commands it does **not** run, and greps its own body to prove no mutating
command is present. Getting that guard right took three attempts and the failures are instructive:

1. matching the literal string `adb` **missed** `"$ADB" install …` — it reported "no runnable install"
   while one was present. *Match the subcommand, not the binary's spelling.*
2. scanning the whole file flagged the heredoc that merely **documents** the commands. *Strip heredoc
   bodies.*
3. matching the verb anywhere on a runnable line flagged the script's own **messages** ("the APK
   cannot install here"), so the script refused to run at all. *Require command position.*

All three are covered by controls: five injected executable forms (variable-spelled, plain, `pm
uninstall`, `push`, `reboot`) are each caught, a commented-out form is correctly ignored, and the real
script passes.

## 5. The authorised step, when it comes

```
# 1. install — PROHIBITED until the owner records authorisation
adb -s <serial> install -r hg_apk_build/homeguardian-harness.apk

# 2. grant runtime permissions AFTER the on-screen consent prompt
adb -s <serial> shell pm grant org.homeguardian.harness android.permission.CAMERA
adb -s <serial> shell pm grant org.homeguardian.harness android.permission.RECORD_AUDIO

# 3. start capture — PROHIBITED: activates the camera. Owner consent must exist FIRST.
adb -s <serial> shell am start -n org.homeguardian.harness/.MainActivity

# 4. evidence for the test record
adb -s <serial> shell dumpsys activity services org.homeguardian.harness   # isForeground, type
adb -s <serial> shell dumpsys media.camera                                 # who holds the camera
```

The harness declares `CAMERA`, `RECORD_AUDIO`, `FOREGROUND_SERVICE`, `INTERNET`,
`ACCESS_NETWORK_STATE`, and its service is `exported="false"`.

A physical test asserts the foreground service keeps producing with the screen off — which is the
whole point of moving transport ownership from the Activity to `CctvService`.

## 6. Still missing before a test can mean anything

- **Owner authorisation** for `apk_install` and `sensor_activation` on a named device.
- **The Panasonic on the ADB transport**, if it is the intended subject.
- **Physical consent** for a camera/microphone test on the device being tested.

Nothing here bypasses those. This document makes the mechanical part one command; the decision stays
with the owner.
