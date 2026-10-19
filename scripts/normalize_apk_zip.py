#!/usr/bin/env python3
"""Normalise an APK's ZIP entry metadata so the archive is byte-reproducible.

Why this exists: the build assembles the APK by adding classes.dex and lib/ with `zip`, which stamps
those entries with the build wall-clock. aapt already normalises the entries it writes
(AndroidManifest.xml, resources.arsc) to a fixed 1980 epoch, so only the zip-added entries varied -
which made two builds of identical source produce different whole-file SHA-256 values.

This runs AFTER packaging and BEFORE zipalign/signing, because the signature covers the archive:
normalising after signing would invalidate it. zipalign preserves entry timestamps, so normalising
first is sufficient for the signed output to be stable.

What is normalised:
  * date_time  - all entries set to a fixed epoch (the same one aapt uses)
  * create_system - forced to 0 (MS-DOS), removing a host-platform variable
  * external_attr - normalised to a fixed mode, removing the umask/permission variable
  * entry order - preserved as-is, because the build adds entries in a deterministic order

Compression is fixed at DEFLATED with a fixed level; deflate output for a given input and level is
deterministic, so the compressed bytes are reproducible too.

Usage: normalize_apk_zip.py <apk-path>
"""
import shutil
import sys
import zipfile

# Match aapt's normalised epoch: 1980-01-01 00:00:00 is the ZIP epoch floor. aapt writes
# 1980-01-01 05:30 for its entries; a fixed 1980-01-01 00:00:00 is used here for all of them, which
# is stable and legal. The specific value does not matter as long as it never varies.
FIXED_DATE = (1980, 1, 1, 0, 0, 0)
FIXED_ATTR = 0o100644 << 16   # regular file, mode 0644, no host permission leakage
COMPRESS_TYPE = zipfile.ZIP_DEFLATED


def normalize(path):
    src = zipfile.ZipFile(path, "r")
    entries = [(info.filename, src.read(info.filename)) for info in src.infolist()]
    src.close()

    tmp = path + ".norm"
    with zipfile.ZipFile(tmp, "w", COMPRESS_TYPE) as out:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
            info.compress_type = COMPRESS_TYPE
            info.create_system = 0
            info.external_attr = FIXED_ATTR
            info.internal_attr = 0
            info.extra = b""
            info.comment = b""
            out.writestr(info, data)
    shutil.move(tmp, path)

    check = zipfile.ZipFile(path, "r")
    names = check.namelist()
    check.close()
    print("    normalised %d entries in %s" % (len(names), path))
    return names


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("usage: normalize_apk_zip.py <apk-path>\n")
        sys.exit(2)
    normalize(sys.argv[1])
