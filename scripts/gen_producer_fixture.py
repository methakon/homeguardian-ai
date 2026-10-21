#!/usr/bin/env python3
"""Generate a producer message fixture from the REAL emitter, not a hand-written line.

WHY THIS EXISTS
The coordinator asked for "producer message fixture using real emitter logic, compatible with
Hermes3 schema v1 and strict receiver, no fake only". A hand-copied JSON line proves nothing: it
cannot drift INTO the code, so it stays green while the emitter changes underneath it. That is the
same failure class as a test that cannot disagree with the thing it tests.

So this does not paraphrase the emitter. It EXTRACTS the emitter's own source text - `num_str()`,
`struct Observation`, `push_observation()` and `drain_observations_json()` - from
`apk/jni/hg_harness_jni.cpp` verbatim, compiles that extracted text on the host, drives it with inputs
that mirror the real capture paths, and writes the resulting bytes to a fixture file.

If the emitter changes its wire form, the extracted text changes with it and the fixture changes. The
fixture cannot silently disagree with the producer.

It also asserts the property that was actually broken: an observation constructed the way
push_observation() constructs it must NOT claim physical provenance.
"""
import re, subprocess, sys, pathlib, textwrap, os, json

SRC = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "apk/jni/hg_harness_jni.cpp")
OUT = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "hg_verify/producer_fixture.ndjson")
src = SRC.read_text(encoding="utf-8")

def grab(pattern, label):
    """Pull a verbatim definition out of the real source."""
    m = re.search(pattern, src, re.S)
    if not m:
        print(f"  EXTRACT FAILED: {label} - the emitter changed shape; update this extractor, "
              f"do not hand-write the fixture", file=sys.stderr)
        sys.exit(2)
    return m.group(0)

num_str   = grab(r"std::string num_str\(double v\)\s*\{.*?\n\}", "num_str")
obs       = grab(r"struct Observation\s*\{.*?\n    \};", "struct Observation")
push      = grab(r"void push_observation\(Node& n,.*?\n\}", "push_observation")
drain     = grab(r"std::string drain_observations_json\(\)\s*\{.*?\n    \}", "drain_observations_json")
kmax      = grab(r"constexpr size_t kMaxPendingObservations\s*=\s*\d+;", "kMaxPendingObservations")

# The extracted text still refers to Node/JNI. Provide the minimum real-shaped scaffolding so the
# EXTRACTED code compiles unmodified - the emitter text itself is never edited.
harness = f"""
// GENERATED HARNESS - the emitter functions below are extracted VERBATIM from
// {SRC}. Do not edit them here; edit the emitter and regenerate.
#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <iostream>

{kmax}

struct Node {{
    {obs}
    std::deque<Observation> observations;
    std::mutex m;
    uint64_t observations_emitted = 0;
    uint64_t observations_dropped = 0;
}};

static Node g_node;
Node& node() {{ return g_node; }}

{num_str}

{push}

{drain}

int main() {{
    // Mirror the REAL capture paths, so the fixture is what the device would actually emit.
    push_observation(node(), "motion", "motion-changed-fraction", 0.42, "fraction", "ok", 1.0, 1761000000000LL);
    push_observation(node(), "presence", "audio-rms", 0.0137, "rms", "ok", 1.0, 1761000000001LL);
    push_observation(node(), "presence", "audio-peak", 0.21, "amplitude", "ok", 1.0, 1761000000002LL);
    push_observation(node(), "presence", "audio-zero-crossing-rate", 0.0043, "rate", "ok", 1.0, 1761000000003LL);
    // A non-finite value must be DROPPED by the real emitter, not emitted as garbage.
    push_observation(node(), "motion", "motion-changed-fraction", std::nan(""), "fraction", "ok", 1.0, 1761000000004LL);

    std::cout << drain_observations_json();
    return 0;
}}
"""

work = pathlib.Path("/tmp/hg_fixture_build"); work.mkdir(parents=True, exist_ok=True)
cpp = work / "emitter_fixture.cpp"; cpp.write_text(harness, encoding="utf-8")

r = subprocess.run(["g++", "-std=c++17", "-O0", "-o", str(work / "gen"), str(cpp)],
                   capture_output=True, text=True)
if r.returncode != 0:
    print("  COMPILE FAILED (the extracted emitter does not stand alone):", file=sys.stderr)
    print(textwrap.indent(r.stderr[:2000], "    "), file=sys.stderr)
    sys.exit(3)

r = subprocess.run([str(work / "gen")], capture_output=True, text=True)
if r.returncode != 0:
    print("  RUN FAILED:", file=sys.stderr); print(r.stderr[:1000], file=sys.stderr); sys.exit(4)

raw = r.stdout
lines = [l for l in raw.split("\n") if l.strip()]
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(raw, encoding="utf-8")

print(f"  extracted verbatim from : {SRC}")
print(f"  fixture written         : {OUT}")
print(f"  lines emitted           : {len(lines)}  (4 expected: the NaN one must be dropped)")
for l in lines:
    try:
        o = json.loads(l)
        print(f"    {o['source_type']:<9} {o['observation_type']:<26} value={o['value']:<10} provenance={o['provenance']}")
    except Exception as e:
        print(f"    UNPARSEABLE: {l[:80]} ({e})")
        sys.exit(5)

# ------------------------------------------------------------------ the assertions that matter
fails = []
for l in lines:
    o = json.loads(l)
    if o.get("schema_version") != 1:
        fails.append(f"schema_version must be 1, got {o.get('schema_version')!r}")
    if o.get("provenance") in ("real", "real_device"):
        fails.append(f"provenance claims physical origin: {o.get('provenance')!r} - the receiver "
                     f"REFUSES this on a synthetic session; this was the live defect")
    if o.get("source_type") not in ("motion", "contact", "presence", "camera_availability",
                                    "microphone_availability", "device_availability", "tamper",
                                    "environmental_threshold"):
        fails.append(f"source_type off the closed vocabulary: {o.get('source_type')!r}")
    if o.get("observation_type") not in ("motion-changed-fraction", "audio-rms", "audio-peak",
                                         "audio-zero-crossing-rate", "activity-level", "availability",
                                         "contact-state", "temperature", "humidity", "pressure",
                                         "illuminance", "tamper-state"):
        fails.append(f"observation_type off the closed vocabulary: {o.get('observation_type')!r}")

if len(lines) != 4:
    fails.append(f"expected 4 observations, got {len(lines)} - the non-finite drop rule changed")

print()
if fails:
    print("  FIXTURE ASSERTIONS FAILED:")
    for f in fails: print(f"    - {f}")
    sys.exit(1)
print("  ALL FIXTURE ASSERTIONS PASS (schema_version=1, no real claim, closed vocabulary, NaN dropped)")
