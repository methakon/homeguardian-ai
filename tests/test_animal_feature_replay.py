"""Synthetic regression inputs only; no private media or model inference."""
import importlib.util
import itertools
import json
from pathlib import Path
import subprocess
import sys
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/animal_feature_replay.py"


def load_module():
    spec = importlib.util.spec_from_file_location("animal_feature_replay", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()

    def setUp(self):
        self.engine = self.module.Replay(baseline_samples=3, sustained_windows=2)
        self.tick = 0

    def feed(self, sources=None, phase="observe", **extra):
        self.tick += 1
        record = {"timestamp_ms": self.tick * 1000, "phase": phase,
                  "sources": sources if sources is not None else
                  {"audio": [1, 0], "vision": [1, 0]}}
        record.update(extra)
        return self.engine.observe(record)

    def train(self):
        for _ in range(3):
            self.feed(phase="baseline")

    def test_quiet_baseline_and_quiet_observation(self):
        self.train()
        result = self.feed()
        self.assertEqual(result["status"], "routine")
        self.assertFalse(result["activity_change"])
        self.assertEqual(result["conflict_k"], 0)
        self.assertEqual(result["provenance"], "offline_feature_replay")
        self.assertFalse(result["alert_eligible"])

    def test_single_spike_not_sustained(self):
        self.train()
        changed = {"audio": [0, 1], "vision": [0, 1]}
        self.assertFalse(self.feed(changed)["activity_change"])
        self.assertFalse(self.feed()["activity_change"])

    def test_sustained_change_from_two_sources(self):
        self.train()
        changed = {"audio": [0, 1], "vision": [0, 1]}
        self.feed(changed)
        result = self.feed(changed)
        self.assertTrue(result["activity_change"])
        self.assertEqual(result["status"], "activity_change")
        self.assertGreater(result["belief_activity_change"], 0)
        self.assertEqual(result["conflict_k"], 0)

    def test_missing_source_resets_streak_and_no_stale_fusion(self):
        self.train()
        changed = {"audio": [0, 1], "vision": [0, 1]}
        self.feed(changed)
        result = self.feed({"audio": [0, 1]})
        self.assertEqual(result["status"], "insufficient_sources")
        self.assertFalse(self.feed(changed)["activity_change"])

    def test_gap_breaks_sustained_sequence(self):
        self.train()
        changed = {"audio": [0, 1], "vision": [0, 1]}
        self.feed(changed)
        self.tick += 60
        self.assertFalse(self.feed(changed)["activity_change"])

    def test_one_changed_source_cannot_assert_multimodal_change(self):
        self.train()
        mixed = {"audio": [0, 1], "vision": [1, 0]}
        self.feed(mixed)
        result = self.feed(mixed)
        self.assertFalse(result["activity_change"])
        self.assertEqual(result["status"], "disagreement")
        self.assertGreater(result["conflict_k"], 0)

    def test_warmup_is_not_routine(self):
        self.feed(phase="baseline")
        self.assertEqual(self.feed()["status"], "insufficient_baseline")

    def test_single_source_status_precedes_incomplete_baseline(self):
        self.feed({"audio": [1, 0]}, phase="baseline")
        result = self.feed({"audio": [1, 0]})
        self.assertEqual(result["status"], "insufficient_sources")
        self.assertFalse(result["sources"]["audio"]["ready"])
        self.assertFalse(result["activity_change"])
        self.assertFalse(result["alert_eligible"])

    def test_vector_norm_floor(self):
        for magnitude in (1e-13, 1e-12):
            with self.subTest(magnitude=magnitude):
                with self.assertRaises(ValueError):
                    self.feed({"audio": [magnitude, 0]}, phase="baseline")
                self.assertEqual(self.engine.states, {})
                self.assertIsNone(self.engine.last_timestamp)
        result = self.feed({"audio": [2e-12, 0]}, phase="baseline")
        self.assertEqual(result["sources"]["audio"]["baseline_samples"], 1)

    def test_baseline_cannot_be_silently_retrained(self):
        self.train()
        with self.assertRaises(ValueError):
            self.feed(phase="baseline")

    def test_invalid_record_does_not_mutate_state(self):
        self.train()
        for bad in ([float("nan"), 0], [float("inf"), 0], [0, 0],
                    [True, 0], [2, 0], [10**1000, 0], [1], [1] * 1025):
            with self.subTest(bad=str(bad)[:30]):
                with self.assertRaises(ValueError):
                    self.feed({"audio": [0, 1], "vision": bad})
                self.assertEqual(self.engine.states["audio"]["streak"], 0)
        self.assertEqual(self.feed()["status"], "routine")

    def test_invalid_timestamp_schema_and_source_count(self):
        for extra in ({"timestamp_ms": True}, {"timestamp_ms": -1},
                      {"phase": "capture"}, {"provenance": "physical"},
                      {"sources": {}}, {"sources": {str(i): [1, 0] for i in range(9)}}):
            with self.subTest(extra=extra):
                with self.assertRaises(ValueError):
                    self.feed(**extra)
        self.train()
        with self.assertRaises(ValueError):
            self.feed(timestamp_ms=1)

    def test_unknown_source_cannot_join_after_baseline(self):
        self.train()
        with self.assertRaises(ValueError):
            self.feed({"new": [1, 0]})

    def test_zero_centroid_rejected(self):
        self.engine = self.module.Replay(baseline_samples=2)
        self.feed({"audio": [1, 0]}, phase="baseline")
        with self.assertRaises(ValueError):
            self.feed({"audio": [-1, 0]}, phase="baseline")
        self.assertEqual(self.engine.states["audio"]["count"], 1)

    def test_total_conflict_suppressed_and_mass_validation(self):
        result = self.module.combine([1, 0, 0], [0, 1, 0])
        self.assertTrue(result[2])
        self.assertEqual(result[1], 1)
        for bad in ([0, 0, 0], [float("nan"), 0, 1], [-1, 1, 1]):
            with self.assertRaises(ValueError):
                self.module.combine(bad, [0, 0, 1])

    def test_fusion_normalization_and_ignorance(self):
        mass, conflict, suppressed = self.module.combine([0, .75, .25], [0, .75, .25])
        self.assertAlmostEqual(sum(mass), 1)
        self.assertEqual(conflict, 0)
        self.assertFalse(suppressed)
        self.assertAlmostEqual(mass[2], .0625)

    def test_joint_conflict_suppresses_six_contradictory_sources(self):
        quiet = {str(i): [1, 0] for i in range(6)}
        mixed = {str(i): [0, 1] if i % 2 else [1, 0] for i in range(6)}
        for _ in range(3):
            self.feed(quiet, phase="baseline")
        self.feed(mixed)
        result = self.feed(mixed)
        self.assertGreaterEqual(result["conflict_k"], 0.9)
        self.assertLess(max(result["conflict_steps"]), 0.9)
        self.assertTrue(result["fusion_suppressed"])
        self.assertEqual(result["status"], "conflict_suppressed")
        self.assertEqual(result["ignorance_mass"], 1)
        self.assertEqual(result["belief_activity_change"], 0)
        self.assertFalse(result["activity_change"])
        self.assertFalse(result["alert_eligible"])

    def test_joint_fusion_matches_unnormalized_enumeration(self):
        # Independent oracle: enumerate focal-set intersections before any
        # normalization, rather than reusing the production pairwise combiner.
        for count in range(2, 9):
            for changed_count in range(count + 1):
                for reverse in (False, True):
                    with self.subTest(count=count, changed=changed_count, reverse=reverse):
                        changed = [i < changed_count for i in range(count)]
                        if reverse:
                            changed.reverse()
                        joint = [0.0] * 4
                        choices = [[(2 if flag else 1, .75), (3, .25)] for flag in changed]
                        for selection in itertools.product(*choices):
                            intersection, weight = 3, 1.0
                            for focal, mass in selection:
                                intersection &= focal
                                weight *= mass
                            joint[intersection] += weight
                        self.engine = self.module.Replay(baseline_samples=3, sustained_windows=2)
                        quiet = {str(i): [1, 0] for i in range(count)}
                        observed = {str(i): [0, 1] if flag else [1, 0]
                                    for i, flag in enumerate(changed)}
                        for _ in range(3):
                            self.feed(quiet, phase="baseline")
                        self.feed(observed)
                        result = self.feed(observed)
                        self.assertAlmostEqual(result["conflict_k"], joint[0])
                        suppressed = joint[0] >= .9
                        self.assertEqual(result["fusion_suppressed"], suppressed)
                        self.assertAlmostEqual(result["ignorance_mass"],
                                               1 if suppressed else joint[3] / (1 - joint[0]))
                        self.assertAlmostEqual(result["belief_activity_change"],
                                               0 if suppressed else joint[2] / (1 - joint[0]))

    def test_config_validation(self):
        for config in ({"baseline_samples": 0}, {"sustained_windows": 0},
                       {"distance_threshold": float("nan")},
                       {"distance_threshold": 3}, {"max_gap_ms": -1}):
            with self.assertRaises(ValueError):
                self.module.Replay(**config)

    def test_cli_rejects_malformed_json_and_oversized_record(self):
        for data in ('{"timestamp_ms":NaN}\n', '{"phase":"observe"}\n',
                     '{"timestamp_ms":1,"timestamp_ms":2}\n', 'x' * 131073 + '\n'):
            run = subprocess.run([sys.executable, "-B", str(SCRIPT)], input=data,
                                 text=True, capture_output=True, timeout=10)
            self.assertEqual(run.returncode, 2)
            self.assertEqual(run.stdout, "")
            self.assertIn("line 1", run.stderr)
            self.assertNotIn(data.strip(), run.stderr)

    def test_cli_end_to_end(self):
        rows = []
        for i in range(6):
            rows.append({"timestamp_ms": i * 1000, "phase": "baseline" if i < 3 else "observe",
                         "sources": {"audio": [1, 0] if i < 3 else [0, 1],
                                     "vision": [1, 0] if i < 3 else [0, 1]}})
        run = subprocess.run([sys.executable, "-B", str(SCRIPT), "--baseline-samples", "3",
                              "--sustained-windows", "2"],
                             input="".join(json.dumps(row) + "\n" for row in rows),
                             text=True, capture_output=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        out = [json.loads(line) for line in run.stdout.splitlines()]
        self.assertEqual(len(out), len(rows))
        self.assertTrue(out[-1]["activity_change"])
        self.assertTrue(all(not row["alert_eligible"] for row in out))


if __name__ == "__main__":
    unittest.main()
