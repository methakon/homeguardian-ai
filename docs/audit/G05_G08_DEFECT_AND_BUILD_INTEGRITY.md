# G05 / G08 — Build defect clearance and build-integrity findings

Covers mission steps 2 (the APK install defect) and 7 (`supervisor.sh` / `SHA256SUMS`).

---

# Part 1 — Why the earlier APK (`e3116ac18d70…`) could not install

## 1.1 Root cause

The earlier candidate was normalised by `scripts/normalize_apk_zip.py`, which at that revision forced
one compression method on **every** entry in order to make the archive byte-reproducible:

```diff
-COMPRESS_TYPE = zipfile.ZIP_DEFLATED
-    entries = [(info.filename, src.read(info.filename)) for info in src.infolist()]
-    with zipfile.ZipFile(tmp, "w", COMPRESS_TYPE) as out:
-        for name, data in entries:
-            info.compress_type = COMPRESS_TYPE
```

That deflated `resources.arsc`. Android 11+ refuses to install an APK whose `targetSdkVersion` is 30
or above when `resources.arsc` is deflated — the install-time failure the fix commit quotes verbatim:

```
Failed parse during installPackageLI: Targeting R+ (version 30 and above) requires
the resources.arsc of installed APKs to be stored uncompressed and aligned on a 4-byte boundary
```

So the artifact was not corrupt and the code was not at fault: a **reproducibility transform changed
packaging semantics**, and the result was structurally uninstallable on the target OS.

## 1.2 The fix (`c8a8a78d4025e6795bad8d136d56bd2c0f9fa052`)

```diff
+# Entries that must never be deflated, whatever their source method was. resources.arsc is a hard
+# install requirement for targetSdk >= 30; the manifest is forced STORED for the same class of
+# reason and because it costs nothing at this size.
+FORCE_STORED = {"resources.arsc", "AndroidManifest.xml"}
+    entries = [(info.filename, src.read(info.filename), info.compress_type) ...]
+    with zipfile.ZipFile(tmp, "w") as out:
+        for name, data, stored_type in entries:
+            method = zipfile.ZIP_STORED if name in FORCE_STORED else stored_type
+    # Fail loudly rather than shipping an APK that cannot install.
```

Per-entry method is now **preserved**, with `resources.arsc` and `AndroidManifest.xml` forced STORED,
and the script fails loudly instead of emitting an uninstallable APK. Deflate remains deterministic,
so byte-reproducibility is retained.

## 1.3 Independently confirmed on the corrected artifact

Measured on the real file, not inferred from the commit:

```
resources.arsc: method=0 (STORED / uncompressed)  size=588  crc=6223b5de
```

**PASS.** The property the fix exists to guarantee is present in the shipped bytes.

Note: the older harness APK (2026-10-15) also has `resources.arsc` STORED, consistent with `aapt`
writing it stored by default. The regression was introduced by the normalisation step, not by `aapt`.

## 1.4 Honest scope limit

The broken artifact's bytes (`e3116ac18d70…`) are **not present on this workstation**, so its defect
is established from (a) the fix's own diff and (b) the corrected artifact's measured property —
**not** by re-measuring the broken file. Anyone holding that file can confirm cheaply:

```bash
python3 -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); print([(i.filename,i.compress_type) for i in z.infolist() if i.filename=='resources.arsc'])" e3116ac1.apk
# expect: method 8 (DEFLATED) = the install failure
```

---

# Part 2 — `supervisor.sh` vs `SHA256SUMS` mismatch (step 7)

## 2.1 What is wrong

`deploy/linux_arm64/SHA256SUMS` records a digest for `supervisor.sh` that the file does not have:

```
recorded: 9695a5a63d4105371b263589f0bfb67ffc5ab55c2555f0dee8c55885d7422ef3
actual  : 566c7f65c4c0d8d4d527d6fdfc78cd2803c61952785c09e1c1ba7a7580aa8933   (4500 bytes, 113 lines)
```

The other two recorded entries are correct: `hg_mcp_server` (`82b6cd87…`) and
`homeguardian.demo.json` (`6c84a474…`) both verify.

## 2.2 Cause — established, not guessed

- `SHA256SUMS` and `supervisor.sh` were introduced in the **same commit**: `76697eb`
  (2026-10-19 20:54:32, *"feat(linux-runtime): add physical deployment bundle and on-phone install
  procedure"*).
- `supervisor.sh` has **exactly one revision** in history (added in `76697eb`), so **no committed
  revision ever matched the recorded value** — this is not later drift.
- Byte-level alternatives were tested and none reproduce the recorded digest:

```
committed bytes       566c7f65c4c0d8d4d527d6fdfc78cd2803c61952785c09e1c1ba7a7580aa8933
CRLF-converted        35e9e1c62823fd436f0ae3b0743c2938a5711c7c71cfcd5b28c6c424cb54a025
trailing-newline off  006a230f4a9fc29af1ee629b4effa345c7ed2011b33171cfc92748052eb871d2
recorded              9695a5a63d4105371b263589f0bfb67ffc5ab55c2555f0dee8c55885d7422ef3   <-- matches nothing
```

Conclusion: the sums were generated from a working-tree copy that differed from the bytes finally
committed — a **generation-time defect inside that commit**, not corruption since.

## 2.3 Corrective action (recommended, not applied)

1. Regenerate `SHA256SUMS` from the **committed bytes at the branch tip** — do not hand-edit a hash
   to make a check pass, and do not delete the file.
2. If any device installed from that bundle, re-verify the deployed `supervisor.sh` against the true
   hash `566c7f65…` rather than the recorded one.
3. Add a CI check that recomputes every entry in `SHA256SUMS` and fails on mismatch, so a manifest can
   never again be committed describing bytes that were not committed.

## 2.4 Incidental: the script's security posture is worth preserving

`supervisor.sh` hardcodes `BIND="127.0.0.1"` with the comment *"never configurable to 0.0.0.0 here"*,
passes `HG_MCP_ALLOW_REMOTE=0`, and refuses to start unless the credentials file is **mode 600 or
stricter**. Whatever is decided about the manifest, those properties should not be lost in the repair.
