# HG-CAM-03 — Panasonic ELUGA I7 Backup Manifest

Device: Panasonic `ELUGA_I7_EE`, ADB serial `LRW86SC6RO8TZ99H`, Android 9 / API 28,
board `k39tv1_64_a27`, MT6739, arm64-v8a. Identity was re-verified against model, board,
ABI and SDK **before** any read of the device, and the backup script refuses to run if any
of those four disagree.

Backup date: 2026-10-17. Script: `scripts/panasonic_eluga_backup.sh`.
Verifier: `scripts/panasonic_eluga_verify_backup.sh`.

## 0. What "complete" means here — read this first

This backup captures **shared storage and reconstructable configuration**. It does **not**
capture private app data, message databases, contacts, credentials or any partition image.
Those are listed as unrecoverable in §3–§5. Exit code 0 from the backup tooling means the
commands ran; it is **not** a statement that the device is safe to wipe. The decision to
wipe does not rest on exit codes — it rests on §6.

## 1. Result and verification

| item | value |
|---|---|
| top-level entries pulled | **14 of 14, 0 failures** |
| files in backup | **3,839** |
| bytes in backup | **4,288,477,523** (4.29 GB) |
| config files captured | 17 |
| manifest entries | 3,839 |
| disk-vs-manifest sample (25 random) | **25/25 hashes match — PASS** |
| archive round-trip (decrypt + extract + verify every entry) | **3,839/3,839 PASS — 0 missing, 0 mismatched** |
| wrong-key rejection | **REJECTED** — gpg exit 2, 0 bytes out, "Bad session key" |
| archive sha256 sidecar | written for both archives |

An earlier run of the restore test reported `0/0 sampled, FAIL`. That was a **bug in my
test, not corruption in the backup**: `sha256sum` emits `HASH<space><space>PATH`, and my
parser split on a single space, so every path lookup silently missed. The parser is fixed
and the test now reports 25/25. The old FAIL is recorded here rather than quietly removed,
because a test that can silently sample nothing is worse than no test.

## 2. Encrypted copy, and the archive round-trip

Stored **outside the repository**, so `git clean -fdx` cannot reach it:

    /home/swarna-sekhar-dhar/backups/homeguardian/
      panasonic-eluga-i7-20261017-123531-shared.tar.zst.gpg   1,548,691,213 bytes
      panasonic-eluga-i7-20261017-123531.tar.zst.gpg                210,712 bytes
      panasonic-eluga-i7-20261017-123531.tar.zst.gpg.sha256

AES-256 symmetric, compressed with zstd (4.29 GB → 1.55 GB). The small archive holds
config/ and manifest/; the large one holds shared/.

**Why the round-trip test is the one that matters:** re-hashing files still sitting in the
backup directory against a manifest generated minutes earlier proves almost nothing — it
cannot detect a corrupt, truncated or unreadable archive. Only decrypting the archive,
extracting it and comparing the *result* against the manifest tests restorability. That is
what `panasonic_eluga_verify_backup.sh` does, and the result was:

    result            : PASS
    manifest entries  : 3839
    hashes verified   : 3839        <- every entry, not a sample
    missing           : 0
    mismatched        : 0
    files restored    : 3839
    bytes restored    : 4,288,477,523   <- byte-identical to the backup

Full output is in `restore-test/archive_roundtrip.txt`, with the 4 GB extraction discarded
afterwards because the encrypted archive is the artefact of record. The test also re-checks
the archive against its `.sha256` sidecar first, so silent corruption after the fact is
caught rather than trusted.

**One correction had to be made to the artefact itself.** The small config+manifest archive
was first written at 12:47:40 — seconds after the buggy test run — so it carried that false
`FAIL` *inside it*, with no round-trip record beside it. That is actively misleading to
discover on decrypt, so the small archive was **rebuilt** rather than merely footnoted. It
is now 211,261 bytes and contains `result.txt` (PASS, 25/25), `result-buggy-parser-run.txt`
(the original buggy output, preserved verbatim rather than deleted) and
`archive_roundtrip.txt`.

The **shared archive was not touched** — its SHA-256 remains
`d4bba0d8238f1b9cd5c71d7f34a8577e089747eca06eab0f9af8f71e5be0747d`, identical to the value
recorded before the rebuild, so the round-trip PASS still applies to exactly those bytes.
Only the regenerated `.sha256` sidecar differs, because it now covers the rebuilt small
archive.

## 3. Recovered — in this backup

Aggregates only. Personal filenames are in `manifest/checksums.sha256` and are deliberately
not reproduced in this document.

| directory | files | bytes | contents |
|---|---:|---:|---|
| `DCIM` | 490 | 2,826,680,009 | camera photos and videos — the largest single item |
| `Android` | — | 977,986,567 | **app external data** (`Android/data`, `Android/media`) |
| `WhatsApp` | 486 | 448,050,383 | WhatsApp media, including 15 `crypt14` files — see §3.1 |
| `PhoneRecord` | 17 | 25,067,796 | **call recordings** |
| `Pictures` | 28 | 5,514,026 | images |
| `mtklog` | — | 5,178,742 | MediaTek diagnostic logs |
| `Download`, `Movies`, `Music`, `Alarms`, `Notifications`, `Podcasts`, `Ringtones`, `Cardboard` | 0 | 0 | present but empty |

By extension across the whole backup: 875 `jpg`, 35 `mp4`, 24 `png`, 17 `3gpp`, 12 `ogg`,
7 `mp3`, 4 `pdf`, 238 `exo`, larger counts of extension-less files under `Android/data`.

### 3.1 A caveat that matters for WhatsApp

`WhatsApp` contains 15 `crypt14` files — WhatsApp's **local encrypted message backups**.
They are copied, but they are **not independently restorable**: `crypt14` is decrypted with
a key held in WhatsApp's private app data (`/data/data/com.whatsapp`), which is not
readable without root and is not in this backup. Scope the claim to "the files are
preserved", not "the messages can be restored". Chat recovery depends on the in-app
Google Drive backup instead — see §4.

## 4. App-private data — NOT recovered, requires root or an in-app export

`/data` is 10 GB, 8.5 GB used, **975 MB free (90 % full)**. Of that, 4.29 GB is the shared
storage in §3. The remaining ~4.2 GB is private app data that ADB cannot read on a
`user`/`release-keys`, non-debuggable, encrypted build. Verified directly:

| database | path | readable? |
|---|---|---|
| SMS/MMS | `/data/data/com.android.providers.telephony/databases/mmssms.db` | **DENIED** |
| contacts | `/data/data/com.android.providers.contacts/databases/contacts2.db` | **DENIED** |
| media index | `/data/data/com.android.providers.media/databases/external.db` | **DENIED** |

168 packages are installed. Everything each of them stores privately — app settings,
logins, chat databases, saved documents, game state — is outside this backup. Recovery
for this class is **manual export from within each app** (e.g. an in-app chat export), or
the platform backup in §5, or root, which is not authorised.

## 5. Account-dependent data — re-authentication required after any reset

`dumpsys account` shows accounts of these **types** (identifiers deliberately not recorded
in this document):

    com.google                      com.facebook.auth.login
    com.google.android.apps.tachyon com.facebook.messenger
    com.google.android.gm.exchange  com.whatsapp
    com.google.android.gm.legacyimap   com.jio.myjio
    com.google.android.gm.pop3

Two findings that bear directly on the reset decision:

1. **Google Backup is configured and auto-restore is enabled**, with `D2dTransport` and
   `BackupTransportService` whitelisted, and a backup pass that has actually run. Some app
   data may therefore return after a reset — but the per-app state shows only partial
   coverage, so this can be treated as a bonus, never as the recovery plan. Completeness
   is not verifiable from this device.
2. The presence of `gmail.legacyimap` and `gmail.pop3` account types means **additional
   non-Google mail accounts are configured inside Gmail**. Those credentials live in
   app-private storage and will have to be re-entered by hand.

Also account-dependent and **not** in this backup: WhatsApp message history (holds
server-side/Drive, not on this phone) and every Google account's cloud state.

## 6. Inaccessible partitions — cannot be imaged

Verified individually: `boot`, `recovery`, `system`, `vendor`, `userdata`, `nvram`,
`proinfo`, `seccfg`, `frp` — **all DENIED** to a non-root shell. The device is
`ro.debuggable=0`, `ro.secure=1`, with `veritymode=enforcing`, so there is no read path
without unlocking the bootloader first.

The 38-entry partition map was recorded structurally to `config/partitions_byname.txt`.
**No partition image is in this backup, and none can be taken without the destructive
unlock this report is meant to avoid.** In particular there is no copy of `nvram`/`proinfo`
(which on MediaTek devices carry per-unit calibration and IMEI) — losing those is not
recoverable from any backup, and is one of the strongest arguments against flashing
anything.

## 7. Key preservation — verified

The key is `/home/swarna-sekhar-dhar/.config/hg-backup/eluga-i7.key`, mode `0600`,
64 base64 characters of CSPRNG output, generated on the workstation and **never printed,
logged or committed**.

- **Portable** — proven by decrypting the archive with a *copy* of the key: the key is a
  plain passphrase file passed to `gpg --passphrase-file`, not tied to this machine, to
  `~/.gnupg`, or to any keyring. It can be stored anywhere.
- **Load-bearing** — losing it loses the archive. There is no escrow, no recovery code and
  no second copy.
- **Not yet on separate storage** — the key, the repo backup and the archive all resolve to
  the **same physical device** (`/dev/sdc3`, the root ext4). That protects against
  `git clean -fdx` and accidental repo deletion, which was the stated threat. It does
  **not** protect against disk failure, theft, fire or ransomware.

This host has two further internal disks (`sda` 223.6 GB, `sdb` 465.8 GB) that are
**unmounted**, plus no attached external media. Copying to another internal disk would not
change the threat model meaningfully, and mounting one is a system change I have not made
and will not make without approval.

**Recommended, and only the owner can do it:** write the key and the archive to media that
leaves the building — external drive, another machine, or a password manager entry — and
store the key **separately from** the archive, so that one device holding both is not a
single point of failure. A printed copy of the key in a safe is a legitimate option; the
key is 64 characters and does not expire.

## 8. Honest limits of this backup

- Private app data, SMS/MMS, call log, contacts, app credentials: **not backed up**.
- No partition image; `nvram`/`proinfo` calibration is **unrecoverable by any means**.
- WhatsApp `crypt14` backups are preserved but **not independently restorable** (§3.1).
- `du` on `/sdcard` is unreliable on this device under FUSE — it reported `0` for
  `/sdcard` and 106 MB for `DCIM` when `DCIM` actually holds 2.83 GB. Every size in this
  document is measured on the **workstation side after transfer**, not from device output.
- One backup is one backup. It has not been re-read from the phone a second time to detect
  bit-rot, and it has not been restored to a device to test end-to-end recovery.
