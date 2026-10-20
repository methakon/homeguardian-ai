# Task A — Stock Android 11 boot-image analysis (Mi A3 / laurel_sprout)

Package: `V12.0.26.0.RFQMIXM`  ·  Device: laurel_sprout (SM6125, trinket)
Method: every value below was read from the firmware images. Nothing is copied from another phone.

---

## 1. Recovery archive — VERIFIED

| Check | Result |
|---|---|
| Size | 2,439,506,308 B — equals published `Content-Length` **PASS** |
| MD5 | `2292e2c0b1c0e9839d5b14427154b5b4` — equals Xiaomi's published value **PASS** |
| gzip integrity | `gzip -t` OK **PASS** |
| Vendor manifest | `md5sum.xml`: **72 verified, 0 mismatched, 0 missing** **PASS** |

Self-contained verification: the manifest travels inside the package and authenticates every
file against itself, so this does not depend on the CDN staying honest.

> Note on a bug I made here: my first parse of `md5sum.xml` returned `0 entries / 72 missing`.
> Two errors — I matched an `md5="…"` attribute when the format is
> `<digest hash="md5" name="X">HASH</digest>` (hash is element *text*), and I resolved names
> against the extraction root when they are relative to `images/`. Corrected: 72/72 PASS.

## 2. boot.img — header VERIFIED

```
magic              ANDROID!
header_version     2
page_size          4096 B
header area        4096 B          (v2)
kernel_addr        0x00008000
kernel_size        14,977,930 B     compression: gzip (magic 1f 8b)
ramdisk_addr       0x01000000
ramdisk_size        8,279,472 B     compression: gzip (magic 1f 8b)
ramdisk expanded   19,917,056 B
second_size        0
tags_addr          0x00000100
recovery_dtbo_size 0   offset 0
dtb_size           0   dtb_addr 0x00000000      <-- NO device tree in boot.img
os_version         0x16000168
id (SHA-1)         b8e2cdbecfc445a9e48ef6f0f5c8612f99f5bfce
file size          67,108,864 B (16384 pages)
```

`os_version` `0x16000168` decodes to Android **11**, year **2022**, month **8** — matching the
package build date `20220808`. The header is self-consistent with the package.

### Derived layout (page-aligned, computed not assumed)

```
header   page 0       offset          0
kernel   page 1       offset       4096   size 14,977,930
ramdisk  page 3658    offset   14983168   size  8,279,472
```

### Boot command line (verbatim)

```
console=ttyMSM0,115200n8 androidboot.hardware=qcom androidboot.console=ttyMSM0
androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000
msm_rtb.filter=0x237 service_locator.enable=1 swiotlb=1
earlycon=msm_geni_serial,0x4a90000 loop.max_part=7 cgroup.memory=nokmem,nosocket
buildvariant=user
```

Two items matter for Task B: `console=ttyMSM0,115200n8` and
`earlycon=msm_geni_serial,0x4a90000` — the headless serial console is available on this board,
which is exactly what an unattended hub needs for diagnosis.

## 3. dtbo.img — the device tree is HERE, not in boot.img

```
magic              d7 b7 ab 1e  (Android DT table image)
page_size          4096
dt_entry_count     17 entries, of which 9 carry data
DT sizes           118,369 / 137,131 / 146,585 / 118,377 / 208,579 / 136,439
                   137,135 / 137,123 / 136,443  bytes
FDT version        16 (all)
file size          8,388,608 B
```

**These are Qualcomm downstream device trees**, not mainline. This is the central Task B
problem and it is unresolved — see §6.

## 4. A/B layout — VERIFIED from partition.xml

The package ships the ODM partition table. Every slot-aware partition exists twice:

```
boot_a boot_b      dtbo_a dtbo_b      vbmeta_a vbmeta_b
system_a system_b  vendor_a vendor_b  product_a product_b
odm_a odm_b        modem_a modem_b    dsp_a dsp_b
xbl_a xbl_b        abl_a abl_b        hyp_a hyp_b   tz_a tz_b
cmnlib_a cmnlib_b  cmnlib64_a cmnlib64_b  keymaster_a keymaster_b
devcfg_a devcfg_b  featenabler? (catefv)  qupfw_a qupfw_b
imagefv_a imagefv_b  uefisecapp_a uefisecapp_b  storsec_a storsec_b
bluetooth_a bluetooth_b  mdtp_a mdtp_b  mdtpsecapp_a mdtpsecapp_b
```

Non-slotted: `misc metadata persist frp fsc fsg devinfo ddr cdt secdata
limits logfs logdump minidump rawdump splash spunvm toolsfv gsort multiimgoem
multiimgqti keystore bk03 apdp`, plus `last_parti`.

**Confirmed A/B (seamless update) device.** The package also ships
`gpt_main0-5.bin`, `gpt_backup0-5.bin`, `gpt_both0-5.bin`, `rawprogram0-5.xml`,
`patch0-5.xml`, `prog_firehose_ddr.elf` — a complete EDL/QFIL recovery path.

### Active slot

```
ro.boot.slot_suffix = _b        (read from the running device)
```

**The device is on slot B.** Any slot-targeted write would therefore address `boot_b`,
`dtbo_b`, `vbmeta_b`. `boot_a`/`dtbo_a`/`vbmeta_a` are the untouched counterparts.

## 5. vbmeta.img — PARTIALLY analysed, fields UNVERIFIED

Unambiguous facts (from the raw hex dump, no struct assumptions):

```
magic          "AVB0"                     -> it IS an AVB2 vbmeta image
release string "avbtool 1.1.0"            (present at offset 128)
file size      4,096 B
header region  bytes 0x00-0xDF are almost entirely zero apart from that string
beyond header  a scan finds real data runs: 286 B @256, 147 B @678, 368 B @826 ...
```

**NOT verified, and deliberately not recorded as values:** `algorithm_type`,
`authentication_data_block_size`, `signature_size`, `public_key_size`, `flags`,
`rollback_index`, and the descriptor list.

I attempted this parse three times with three different struct offsets and got three different
answers (e.g. `rollback_index` came out as `14637248544768`, then `3408`, then `0`). Three
disagreeing results means none is correct. `avbtool` is absent from this host and is not
installable (`pip download avbtool` → no matching distribution), so there is no authoritative
decoder available here.

**Per the task instruction — an unverified component is named, not substituted.** These fields
resolve one of two ways, both requiring something I do not have right now:

1. `avbtool info_image --image vbmeta.img` on a host that has avbtool.
2. The device's own record, which is *more* authoritative anyway:
   `ro.boot.vbmeta.device_state`, `ro.boot.verifiedbootstate`, `ro.boot.veritymode`,
   `ro.boot.vbmeta.digest`, `ro.boot.vbmeta.hash_alg`, `ro.boot.vbmeta.avb_version`,
   `ro.boot.flash.locked`. These were not captured because the device left ADB for the
   fastboot session and has not returned.

### What IS safe to say

The file is a genuine AVB2 vbmeta and it is only 4096 B. A vbmeta carrying a real RSA public
key *and* signature is substantially larger than that, so on its face this looks unsigned. But
**"looks unsigned" is an inference from size, not a verified reading**, and I am labelling it
as such rather than stating it as fact. `verifiedbootstate` was observed as `green` on the
running device earlier — which, if the boot chain were genuinely unsigned, would require
explanation. These two observations are in tension and I am not going to resolve that tension
by guessing.

## 6. The unresolved Task B question — where does mainline's DT come from?

`boot.img` has `dtb_size = 0`. `dtbo.img` holds 9 Qualcomm *downstream* DTs. Mainline Linux
needs its own `sm6125-xiaomi-laurel-sprout.dtb` (41,049 B, already built and hashed).

So the stock layout gives no slot for the mainline DT in its native form. Candidate approaches,
**none of which I have validated**:

| Approach | Status |
|---|---|
| Put the DTB in the boot v2 `dtb` section (offset 1632) | plausible; the field exists and stock leaves it empty. **Untested.** |
| Append the DTB to the kernel `Image` | standard arm64 convention; **untested** against this bootloader |
| Ship DTB inside the initramfs and have the initramfs/kernel pick it up | plausible; **untested** |
| Use `dtbo_b` with a mainline-style DTBO | **unknown whether this bootloader accepts a non-Qualcomm DTBO** |

**I am not selecting one.** This is precisely the "do not invent a substitute" case: choosing
wrongly here produces a device that does not boot, and the choice is cheap to test only after
fastboot actually works.

## 7. Artifact hashes

```
boot.img   67,108,864 B  201b9e9f76fa13819c0c229a05749e2c5901bc470332a95dcf2a5ec497cfece4
dtbo.img    8,388,608 B  4d445058bf5558e3de5ac3f736b09070c62ca4392210d11ab1e36100f27f363e
vbmeta.img      4,096 B  b008c232392e7ecd9abbe9fda72a4015b38cf2f323d9287853b1ce9ec0af0a4d
system.img 2,885,079,956 B a484ef5bd5b0719219faa601e073211a...
vendor.img   897,700,228 B 212a6720925ad11263703055fc8e78e1...
```

## 8. Bottom line for Task A

Verified: archive integrity, boot header version/sizes/offsets/cmdline, kernel+ramdisk
compression, A/B partition layout, active slot, DTBO structure.

Unverified and named as such: all vbmeta AVB fields.

**Blocking everything downstream:** fastboot USB does not enumerate on this device
(`error -71`, kernel power-cycle also failed). No read-only fastboot query could be run, so
`anti`, `slot-successful`, `current-slot` and `get_unlock_ability` remain **UNKNOWN** — and
an unknown value is not a verified rollback index.
