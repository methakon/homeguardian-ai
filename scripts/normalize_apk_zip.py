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

Compression is preserved per entry rather than forced, because the method is part of the APK's
correctness, not just its size. Specifically, `resources.arsc` MUST be STORED (uncompressed):
Android 11+ refuses to install an APK whose targetSdkVersion is 30 or above when resources.arsc is
deflated ("Failed parse during installPackageLI: Targeting R+ ... requires the resources.arsc of
installed APKs to be stored uncompressed"). aapt already writes it STORED, so preserving the source
method keeps that property. Deflate output for a given input and level is deterministic, so
preserving the original method is still byte-reproducible.

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

# Entries that must never be deflated, whatever their source method was. resources.arsc is a hard
# install requirement for targetSdk >= 30; the manifest is forced STORED for the same class of
# reason and because it costs nothing at this size.
FORCE_STORED = {"resources.arsc", "AndroidManifest.xml"}


def normalize(path):
    src = zipfile.ZipFile(path, "r")
    # Preserve (name, data, original compress_type) so the archive keeps each entry's storage method.
    entries = [(info.filename, src.read(info.filename), info.compress_type)
               for info in src.infolist()]
    src.close()

    forced = []
    tmp = path + ".norm"
    with zipfile.ZipFile(tmp, "w") as out:
        for name, data, stored_type in entries:
            method = zipfile.ZIP_STORED if name in FORCE_STORED else stored_type
            if stored_type != method:
                forced.append(name)
            info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
            info.compress_type = method
            info.create_system = 0
            info.external_attr = FIXED_ATTR
            info.internal_attr = 0
            info.extra = b""
            info.comment = b""
            out.writestr(info, data)
    shutil.move(tmp, path)

    check = zipfile.ZipFile(path, "r")
    names = check.namelist()
    # Fail loudly rather than shipping an APK that cannot install.
    arsc = check.getinfo("resources.arsc") if "resources.arsc" in names else None
    check.close()
    print("    normalised %d entries in %s" % (len(names), path))
    if forced:
        print("    forced to STORED: %s" % ", ".join(sorted(forced)))
    if arsc is not None and arsc.compress_type != zipfile.ZIP_STORED:
        raise SystemExit("normalize_apk_zip: resources.arsc is not STORED - this APK will not "
                         "install on targetSdk >= 30")
    return names


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("usage: normalize_apk_zip.py <apk-path>\n")
        sys.exit(2)
    normalize(sys.argv[1])
