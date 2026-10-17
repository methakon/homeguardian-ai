# HG-CAM-03 — Phase 4: Factory Reset Decision Brief

**Status: report only. Nothing destructive has been done and nothing will be without
explicit confirmation.** No factory reset, no bootloader unlock, no flashing, no partition
formatting, no recovery modification, and no account removal — including the Google,
WhatsApp, Facebook and Jio accounts, which remain in place as instructed.

Inputs: `HG_CAM_02_PATH_B_OS_FEASIBILITY.md` and `HG_CAM_03_BACKUP_MANIFEST.md`.

---

## 1. Backup location and verification result

    encrypted archive (outside the repo, git clean -fdx cannot reach it)
      /home/swarna-sekhar-dhar/backups/homeguardian/
        panasonic-eluga-i7-20261017-123531-shared.tar.zst.gpg   1,548,691,213 bytes
        panasonic-eluga-i7-20261017-123531.tar.zst.gpg                210,712 bytes
        ....sha256                                                       (both archives)

    working copy (inside the repo, gitignored via /devices/backup/)
      ~/projects/homeguardian-ai/devices/backup/deviceprofile/panasonic-eluga-i7/

    key   ~/.config/hg-backup/eluga-i7.key   0600, 64 base64 chars, never printed or committed

| check | result |
|---|---|
| identity verified before any read (model + board + ABI + SDK) | PASS |
| top-level entries pulled | 14 / 14, 0 failures |
| files / bytes | 3,839 / 4.29 GB |
| disk-vs-manifest sample, 25 random files | 25 / 25 PASS |
| archive decrypt → extract → verify every entry | **3,839/3,839 PASS, 0 missing, 0 mismatched** |
| wrong key rejected (gpg exit 2, 0 bytes, "Bad session key") | PASS, encryption is real |
| key portable (decrypts from a copy of itself) | PASS |
| key and archive on **separate** physical storage | **NO — same disk; see §1.2** |

### 1.1 The test that actually matters

Re-hashing files that are still sitting in the backup directory against a manifest created
minutes earlier proves very little. The meaningful test — decrypt the archive, extract it,
compare the result against the manifest — is what `panasonic_eluga_verify_backup.sh` runs,
with the outcome recorded in `restore-test/archive_roundtrip.txt`.

One correction belongs on the record: the first restore test reported `0/0 sampled — FAIL`.
That was **my test being wrong, not the backup being corrupt** — `sha256sum` writes
`HASH<space><space>PATH` and my parser split on one space, so it silently matched nothing.
Fixed and re-run at 25/25. A test that can quietly sample zero files and still print a
verdict is more dangerous than no test, so it is recorded rather than erased.

### 1.2 Key escrow — needs an action only you can take

The key is portable and **not** machine-bound; it is a plain passphrase file, and I proved
it by decrypting the archive with a copy. But the key, the working backup and the archive
all resolve to the **same physical disk** (`/dev/sdc3`). That covers the stated threat
(`git clean -fdx`, accidental repo deletion) and **not** disk failure, theft, fire or
ransomware. This host has two unmounted internal disks (`sda` 223.6 GB, `sdb` 465.8 GB) and
no attached external media.

**Action needed from you:** put the key and the archive on media that leaves the building,
and keep the key **separate from** the archive. Do not put both on one device. A printed
copy of the 64-character key in a safe is a legitimate custody method. Until that is done,
a single disk failure loses everything.

## 2. Data that cannot be recovered

State this plainly, because it defines what a reset actually costs:

- **SMS/MMS, call log and contacts.** Verified `DENIED` at
  `com.android.providers.telephony/databases/mmssms.db`,
  `com.android.providers.contacts/databases/contacts2.db`,
  `com.android.providers.media/databases/external.db`. Not backed up. Recoverable only by
  a manual in-app/Google export performed **before** the reset, or not at all.
- **All private app data** — ~4.2 GB across 168 packages: app logins, chat databases,
  settings, saved documents, game state.
- **WhatsApp message history.** The 15 `crypt14` files copied into `WhatsApp/` are
  preserved but **not independently restorable** — they are decrypted with a key held in
  WhatsApp's private app data, which is not in the backup. Recovery depends on the in-app
  Google Drive backup, which is a separate, unverified-here path.
- **`nvram` / `proinfo` — unrecoverable by any means.** These MediaTek partitions carry
  per-unit calibration and IMEI. They are not readable without root, they are in no backup,
  and if a flash corrupts them the phone is permanently degraded. This is the single
  strongest technical argument against flashing anything on this device.
- **Every account credential.** Re-entered by hand after a reset, including the
  `gmail.legacyimap` / `gmail.pop3` accounts configured inside Gmail.
- **No partition image exists** — `boot`, `recovery`, `system`, `vendor`, `userdata`,
  `nvram`, `proinfo`, `seccfg`, `frp` all verified `DENIED` to a non-root shell.

## 3. Factory Reset Protection — the reactivation risk

**This is the item that can turn a routine reset into a locked phone.**

A Google account **is** signed in on this device (`dumpsys account` shows `com.google`
entries). On Android 5.1 through 9 with a 2021 security patch, that is the condition that
**arms FRP**.

**FRP requirement after a reset:** the setup wizard will refuse to complete until you sign
in with a Google account that was **previously added to this device**. This is not
bypassable by design, and I will not attempt to bypass it. If the credential for a
previously-added account is not available, the usable recovery routes are the official
Google account-recovery flow, or a paid service-centre intervention. FRP is also a
**security feature**, so the correct handling is to disarm it legitimately rather than
defeat it.

**The legitimate way to disarm it — before the reset, not after:**

1. Confirm you can sign in to every account currently on the device.
2. Remove **all** accounts on the device (Settings → Accounts). With no account present at
   reset time, FRP is not armed.
3. Only then perform the factory reset.

**You have instructed me not to remove those accounts, so I have not.** Steps 1–2 are
owner actions: removing an account prompts for the credential or screen lock, and
**do not send me any password, PIN or pattern** — I do not need them and will not accept
them. You perform the removal on the phone; I verify only that no account remains.

**Two facts I cannot verify from here, and which you must confirm:**

- **Whether you can sign back in to every account** on this device — Google (including
  the mail accounts configured inside Gmail), Facebook, WhatsApp and Jio. If any
  credential is lost or any account is unrecoverable, **do not reset**.
- **Whether the device has a screen lock**, which a reset and account removal will require.

If either answer is "no" or "not sure", the reset is unsafe and Path A can proceed without
it — a dedicated CCTV service does not require a factory reset to run.

## 4. Recommended OS and CCTV software

**OS: keep the installed stock Android 9. Do not install a replacement OS.**

From the Path B assessment, a replacement OS is not a judgement call — it is **blocked**:
no device-specific ROM exists (no LineageOS, no /e/OS, no unofficial build; the only
artifact for this exact model is auto-generated `twrpdtgen` output, which is not a ROM),
there is **no published kernel source** to build one from, the bootloader unlock the
firmware reports as `sys.oem_unlock_allowed=0` is not available as shipped, and a GSI would
most likely cost the camera — the one feature the device is being kept for.

**CCTV architecture — the confirmed approach:**

- **Foreground service**, targeting **API 28**. Android 9 names the foreground service as
  the fix for its camera/mic background restriction, and API 28 targeting sidesteps the
  Android 11+ `foregroundServiceType` gates entirely.
- **Stream, do not store.** `DCIM` alone is 2.83 GB and `/data` has only 975 MB free. A
  security camera that writes locally will fill this device; it must send frames onward.
- **Rear/front camera selection**, microphone uplink, speaker talkback, **encrypted
  transport**, and a **visible capture indicator**.
- Hardware is adequate and measured: `OMX.MTK.VIDEO.ENCODER.AVC` H.264 to 1920x1080
  (70–155 fps measured at 720p), PCM-16 audio to 48 kHz, AOSP acoustic echo canceller
  present, 2.4 GHz Wi-Fi at 65 Mbps.

**Specific APK selection and installation is the next approval gate.** No media APK has
been installed, no camera or microphone has been activated, and I am not recommending a
named APK before it has been evaluated against the requirement list above on this exact
device.

## 5. Factory reset versus keeping the installed OS

**Honest case for resetting** (it is not merely cosmetic):

- Clears 8.5 GB of accumulated data, on a device with 975 MB free.
- Removes **the previous owner's accounts and data**. A CCTV node carrying the prior
  owner's Google, Facebook, WhatsApp and Jio sessions is a genuine liability, and this is
  the strongest reason to reset.
- Removes 168 stale packages, including third-party apps that consume CPU on an idle
  device.
- Gives a known-clean starting state to build on.

**Honest case against:**

- **FRP lockout risk** — recoverable only with a previously-added Google credential.
  A failed reset can cost the device entirely.
- Destroys the unrecoverable data in §2 by design.
- Does not deliver a newer Android, better camera support or better hardware — it changes
  nothing in §4.
- Preinstalled Panasonic/MTK components may not be reinstallable if the OTA path is gone.

**Recommendation:** keep stock Android 9 (Path A), and reset **only** as a privacy-cleaning
step, **only** after (a) §1.1 verification is green, (b) you have independently confirmed
you can re-authenticate every account, and (c) all accounts have been removed by you so FRP
is not armed. The reset is **optional for function** — the CCTV service runs fine on the
current OS. It is **advisable for privacy**. That ordering is deliberate: the reset is not
on the critical path, so it should never be the step that puts the device at risk.

## 6. Recovery procedure if installation fails

Because the recommendation involves **no flashing**, this is the good case:

- **CCTV app misbehaves or will not start** → uninstall it. Nothing else was modified;
  the device is untouched. No recovery needed.
- **The factory reset hangs or bootloops** → boot **stock recovery** (which is verified and
  present: `ro.expect.recovery_id == ro.recovery_id`) and wipe cache/data from there.
  Nothing we do modifies recovery.
- **FRP blocks setup** → sign in with a previously-added Google account. No workaround
  will be attempted. If that fails, account recovery or a service centre is the route.
- **If you later choose to flash anyway (not recommended)** → recovery means the unverified
  third-party stock image (`EB-90S54E7Ev3006`/`v2009`, device is on `v3024`) or a service
  centre. An unverified firmware image on a device intended as a security camera is a
  supply-chain risk as well as a brick risk, which is why §4 recommends against it.

## 7. What I need before proceeding

1. **Confirmation you can sign back in to every account** currently on the device — Google
   (including mail accounts configured inside Gmail), Facebook, WhatsApp, Jio. Do not send
   credentials; just confirm the capability.
2. **Confirmation the device's screen lock is known to you.**
3. **A decision on the reset itself**: reset for privacy, or keep the current OS and just
   install the CCTV service.
4. **Key escrow completed** (§1.2) — off-machine, key separate from archive.
5. **Explicit approval before any camera/microphone activation or APK installation.**

**Stopping here for your final approval, as instructed.** No destructive action taken, no
accounts removed, no camera or microphone activated, no APK installed, and neither the
Mi A3 work, the HG-SEC-04 audit nor the production mail VPS was touched.
