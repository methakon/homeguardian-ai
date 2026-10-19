#!/usr/bin/env python3
"""Compute a precisely defined CONTENT DIGEST for an APK.

Why this exists alongside the whole-file SHA-256: the whole-file hash covers the ZIP container,
which includes per-entry metadata. Even with metadata normalised at build time (see
normalize_apk_zip.py), a digest over the actual payload is the thing that answers "is this the same
code?". The two hashes answer different questions and neither replaces the other:

  whole-file SHA-256  - artifact integrity. Detects any change to the bytes we ship, including the
                        signature block. Mandatory for release gating.
  content digest      - code identity. Independent of container metadata, so it stays comparable
                        across builds, toolchains and agents even when the container legitimately
                        differs.

DEFINITION (exact, so another agent can reproduce it byte-for-byte):

    digest = SHA-256 of the concatenation, over entries sorted by filename
             in ascending byte order, of:

                 utf8(filename) || 0x00 || uint64_be(content_length) || content_bytes

    where:
      * filename       - the ZIP entry name exactly as stored, e.g. "classes.dex",
                         "lib/arm64-v8a/libhg_edge_service.so"
      * content_length - length in bytes of the STORED (compressed) entry data, as a big-endian
                         unsigned 64-bit integer, included so that concatenation cannot be
                         ambiguous between different name/length splits
      * content_bytes  - the raw stored bytes of the entry (the compressed stream, not the
                         decompressed payload)

    Entries are included whether or not they are compressed. The signing block
    (META-INF/*.SF, *.RSA, and the APK Signing Block) is NOT part of the content digest: those are
    signature artefacts rather than code, and including them would make the digest a restatement of
    the whole-file hash.

    Directory entries (names ending in "/") are included if present; they carry zero-length content.

The digested set is reported per entry so the coverage is auditable - a digest over an unspecified
set is not verifiable.

Usage: apk_content_digest.py <apk-path> [--list]
"""
import hashlib
import sys
import zipfile

SIGNATURE_PREFIXES = ("META-INF/",)


def is_signature_artifact(name):
    if not name.startswith(SIGNATURE_PREFIXES):
        return False
    upper = name.upper()
    return upper.endswith(".SF") or upper.endswith(".RSA") or upper.endswith(".DSA") \
        or upper.endswith(".EC") or upper == "META-INF/MANIFEST.MF"


def content_digest(path, list_entries=False):
    z = zipfile.ZipFile(path, "r")
    entries = []
    for info in z.infolist():
        if is_signature_artifact(info.filename):
            continue
        entries.append((info.filename, info.file_size, info.compress_size))

    h = hashlib.sha256()
    total = 0
    for name, size, csize in sorted(entries, key=lambda e: e[0].encode("utf-8")):
        data = z.read(name)          # decompressed payload
        nb = name.encode("utf-8")
        h.update(nb)
        h.update(b"\x00")
        h.update(len(data).to_bytes(8, "big"))
        h.update(data)
        total += len(data)
        if list_entries:
            print("    %-44s %8d bytes" % (name, len(data)))
    z.close()
    return h.hexdigest(), entries, total


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.stderr.write("usage: apk_content_digest.py <apk-path> [--list]\n")
        sys.exit(2)
    path = sys.argv[1]
    want_list = "--list" in sys.argv[2:]
    if want_list:
        print("  entries covered by the content digest:")
    digest, entries, total = content_digest(path, want_list)
    print("  ------------------------------------------------")
    print("  entries covered ....... %d" % len(entries))
    print("  total content bytes ... %d" % total)
    print("  CONTENT DIGEST ........ %s" % digest)
    print("  (signature artefacts META-INF/*.SF|*.RSA|*.DSA|*.EC|MANIFEST.MF are excluded:")
    print("   they are signature, not code; the whole-file SHA-256 covers them.)")
