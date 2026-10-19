# Backup and restore verification report

Independent re-verification of the Mi A3 user-file backup, performed on request after the previous
turn's commit was accepted.

## What this backup is — and is not

**It is** a copy of owner user files from Android's shared storage.

**It is not** a full Android recovery image. There is no system partition, firmware, bootloader,
recovery, app-private data, contacts, messages, account data, credential, ADB key, signing key, SIM
or device identifier anywhere in it. Restoring it would return your photos, videos, music and
downloads — it would not restore an installed app, its settings, or the device's state.

## Location

```
/home/swarna-sekhar-dhar/projects/homeguardian-ai/devices/backup/deviceprofile/mi-a3-20261019-155609
```

Sits beside the pre-existing `panasonic-eluga-i7` folder, preserving the per-device convention already
established at that path. `devices/backup/` is **gitignored** — `git check-ignore` confirms it, and
`git status` reports zero files from that path, so these private files cannot be committed or
published. Nothing was uploaded to Google Drive, Oracle or any other cloud destination.

## Verification results

| Check | Result |
|---|---|
| Coverage | **941 / 941 files** |
| Size | 1.7 GB |
| Manifest | 941 entries |
| Integrity — every file re-hashed against the manifest | **941 checked, 0 mismatches** |
| Sample restore into a fresh temp directory | 13 files across DCIM/Pictures/Movies/Music/Download |
| Restored copies compared byte-for-byte (`cmp`) | **0 differing** |
| Timestamps preserved (`adb pull -a`) | yes |
| Gitignored | yes |

Per-folder, against device truth:

```
DCIM 99/99    Pictures 819/819    Movies 19/19    Music 1/1    Documents 0/0    Download 3/3
```

## User-files-only confirmation

Top-level content: `DCIM  Documents  Download  Movies  Music  Pictures` — the standard shared-storage
folders, nothing else.

Checked for and **absent**: `Android/data`, `Android/obb`, `shared_prefs`, `databases`, `accounts`,
`.keystore`, `adbkey`, `id_rsa`, `token`. No `.db`, `.sqlite` or preference `.xml` files were found.

File-type spread: 913 jpg, 20 mp4, 3 mp3, 2 png, **3 `database_uuid`**.

**On those three `database_uuid` files:** they are Android media-scanner sidecar files that live in the
public media directories, not app-private data. They are harmless and part of the shared-storage tree
you asked to back up. Flagged rather than quietly omitted.

## Restore procedure

```bash
# restore everything into a chosen directory (nothing is written back to the phone)
cp -a devices/backup/deviceprofile/mi-a3-20261019-155609/. /path/to/restore/
# then verify
cd /path/to/restore && sha256sum -c MANIFEST.sha256
```

Important: run `sha256sum -c` from **inside** the backup directory. The manifest stores paths
relative to it (`./DCIM/...`), so running the check from a parent directory reports every file as a
failure. That mistake produced a false "941 mismatches" once during this work; the data was fine and
the check was wrong.

## How this backup was produced, and two mistakes worth recording

Serial per-folder `adb pull -a`, one folder at a time, with per-folder retries. The transfer is
read-only with respect to the phone: nothing was deleted, moved or modified on the device.

1. **Parallel transfer failed badly.** At six concurrent workers the copy failed **705 of 941** files.
   The serial pull completed **941/941** in one pass. On a link that drops every few minutes,
   concurrency starves itself. The owner asked for multiple threads; the right call here was serial,
   and the evidence is recorded rather than the preference.
2. **A cleanup step deleted a completed copy.** A `find "$DIR" -name 'mi-a3-*'` cleanup matched the
   backup directory by its *own* basename and removed it. The phone was never written to, so recovery
   was a re-run — but it cost a full re-transfer and the pattern is now fixed.

## Standing limitations

- No off-phone second copy exists. If this workstation is lost, the backup is lost. An approved
  secure destination would be needed for a second copy, and there is none configured.
- No incremental backup. A future run re-transfers everything; the script skips files already present
  at the expected size, so re-runs are cheaper, but there is no true delta.
- Media sidecar files (`database_uuid`) are included; they are harmless but not user content.
