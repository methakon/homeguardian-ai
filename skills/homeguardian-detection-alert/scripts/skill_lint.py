#!/usr/bin/env python3
"""Lint the homeguardian-detection-alert skill AND re-check its claims against the real source.

Three checks, in increasing order of usefulness:
  1. structure    - frontmatter, trigger-shaped description, required sections
  2. anchors      - every file:line the skill quotes still resolves, and the symbol it claims is there
  3. behaviour    - the state->detection mapping is PARSED OUT OF src/core/SecurityObservation.cpp, and the
                    skill's valid/invalid examples are judged against that parse (not against a copy of the
                    rules, which would drift).
Exit code 0 = the skill still tells the truth about this checkout.
"""
from __future__ import annotations
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
SKILL = ROOT / "skills" / "homeguardian-detection-alert"
fails, checks = [], 0

def check(cond, label, detail=""):
    global checks
    checks += 1
    if cond:
        print(f"  ok    {label}")
    else:
        print(f"  FAIL  {label}  {detail}")
        fails.append(label)

def text(p: pathlib.Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")

# ---------------------------------------------------------------- 1. structure
skill = text(SKILL / "SKILL.md")
fm = re.match(r"^---\n(.*?)\n---\n", skill, re.S)
check(bool(fm), "SKILL.md has YAML frontmatter")
if fm:
    for key in ("name:", "description:", "version:"):
        check(key in fm.group(1), f"frontmatter has {key}")
    desc = re.search(r"description:\s*(.+)", fm.group(1))
    check(bool(desc) and len(desc.group(1)) >= 20, "description is a usable trigger",
          f"got {desc.group(1)[:60] if desc else None!r}")
for section in ("Trigger", "Architecture", "Observation schema", "Live timestamps", "liveness",
                "Evidence detector IDs", "cooldown", "negative controls", "Tested positive",
                "Audit trail", "hypotheses only", "new detector", "Changing the engine", "Evidence discipline"):
    check(section.lower() in skill.lower(), f"SKILL.md covers: {section}")
check("not a runtime" in skill.lower() or "not** a runtime" in skill.lower(),
      "SKILL.md states it is guidance, not a runtime")
for f in ("README.md", "references/code-map.md", "references/observation-schema.md",
          "references/evidence-allowlist-and-fusion.md", "references/animal-early-warning-research.md",
          "examples/valid-observation.json", "examples/invalid-observation.json",
          "examples/positive-fixture.md", "examples/new-detector-checklist.md", "scripts/verify_skill.sh"):
    check((SKILL / f).exists(), f"referenced file exists: {f}")

# ---------------------------------------------------------------- 2. anchors
# (file, line, symbol that must appear on that line or the next few)
ANCHORS = [
    ("src/core/SecurityObservation.cpp", 75, "security_status_for"),
    ("src/core/SecurityObservation.cpp", 78, "on"),            # motion: a detection
    ("src/core/SecurityObservation.cpp", 95, "offline"),       # availability: problem states detect
    ("src/core/SecurityObservation.cpp", 124, "sensor."),
    ("src/core/SecurityObservation.cpp", 127, "authorized"),
    ("src/core/FusionEngine.cpp", 26, "inconclusive is not a vote"),
    ("src/core/DetectionFusionRule.h", 36, "min_usable"),
    ("src/core/DetectionFusionRule.h", 37, "min_confidence"),
    ("src/core/DetectionFusionRule.h", 50, "activity-energy"),
    ("src/core/DetectionFusionRule.h", 55, "use_episode_evidence"),
    ("src/core/Pipeline.h", 15, "correlation_window"),
    ("src/net/ObservationIngest.h", 51, "motion-changed-fraction"),
    ("src/main_ingress.cpp", 296, "policy.admit"),
    ("src/main_ingress.cpp", 297, "record_alert"),
    ("scripts/verify_fusion_window.sh", 1, ""),
    ("scripts/verify_alert_causality.sh", 1, ""),
]
for rel, line, symbol in ANCHORS:
    p = ROOT / rel
    if not p.exists():
        check(False, f"anchor {rel}:{line}", "file missing")
        continue
    lines = text(p).splitlines()
    check(len(lines) >= line, f"anchor {rel}:{line} exists", f"file has {len(lines)} lines")
    if symbol:
        window = "\n".join(lines[max(0, line - 2):line + 2])
        check(symbol in window, f"anchor {rel}:{line} still says {symbol!r}")

# every file:line reference quoted anywhere in the skill must resolve
quoted = 0
for f in list(SKILL.rglob("*.md")):
    for m in re.finditer(r"`?((?:src|scripts|deploy|tools)/[\w./-]+\.(?:cpp|h|sh|json|py)):(\d+)", text(f)):
        rel, ln = m.group(1), int(m.group(2))
        quoted += 1
        p = ROOT / rel
        if not p.exists():
            check(False, f"quoted ref resolves: {rel}:{ln}", "file missing")
        elif len(text(p).splitlines()) < ln:
            check(False, f"quoted ref resolves: {rel}:{ln}", "line beyond EOF")
check(quoted >= 20, "the skill quotes real file:line anchors", f"found {quoted}")

# ---------------------------------------------------------------- 3. behaviour
src = text(ROOT / "src" / "core" / "SecurityObservation.cpp")
fn = re.search(r"security_status_for\(SecuritySourceType type, const std::string& state\) \{(.*?)\n\}\n",
               src, re.S)
check(bool(fn), "parsed security_status_for out of the real source")
sets: dict[str, dict[str, set]] = {}
if fn:
    body = fn.group(1)
    # The function is a `switch (type)` with lowercase enum case labels. Some labels share one body (the three
    # availability types do), so consecutive bodyless labels are merged onto the next body-bearing block. A
    # state set is followed by either `return DetectorStatus::x;` or a braced return on the next line.
    # Guessing the key here silently produced an empty parse, and an empty parse makes a negative check pass
    # vacuously -- so every case below is required to be non-empty before it is trusted.
    SET = re.compile(r"state_in\(state, \{([^}]*)\}\)\)\s*(?:\{\s*)?return DetectorStatus::(\w+)", re.S)
    labels = list(re.finditer(r"case SecuritySourceType::(\w+):", body))
    pending: list[str] = []
    for i, m in enumerate(labels):
        pending.append(m.group(1).lower())
        end = labels[i + 1].start() if i + 1 < len(labels) else len(body)
        seg = body[m.end():end]
        found = list(SET.finditer(seg))
        if not found:
            continue
        d: dict[str, set] = {}
        for sm in found:
            d.setdefault(sm.group(2), set()).update(
                s.strip().strip('"') for s in sm.group(1).split(",") if s.strip())
        for label in pending:
            sets[label] = d
        pending = []
    check(len(sets) >= 5, "parsed every source-type case out of security_status_for", f"got {sorted(sets)}")
for t, expect_positive in (("motion", "on"), ("presence", "present"), ("contact", "open"),
                           ("tamper", "tamper"), ("environmental_threshold", "crossed"),
                           ("camera_availability", "offline"), ("microphone_availability", "offline")):
    det_set = sets.get(t, {}).get("detected", set())
    check(bool(det_set),
          f"{t}: detected-state set parsed (non-empty, so the checks below mean something)", f"{det_set}")
    check(expect_positive in det_set, f"{t} + state '{expect_positive}' is a DETECTION")
det = sets.get("motion", {}).get("detected", set())
check("present" not in det,
      "motion + state 'present' is NOT a detection (the blocker this skill warns about)", f"{det}")
nondet = sets.get("motion", {}).get("not_detected", set())
check("off" in nondet, "motion + state 'off' is an explicit NEGATIVE vote (lowers agreement)", f"{nondet}")
pres = sets.get("presence", {}).get("detected", set())
check("present" in pres, "presence + state 'present' is a DETECTION (the corroborating frame)", f"{pres}")
avail_det = sets.get("microphone_availability", {}).get("detected", set())
check("offline" in avail_det and "online" not in avail_det,
      "availability is detected only for PROBLEM states - the negative control's foundation", f"{avail_det}")

# the examples are judged against that parse
valid = json.loads(text(SKILL / "examples" / "valid-observation.json"))
valp = json.loads(text(SKILL / "examples" / "valid-observation-presence.json"))
bad = json.loads(text(SKILL / "examples" / "invalid-observation.json"))
check(valid["features"]["state"] in det, "valid-observation.json really is a positive vote")
check(valp["features"]["state"] in pres, "valid-observation-presence.json really is a positive vote")
check(str(bad["features"]["state"]) not in det,
      "invalid-observation.json is invalid for the reason claimed", f"state={bad['features']['state']}")
for ex in (valid, valp, bad):
    for k in ("source_type", "observation_type", "device_id", "sequence", "confidence", "provenance_claim"):
        check(k in ex, f"example frame carries {k}")
    check(all(v != "LIVE-EPOCH-MS" or k == "acquired_at_ms" for k, v in ex.items()),
          "example does not ship a pinned timestamp as if live")

print(f"\n  skill lint: {checks - len(fails)} ok, {len(fails)} failed")
sys.exit(1 if fails else 0)
