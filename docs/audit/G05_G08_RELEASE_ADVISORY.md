# G05 / G08 — Safe Android release advisory: installed build vs candidate

**Scope:** advisory only. **No install, replace, uninstall, reboot or downgrade was performed.**

---

## 1. The two builds, measured

| | installed on the Mi A3 | candidate |
|---|---|---|
| package | `org.homeguardian.edgeservice` | `org.homeguardian.edgeservice` (same) |
| versionName | **1.0.0** | **1.0.3** |
| versionCode | **1** | **4** |
| APK size | 1,634,936 B | 1,651,248 B |
| APK sha256 | `2d6018e0afa4c7250b621efcb4194dc3d964c4895e18cc043738644d03a47246` | `cee0024c97f064045ff2d7829cf8e3f0f98e5944fe0095757044b98a37b3f172` |
| installed / built | 2026-10-17 (last update 11:19) | built 2026-10-19 |
| targetSdk | 30 | 30 |

The installed APK hash was obtained by a **read-only** pull of the device's own `base.apk` purely to
compare signing identity. Video, contacts, accounts and app data were not read.

---

## 2. Signing continuity — **CONTINUOUS** (this is the decisive fact)

| | installed | candidate |
|---|---|---|
| subject | `HomeGuardian Harness Debug` | `HomeGuardian Harness Debug` |
| serial | `44E3EA415474EB06` | `44E3EA415474EB06` |
| SHA-256 fingerprint | `86:9B:0A:27:…:28:1B` | `86:9B:0A:27:…:28:1B` |
| SHA-1 fingerprint | `D5:31:23:97:…:87:9C` | identical |

**Byte-identical certificate.** Therefore:

- An **in-place update is permitted** by Android (same package, same signer, versionCode increases).
- **Application data is preserved**: Android keeps the app's data directory across an update of the
  same package signed by the same key. That includes the app-private files under `filesDir` — notably
  the device's CSPRNG auth token, which the coordinator's registry must already hold.

Do **not** trust a truncated hash from a platform dump for this comparison. `dumpsys package` reports
`signatures=[adc75fa8]`, which matches neither the installed nor the candidate certificate fingerprint;
it is derived differently. The equality above was established by extracting and fingerprinting the
certificate from both APKs.

**Caveat, stated plainly:** the signing key is a **debug** key (`CN=… Harness Debug`, self-signed). It
is acceptable for a controlled handset and a development candidate, but it is not a production key and
its use should not be presented as release signing.

---

## 3. What an update does and does not do

**Does:** replaces the APK, keeps user data, keeps the signing lineage, and the new build starts the
same foreground service and the same `BOOT_COMPLETED` receiver.

**Does not:** grant VPN consent, enrol a VPN peer, change network configuration, or alter the phone's
Android version. With no VPN consent and no enrolled peer, `RouteSecurityPolicy` continues to **refuse
every non-loopback destination** — so this update cannot by itself begin routing device traffic
anywhere. That is a property of the app's design, not an assumption.

---

## 4. Downgrade and rollback — the part that is easy to get wrong

- **Downgrade is blocked.** Android rejects an install whose `versionCode` is lower than the installed
  one (`INSTALL_FAILED_VERSION_DOWNGRADE`). Once 4 is installed, the 1.0.0 build **cannot simply be
  reinstalled over it**.
- **There is therefore no clean in-place rollback.** The available paths are:
  1. keep the update and fix forward, or
  2. uninstall and reinstall the older APK — which **destroys application data and the device auth
     token**, requiring re-provisioning (and a matching registry entry on the coordinator), or
  3. restore the stock Android 11 image — the full recovery path, which rewrites the system and user
     partitions.
- **Do not treat uninstall as a harmless rollback.** It is a destructive operation: it deletes the
  app's data directory, invalidates the provisioned token, and on this handset is the heaviest option
  short of a full flash.

---

## 5. Recovery assets (verified)

The full recovery path exists and is intact on the workstation:

```
file   laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
size   2,439,506,308 B          PASS (matches documented value)
md5    2292e2c0b1c0e9839d5b14427154b5b4   PASS (matches Xiaomi's published value)
gzip   structurally intact      PASS (gzip -t)
```

Package `V12.0.26.0.RFQMIXM`, device `laurel_sprout` (SM6125), Android 11. **Not copied anywhere** —
it stays on the workstation as the recovery material.

---

## 6. Risk assessment

| Risk | Assessment |
|---|---|
| Update fails signature check | **Not expected** — signers are identical |
| Data loss from the update itself | **Not expected** — same package and signer; data preserved |
| Rollback to the previous build | **Blocked by Android** — versionCode cannot decrease; uninstall is destructive |
| Bad build crash-looping at boot | Mitigated in design: `EdgeService` restarts the engine with bounded exponential backoff (capped at 60 s, limited attempts) and reports refusals rather than looping |
| Silent traffic redirection after update | **Not possible without a separate, explicit consent + peer enrolment** — out of scope of this update |
| Boot receiver invoked by another app | Exported with no permission guard, but both actions are system-protected broadcasts |
| Hardware/OS acceptance | **NOT TESTED** — see §7 |

## 7. What remains untested (must not be reported as certified)

- **No real-device installation of the candidate was performed** — prohibited in this mission. Package
  inspection is not device acceptance.
- **v2/v3/v4 signature cryptographic validity is unverified** (`apksigner` needs a JVM; none present).
  Only v1 was verified, plus the presence of the signing block and `.idsig`.
- **No on-device behaviour of this candidate was observed**: no service start, no MCP handshake, no
  coordinator bridge, no WebRTC, no consent flow on the phone. The synthetic/loopback verification
  that exists elsewhere in the project is **not** physical acceptance.
- VPN routing remains **untested on hardware** and is, by the app's own policy, refusing non-loopback
  destinations until a peer is enrolled and consent granted.

## 8. Recommendation

The candidate is a **defensible in-place upgrade**: same package, same signer, higher versionCode,
data preserved, and the install-blocking `resources.arsc` defect fixed. It is **not** reversible by
reinstalling the old build. If the intent is to test and possibly retreat, decide that **before**
updating, or plan to fix forward — do not plan on uninstall as the rollback.
