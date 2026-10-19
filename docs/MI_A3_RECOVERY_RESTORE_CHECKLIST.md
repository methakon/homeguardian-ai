# Mi A3 — stock Android 11 restore checklist

The verified rollback path. Read the whole thing before running any of it.

## Verified artifact

```
file   laurel_sprout_global_images_V12.0.26.0.RFQMIXM_20220808.0000.00_11.0_2292e2c0b1.tgz
bytes  2439506308                     == Content-Length          VERIFIED
md5    2292e2c0b1c0e9839d5b14427154b5b4 == vendor-published      VERIFIED
sha256 0e6b3c1b17aa860bacae1b979d4f63dd007a060f84291363263aef577444fe3c
gzip   integrity test OK
build  V12.0.26.0.RFQMIXM  == the device's own fingerprint build
```

Kept at `/home/swarna-sekhar-dhar/stock-recovery/mi-a3/`. Note there are **two** vendor hosts
for this file: `bigota` served 33 KB/s, `cdnorg` served 3.43 MB/s.

## Anti-rollback — check this FIRST

`flash_all.sh` refuses to run at all if the device's anti-rollback version is newer than the
package's:

```sh
CURRENT_ANTI_VER=0
ver=`fastboot getvar anti`      # grep -oP "anti: \K[0-9]+"
if [ $ver -gt $CURRENT_ANTI_VER ]; then
  echo "Current device antirollback version is greater than this package"; exit 1
fi
```

**Consequence: if the device reports `anti` greater than 0, this package CANNOT restore it.**
That value is only readable in bootloader mode, and until it is read, rollback is unproven.
The script also self-checks the device identity:

```sh
fastboot getvar product | grep "^product: *laurel_sprout"   # else "Missmatching image and device"
```

## Which script to use

| Script | Effect | Use? |
|---|---|---|
| `flash_all.sh` | **erases and rewrites `userdata`** — full wipe | yes, for a clean restore |
| `flash_all_except_data_storage.sh` | erases boot/system/vendor/mdtp on both slots, **leaves userdata** | yes, to preserve user data |
| `flash_all_lock.sh` | ends with **`fastboot flashing lock`** | **NEVER** |

**Never run `flash_all_lock.sh`, and never use "clean all and lock".** Relocking over a
mismatched boot chain is a documented brick path.

Since a bootloader unlock already wipes the device, `flash_all.sh` is the honest choice for a
full restore — the data is already gone by then.

## What it writes

`boot, dtbo, dummy, metadata, misc, persist, product, system, system_other, userdata, vbmeta, vendor`
plus `crclist.txt` / `sparsecrclist.txt` (42-line integrity lists).

Slot-aware over A/B (`boot_a`/`boot_b`, `system_a`/`system_b`). `fastboot` must be >= 31.0.2;
ours is 37.0.1.

## Prerequisites before running

```
[ ] anti value read in bootloader mode and <= 0
[ ] fastboot transport proven stable in bootloader mode (not just adb)
[ ] fastboot on PATH or called by absolute path (~/Android/Sdk/platform-tools/fastboot)
[ ] battery > 50%, known-good data cable, rear port, no hub
[ ] `fastboot getvar all` captured and saved, for comparison afterwards
```

## Running it

```bash
cd /home/swarna-sekhar-dhar/stock-recovery/mi-a3/laurel_sprout_global_images_V12.0.26.0.RFQMIXM_11.0
FB=/home/swarna-sekhar-dhar/Android/Sdk/platform-tools/fastboot
"$FB" devices                 # must list the device
"$FB" getvar anti             # must be <= 0
bash ./flash_all.sh           # full restore, wipes userdata
```

## After a restore

```bash
"$FB" getvar all              # compare against the pre-flash capture
"$FB" oem device-info         # expect: unlocked (until relocked)
# first boot takes several minutes; do not interrupt it
```

Then re-enable developer options and USB debugging, since the wipe resets them.
Do **not** relock the bootloader unless the device is fully stock, booting, and you have decided
you are finished — relocking is the step with no recovery if anything is inconsistent.
