#!/usr/bin/env python3
"""Mutation-test the receiver's schema/provenance detections.

WHY THE FIRST VERSION OF THIS WAS WRONG
    It reported "NOTHING - a blind spot" for four mutants. The truth was the opposite: a patch had been mangled
    by shell escaping, the build FAILED with a compile error, the OLD binary was still in place, and the tests
    passed against it. "Tests pass" was being read as "the mutant was not detected" without ever checking that
    the mutant had been compiled at all.

    So every step is now verified: the patch must apply exactly, the build must succeed, and the test binary
    must have CHANGED before its result means anything. A mutant that cannot be built is reported as invalid
    rather than quietly counted as undetected.
"""
import hashlib
import pathlib
import subprocess
import sys

CPP = pathlib.Path("src/net/PanasonicNodeReceiver.cpp")
BIN = pathlib.Path("build/homeguardian_tests")
ENV = {
    "HG_SOURCE_DIR": str(pathlib.Path.cwd()),
    "PATH": "/usr/bin:/bin:/snap/bin:" + str(pathlib.Path.home() / ".local/bin"),
    "HOME": str(pathlib.Path.home()),
}

MUTANTS = [
    ("M1 presence inferred from the value again (the sentinel bug)",
     "    if (!parsed.has_schema_version) {",
     "    if (parsed.schema_version == kSchemaVersionAbsent) {",
     ["schema", "producer-contract"]),
    ("M2 strict-mode refusal disabled",
     "        if (cfg_.require_schema_version) {",
     "        if (false) {",
     ["schema", "fixture"]),
    ("M3 provenance vocabulary widened: any non-empty claim counts as real",
     'bool claim_is_real(const std::string& c) { return c == "real" || c == "real_device"; }',
     'bool claim_is_real(const std::string& c) { return !c.empty(); }',
     ["provenance", "compat"]),
    ("M4 no-escape guard disabled (the producer cannot escape a quote)",
     "            if (c == '\"' || c < 0x20) {",
     "            if (false) {",
     ["producer-contract"]),
    ("M5 an absent version is no longer counted",
     "        ++schema_absent_;\n        if (cfg_.require_schema_version) {",
     "        if (cfg_.require_schema_version) {",
     ["schema", "fixture"]),
]


def build():
    r = subprocess.run(["cmake", "--build", "build", "--target", "homeguardian_tests", "-j6"],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr)


def run_tag(tag):
    r = subprocess.run([str(BIN), f"[{tag}]"], capture_output=True, text=True, env=ENV)
    failed = r.returncode != 0 or "FAILED" in r.stdout
    return failed


def digest():
    return hashlib.sha256(BIN.read_bytes()).hexdigest()[:12]


def main() -> int:
    original = CPP.read_text(encoding="utf-8")
    baseline = digest()
    print(f"baseline binary {baseline}")
    rows = []
    for name, old, new, tags in MUTANTS:
        if old not in original:
            print(f"  {name}\n      INVALID: mutation site not found; fix the test, not the result")
            rows.append((name, "site-not-found", []))
            continue
        CPP.write_text(original.replace(old, new, 1), encoding="utf-8")
        rc, out = build()
        if rc != 0:
            errors = [l for l in out.splitlines() if "error:" in l][:1]
            print(f"  {name}\n      INVALID: mutant does not compile ({errors})")
            rows.append((name, "build-failed", []))
            CPP.write_text(original, encoding="utf-8")
            continue
        after = digest()
        if after == baseline:
            print(f"  {name}\n      INVALID: binary did not change - the mutant was never compiled")
            rows.append((name, "binary-unchanged", []))
            CPP.write_text(original, encoding="utf-8")
            continue
        caught = [t for t in tags if run_tag(t)]
        print(f"  {name}\n      caught by: {caught if caught else 'NOTHING - a blind spot'}")
        rows.append((name, "caught" if caught else "survived", caught))
        CPP.write_text(original, encoding="utf-8")

    # restore and prove the tree is back to green
    CPP.write_text(original, encoding="utf-8")
    rc, _ = build()
    restored = run_tag("panasonic") is False
    print(f"\n  restored: build_rc={rc} · [panasonic] green again={restored} · binary {digest()}")
    survived = [n for n, status, _ in rows if status in ("survived", "build-failed", "site-not-found")]
    print(f"  mutants: {len(rows)} · survived/undetected: {len(survived)}")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
