#!/usr/bin/env python3
"""Offline, advisory-only vector drift replay. No capture, inference or delivery.

Read bounded feature windows as JSONL from stdin; write one decision per window.
See docs/ANIMAL_FEATURE_REPLAY.md for the trust boundary and input contract.
"""
import argparse
import copy
import json
import math
import re
import sys

MAX_LINE = 131072
MAX_SOURCES = 8
MAX_DIMENSIONS = 1024
# Theta contains only routine and activity_change, never species or disasters.
# Mass order: {routine}, {activity_change}, {routine, activity_change}.
IGNORANCE = [0.0, 0.0, 1.0]


def finite_number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def combine(left, right):
    """Dempster's rule with explicit K; suppress instead of dividing by ~zero."""
    for mass in (left, right):
        if (not isinstance(mass, (list, tuple)) or len(mass) != 3
                or any(not finite_number(v) or v < 0 for v in mass)
                or not math.isclose(sum(mass), 1.0, abs_tol=1e-9)):
            raise ValueError("invalid mass function")
    product = [0.0, 0.0, 0.0]
    conflict = 0.0
    for a in range(1, 4):
        for b in range(1, 4):
            value = left[a - 1] * right[b - 1]
            intersection = a & b
            if intersection:
                product[intersection - 1] += value
            else:
                conflict += value
    if conflict >= 0.9:
        return IGNORANCE.copy(), conflict, True
    return [v / (1.0 - conflict) for v in product], conflict, False


class Replay:
    """Bounded frozen quiet baseline and consecutive-window cosine drift.

    Source ids are caller assertions, NOT proof of independent physical sensors.
    Fusion is experimental and can never make a decision eligible for alerting.
    """
    def __init__(self, baseline_samples=8, sustained_windows=3,
                 distance_threshold=0.35, max_gap_ms=5000):
        for name, value in (("baseline_samples", baseline_samples),
                            ("sustained_windows", sustained_windows),
                            ("max_gap_ms", max_gap_ms)):
            if type(value) is not int or not 2 <= value <= 1000000:
                raise ValueError("invalid " + name)
        if (not finite_number(distance_threshold)
                or not 0 < distance_threshold <= 2):
            raise ValueError("invalid distance_threshold")
        self.baseline_samples = baseline_samples
        self.sustained_windows = sustained_windows
        self.distance_threshold = distance_threshold
        self.max_gap_ms = max_gap_ms
        self.states = {}
        self.last_timestamp = None

    def observe(self, record):
        # Validate everything before committing state: a bad second source cannot
        # advance the first source's streak or poison its quiet baseline.
        if not isinstance(record, dict) or set(record) != {"timestamp_ms", "phase", "sources"}:
            raise ValueError("expected timestamp_ms, phase and sources only")
        timestamp = record["timestamp_ms"]
        if (type(timestamp) is not int or not 0 <= timestamp <= 2**53
                or (self.last_timestamp is not None and timestamp <= self.last_timestamp)):
            raise ValueError("timestamp must be a strictly increasing nonnegative integer")
        phase = record["phase"]
        if phase not in ("baseline", "observe"):
            raise ValueError("phase must be baseline or observe")
        sources = record["sources"]
        if not isinstance(sources, dict) or not 1 <= len(sources) <= MAX_SOURCES:
            raise ValueError("expected 1 to 8 sources")
        if len(set(self.states) | set(sources)) > MAX_SOURCES:
            raise ValueError("source state limit exceeded")
        vectors = {}
        for source, vector in sources.items():
            if not isinstance(source, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,48}", source):
                raise ValueError("invalid source identifier")
            if (not isinstance(vector, list) or not 2 <= len(vector) <= MAX_DIMENSIONS
                    or any(not finite_number(v) or not -1 <= v <= 1 for v in vector)):
                raise ValueError("expected 2 to 1024 finite vector components in [-1,1]")
            norm = math.sqrt(sum(v * v for v in vector))
            if norm <= 1e-12:
                raise ValueError("zero vector is not evidence")
            state = self.states.get(source)
            if state and len(vector) != len(state["sum"]):
                raise ValueError("source feature dimension changed")
            if phase == "observe" and state is None:
                raise ValueError("source has no explicit baseline")
            if phase == "baseline" and state and state["count"] >= self.baseline_samples:
                raise ValueError("baseline is frozen; start a new replay to retrain")
            vectors[source] = [v / norm for v in vector]

        states = copy.deepcopy(self.states)
        gap = self.last_timestamp is not None and timestamp - self.last_timestamp > self.max_gap_ms
        for source, state in states.items():
            if gap or source not in sources or phase == "baseline":
                state["streak"] = 0
        result = {"timestamp_ms": timestamp, "provenance": "offline_feature_replay",
                  "severity": "info", "alert_eligible": False,
                  "activity_change": False, "status": "baseline",
                  "conflict_k": 0.0, "conflict_steps": [],
                  "belief_activity_change": 0.0, "ignorance_mass": 1.0,
                  "fusion_suppressed": False, "sources": {},
                  "parameters_validated_on_animals": False}
        masses = []
        changed_count = 0
        pending_count = 0
        for source in sorted(vectors):
            vector = vectors[source]
            state = states.setdefault(source, {"sum": [0.0] * len(vector), "count": 0, "streak": 0})
            if phase == "baseline":
                state["sum"] = [a + b for a, b in zip(state["sum"], vector)]
                state["count"] += 1
                if state["count"] == self.baseline_samples:
                    if math.sqrt(sum(v * v for v in state["sum"])) <= 1e-12:
                        raise ValueError("quiet baseline centroid cancels to zero")
                result["sources"][source] = {"baseline_samples": state["count"]}
                continue
            if state["count"] < self.baseline_samples:
                result["sources"][source] = {"ready": False}
                continue
            norm = math.sqrt(sum(v * v for v in state["sum"]))
            cosine = sum(a * b for a, b in zip(vector, state["sum"])) / norm
            distance = max(0.0, min(2.0, 1.0 - cosine))
            above = distance >= self.distance_threshold
            state["streak"] = min(state["streak"] + 1, self.sustained_windows) if above else 0
            changed = state["streak"] >= self.sustained_windows
            changed_count += int(changed)
            pending_count += int(above and not changed)
            # These are uncalibrated simple-support masses, NOT probabilities of
            # animal presence. Pending drift contributes ignorance, not routine.
            mass = ([0.0, 0.75, 0.25] if changed else
                    IGNORANCE.copy() if above else [0.75, 0.0, 0.25])
            masses.append(mass)
            result["sources"][source] = {"ready": True, "cosine_distance": distance,
                                         "sustained": changed, "streak": state["streak"]}

        if phase == "observe":
            combined = IGNORANCE.copy()
            for mass in masses:
                combined, conflict, suppressed = combine(combined, mass)
                result["conflict_steps"].append(conflict)
                # Each step's K is conditional on the preceding nonempty mass.
                # Joint survival is the product of the conditional survivals.
                result["conflict_k"] += (1.0 - result["conflict_k"]) * conflict
                if suppressed:
                    result["fusion_suppressed"] = True
                    break
            if result["conflict_k"] >= 0.9:
                result["fusion_suppressed"] = True
                combined = IGNORANCE.copy()
            result["belief_activity_change"] = combined[1]
            result["ignorance_mass"] = combined[2]
            if len(sources) < 2:
                result["status"] = "insufficient_sources"
            elif len(masses) != len(sources):
                result["status"] = "insufficient_baseline"
            elif result["fusion_suppressed"]:
                result["status"] = "conflict_suppressed"
            elif changed_count and changed_count != len(sources):
                result["status"] = "disagreement"
            elif changed_count >= 2:
                result["status"] = "activity_change"
                result["activity_change"] = True
            elif pending_count:
                result["status"] = "pending_change"
            else:
                result["status"] = "routine"
        self.states = states
        self.last_timestamp = timestamp
        return result


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate JSON key")
        value[key] = item
    return value


def reject_constant(_value):
    raise ValueError("nonfinite JSON number")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-samples", type=int, default=8)
    parser.add_argument("--sustained-windows", type=int, default=3)
    parser.add_argument("--distance-threshold", type=float, default=0.35)
    parser.add_argument("--max-gap-ms", type=int, default=5000)
    args = parser.parse_args()
    line_number = 0
    try:
        replay = Replay(**vars(args))
        while True:
            line = sys.stdin.buffer.readline(MAX_LINE + 1)
            if not line:
                break
            line_number += 1
            if len(line) > MAX_LINE:
                raise ValueError("record exceeds 128 KiB")
            record = json.loads(line, parse_constant=reject_constant, object_pairs_hook=unique_object)
            result = replay.observe(record)
            print(json.dumps(result, allow_nan=False, separators=(",", ":")))
    except (ValueError, OverflowError, RecursionError) as error:
        # Never echo input features, identifiers or JSON decoder document text.
        message = "invalid JSON" if isinstance(error, json.JSONDecodeError) else str(error)
        if isinstance(error, (RecursionError, OverflowError)):
            message = "numeric or nesting limit exceeded"
        print(f"replay: line {line_number}: {message}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
