#!/usr/bin/env bash
# evidence_segment_15s.sh - a ~18 second evidence segment built from COMMITTED output only.
#
# WHY THIS EXISTS
#   The G04/G10 handoff needs a short, precise segment of what the physical USB camera experiment actually
#   demonstrated, without starting new capture and without changing any camera setting. So this replays the
#   real, already-committed log lines, paced to ~18 seconds, and labels them on screen. It opens no device,
#   writes no media and retains nothing.
#
# CLASS: REPLAY of REAL capture output. The lines are genuine physical-camera output; the replay is not a
#   capture, and it must never be presented as one. The banner says so, on screen, for the whole segment.
#
# USAGE   scripts/evidence_segment_15s.sh [REV]        (default REV: the audited state)
set -euo pipefail
REV="${1:-ee58844e2ca6dde52e111dd899304127b358efa5}"
E=docs/evidence

line() { printf '%s\n' "$*"; }
pause() { sleep "${1:-2}"; }
show() {  # show <file> <grep-pattern> [label]
  local f="$1" pat="$2" label="${3:-}"
  git show "${REV}:${E}/${f}" 2>/dev/null | grep -E "$pat" | head -4 || true
  line "        ^ source: ${E}/${f} @ ${REV:0:8}${label:+ ($label)}"
}

line "════════════════════════════════════════════════════════════════════════"
line " HomeGuardian - physical USB UVC camera evidence, ${REV:0:8}"
line " REPLAY of committed output. No camera is opened. Nothing is retained."
line "════════════════════════════════════════════════════════════════════════"
pause 2

line ""
line "[1] A real capture: bounded scalars only, no image retained"
git show "${REV}:${E}/PHYSICAL_CAMERA_STILL_SCENE.log" 2>/dev/null | sed -n '2,4p'
pause 2

line ""
line "[2] The opening change was the CAMERA settling, not movement"
show PHYSICAL_CAMERA_CHARACTER_RUN.log 'seq=6' 
line "        the descriptor itself classifies it global_illumination -"
line "        an earlier reading of this line as a subject walking past is withdrawn."
pause 3

line ""
line "[3] Ambient scene, three separate runs: no deliberate subject"
show PHYSICAL_CAMERA_AMBIENT_RUN_3.log 'seq=291'
pause 5

line ""
line "[4] A real brightness step (a light switched on), and the separation"
show PHYSICAL_CAMERA_BRIGHTNESS_STEP2.log 'RESULT|peak motion|frames'
show PHYSICAL_CAMERA_BRIGHTNESS_STEP2.log 'motion-char|character' 
line "        before correction 65 motion-character labels / 120 observations;"
line "        after correction 19 / 120 - the labels are a descriptor, not a detection."
pause 5

line ""
line "[5] What is NOT claimed here"
line "        no species classification, no disaster prediction, no live camera-to-alert"
line "        operation, no V380 stream, no deployment. Synthetic suites cover the spine."
line ""
line " REPLAY of committed output. No camera was opened for this segment."
line "════════════════════════════════════════════════════════════════════════"
