"""Tests for MAC-anchored camera recovery.

The interesting cases are the *refusals*: a recovery mechanism that scans when it
should not is worse than one that never scans, because it disturbs a working
camera and floods the LAN. Every guard is asserted on directly.
"""

from __future__ import annotations

import time
import unittest

from camera_ingest.recovery import (
    CameraLocator,
    RecoveringCameraSource,
    RecoveryWatcher,
    normalise_mac,
    parse_neigh_table,
    subnet_of,
)

CAM = "02:00:5e:00:53:01"
OTHER = "aa:bb:cc:dd:ee:ff"


class TestIdentity(unittest.TestCase):
    def test_mac_is_canonicalised(self) -> None:
        self.assertEqual(normalise_mac("02-00-5E-00-53-01"), CAM)
        self.assertEqual(normalise_mac(" 02:00:5e:00:53:01 "), CAM)

    def test_a_non_mac_is_rejected_loudly(self) -> None:
        for bad in ("", "not-a-mac", "02:00:5e:00:53", "192.168.1.13"):
            with self.assertRaises(ValueError, msg=bad):
                normalise_mac(bad)

    def test_subnet_of(self) -> None:
        self.assertEqual(subnet_of("192.168.1.13"), "192.168.1.")
        self.assertEqual(subnet_of("10.0.0.9"), "10.0.0.")
        with self.assertRaises(ValueError):
            subnet_of("192.168.1")

    def test_neigh_table_parses_mac_to_address(self) -> None:
        text = (
            "192.168.1.1 dev wlp6s0 lladdr 11:22:33:44:55:66 REACHABLE\n"
            f"192.168.1.13 dev wlp6s0 lladdr {CAM} STALE\n"
            "192.168.1.99 dev wlp6s0 FAILED\n"          # no lladdr -> skipped
            "192.168.1.7 dev wlp6s0 lladdr nonsense REACHABLE\n"  # bad MAC -> skipped
        )
        table = parse_neigh_table(text)
        self.assertEqual(table[CAM], "192.168.1.13")
        self.assertEqual(table["11:22:33:44:55:66"], "192.168.1.1")
        self.assertEqual(len(table), 2)


class _FakeSource:
    """A camera that answers only at its current address.

    Failure is modelled by *address*, not by a flag, because that is what a DHCP
    move actually looks like: the old address stops answering while the camera is
    perfectly healthy at a new one.
    """

    def __init__(self, host: str = "192.168.1.13", reachable_at: str | None = None) -> None:
        self._host = host
        self.reachable_at = reachable_at if reachable_at is not None else host
        self.calls = 0
        self.addresses_tried: list[str] = []

    @property
    def fail(self) -> bool:
        return self._host != self.reachable_at

    @fail.setter
    def fail(self, value: bool) -> None:
        # Convenience for the tests that only care "wedged" vs "fine".
        if value:
            self.reachable_at = "<nowhere>"
        elif self.reachable_at == "<nowhere>":
            self.reachable_at = self._host

    @property
    def name(self) -> str:
        return f"fake:{self._host}"

    def stream_url(self, *_a, **_k) -> str:
        return f"http://{self._host}/snapshot"

    def probe(self, timeout: float = 8.0):
        return self._attempt()

    def snapshot(self, timeout: float = 8.0) -> bytes:
        return self._attempt()

    def open_stream(self, timeout: float = 10.0, chunk: int = 0):
        self._attempt()
        yield b"chunk"

    def _attempt(self):
        self.calls += 1
        self.addresses_tried.append(self._host)
        if self._host != self.reachable_at:
            raise ConnectionError(f"nothing answering at {self._host}")
        return b"jpeg"


def _locator(neigh_text: str, *, clock=None, sweep=None, last="192.168.1.13", interval=60.0):
    """A locator whose sweep is a no-op unless a test supplies one.

    Tests must never put real traffic on this machine's LAN, so the real sweep is
    injected explicitly rather than inherited from the default.
    """
    return CameraLocator(
        mac=CAM,
        last_host=last,
        min_scan_interval_s=interval,
        neigh_reader=lambda: neigh_text,
        sweep=sweep if sweep is not None else (lambda subnet, timeout: None),
        clock=clock or (lambda: 0.0),
    )


class TestRecoveryGuards(unittest.TestCase):
    def test_never_scans_without_failure_evidence(self) -> None:
        loc = _locator("")  # empty table: a scan WOULD run if it were allowed
        outcome = loc.maybe_rediscover(had_failure=False, live=True)
        self.assertFalse(outcome.found)
        self.assertFalse(outcome.scanned)
        self.assertEqual(loc.scan_count, 0, "a healthy camera must never be swept")

    def test_never_scans_while_live_even_with_stale_failure_flag(self) -> None:
        loc = _locator("")
        outcome = loc.maybe_rediscover(had_failure=True, live=True)
        self.assertFalse(outcome.found)
        self.assertEqual(loc.scan_count, 0)

    def test_scan_runs_at_most_once_per_interval(self) -> None:
        now = {"t": 1000.0}
        loc = _locator("", clock=lambda: now["t"], interval=60.0)
        first = loc.maybe_rediscover(had_failure=True, live=False)
        self.assertTrue(first.scanned)
        self.assertEqual(loc.scan_count, 1)

        now["t"] += 5.0  # well inside the floor
        second = loc.maybe_rediscover(had_failure=True, live=False)
        self.assertFalse(second.scanned)
        self.assertEqual(second.reason, "refused: scan interval not elapsed")
        self.assertEqual(loc.scan_count, 1, "the one-minute floor must hold")

        now["t"] += 60.0
        third = loc.maybe_rediscover(had_failure=True, live=False)
        self.assertTrue(third.scanned)
        self.assertEqual(loc.scan_count, 2)

    def test_neighbour_read_is_free_and_allowed_inside_the_interval(self) -> None:
        """A cached hit costs nothing, so it is not rate-limited."""
        now = {"t": 1000.0}
        loc = _locator(f"192.168.1.13 dev wlp6s0 lladdr {CAM} STALE\n",
                       clock=lambda: now["t"])
        first = loc.maybe_rediscover(had_failure=True, live=False)
        self.assertTrue(first.found)
        now["t"] += 1.0
        again = loc.maybe_rediscover(had_failure=True, live=False)
        self.assertTrue(again.found)
        self.assertEqual(loc.scan_count, 0, "a table read must not consume the scan budget")


class TestRealScenarios(unittest.TestCase):
    def test_restart_at_the_same_address(self) -> None:
        """The camera reboots on the same lease: it is still in the table."""
        src = _FakeSource("192.168.1.13")
        loc = _locator(f"192.168.1.13 dev wlp6s0 lladdr {CAM} STALE\n")
        rec = RecoveringCameraSource(src, loc)

        src.fail = True
        with self.assertRaises(ConnectionError):
            rec.snapshot()
        self.assertEqual(rec.state, "DISCONNECTED")

        src.fail = False
        self.assertEqual(rec.snapshot(), b"jpeg")
        self.assertEqual(rec.state, "LIVE")
        self.assertEqual(rec.consecutive_failures, 0)
        self.assertEqual(src._host, "192.168.1.13")

    def test_address_move_is_repaird_automatically(self) -> None:
        """The DHCP lease moved: the MAC is the identity, so we follow it."""
        # The camera is genuinely healthy — just no longer at the old address.
        src = _FakeSource("192.168.1.13", reachable_at="192.168.1.77")
        # The neighbour table proves the MAC now lives at .77.
        loc = _locator(f"192.168.1.77 dev wlp6s0 lladdr {CAM} REACHABLE\n")
        rec = RecoveringCameraSource(src, loc)

        # One call is all it takes: it fails at .13, repairs, and succeeds at .77.
        self.assertEqual(rec.snapshot(), b"jpeg")
        self.assertEqual(src._host, "192.168.1.77", "must follow the MAC, not the address")
        self.assertEqual(loc.last_host, "192.168.1.77")
        self.assertEqual(rec.state, "LIVE")
        self.assertEqual(src.addresses_tried, ["192.168.1.13", "192.168.1.77"])
        self.assertEqual(loc.scan_count, 0, "a table hit needs no sweep at all")

    def test_move_recovered_by_sweep_when_the_table_is_cold(self) -> None:
        """After a power cycle the neighbour entry can be gone; sweep, then find it."""
        table = {"text": ""}

        def sweep(subnet: str, timeout: float) -> None:
            self.assertEqual(subnet, "192.168.1.")
            table["text"] = f"192.168.1.88 dev wlp6s0 lladdr {CAM} REACHABLE\n"

        now = {"t": 0.0}
        src = _FakeSource("192.168.1.13")
        loc = CameraLocator(
            mac=CAM, last_host="192.168.1.13", neigh_reader=lambda: table["text"],
            sweep=sweep, clock=lambda: now["t"],
        )
        rec = RecoveringCameraSource(src, loc)

        rec._note_failure()
        self.assertTrue(rec._repoint())
        self.assertEqual(src._host, "192.168.1.88")
        self.assertEqual(loc.scan_count, 1)

    def test_camera_genuinely_gone_is_reported_not_masked(self) -> None:
        """If the MAC is not on the subnet, say so and keep the last address."""
        src = _FakeSource("192.168.1.13")
        loc = _locator("")  # table has nothing; sweep finds nothing
        rec = RecoveringCameraSource(src, loc)

        src.fail = True
        with self.assertRaises(ConnectionError):
            rec.snapshot()
        self.assertEqual(rec.state, "DISCONNECTED")
        self.assertEqual(src._host, "192.168.1.13", "an unverified move must not be adopted")
        self.assertEqual(loc.last_reason, "not_found")

    def test_success_clears_the_failure_streak(self) -> None:
        src = _FakeSource()
        rec = RecoveringCameraSource(src, _locator(f"192.168.1.13 dev wlp6s0 lladdr {CAM} STALE\n"))
        for _ in range(3):
            rec._note_failure()
        self.assertEqual(rec.consecutive_failures, 3)
        rec.snapshot()
        self.assertEqual(rec.consecutive_failures, 0)
        self.assertEqual(rec.state, "LIVE")


class TestEnrolment(unittest.TestCase):
    """Recovery is opt-in: enrolling a MAC is what arms it.

    These tests must not read this machine's real .env: the owner's camera MAC may
    be enrolled there, which would make "no MAC configured" impossible to express.
    Each test therefore names its own (absent) dotenv explicitly.
    """

    #: A path that is guaranteed not to exist, so no MAC can be discovered.
    NO_DOTENV = "/nonexistent/hg-test-absent.env"

    def setUp(self) -> None:
        import os

        self._saved = os.environ.pop("HG_CAMERA_MAC", None)

    def tearDown(self) -> None:
        import os

        if self._saved is not None:
            os.environ["HG_CAMERA_MAC"] = self._saved
        else:
            os.environ.pop("HG_CAMERA_MAC", None)

    def test_no_mac_means_no_wrapper(self) -> None:
        from camera_ingest.recovery import recovering_source_from_env

        src = _FakeSource()
        got = recovering_source_from_env(src, dotenv_path=self.NO_DOTENV)
        self.assertIs(got, src, "an unenrolled camera must not be wrapped")

    def test_enrolling_a_mac_arms_recovery(self) -> None:
        import os

        from camera_ingest.recovery import RecoveringCameraSource, recovering_source_from_env

        os.environ["HG_CAMERA_MAC"] = CAM
        src = _FakeSource()
        wrapped = recovering_source_from_env(src, dotenv_path=self.NO_DOTENV)
        self.assertIsInstance(wrapped, RecoveringCameraSource)
        assert isinstance(wrapped, RecoveringCameraSource)  # narrow for the type checker
        self.assertEqual(wrapped._locator.mac, CAM)

    def test_a_malformed_mac_is_rejected_rather_than_ignored(self) -> None:
        import os

        from camera_ingest.recovery import recovering_source_from_env

        os.environ["HG_CAMERA_MAC"] = "not-a-mac"
        with self.assertRaises(ValueError):
            recovering_source_from_env(_FakeSource(), dotenv_path=self.NO_DOTENV)


def _watched(host: str = "192.168.1.13", *, reachable_at=None, table: str | None = None):
    """A RecoveringCameraSource wired to a fake camera, for watcher tests."""
    src = _FakeSource(host, reachable_at=reachable_at)
    text = table if table is not None else f"{host} dev wlp6s0 lladdr {CAM} REACHABLE\n"
    return RecoveringCameraSource(src, _locator(text, last=host))


class TestWatcher(unittest.TestCase):
    """The watcher is the piece that makes recovery autonomous.

    It is also the only thing here that touches the camera of its own accord, so
    every guard on it is asserted directly.
    """

    def test_the_default_heartbeat_is_ninety_seconds(self) -> None:
        from camera_ingest.recovery import DEFAULT_WATCH_INTERVAL_S, RecoveryWatcher

        w = RecoveryWatcher(_FakeSource())
        self.assertEqual(w.interval, 90.0)
        self.assertEqual(DEFAULT_WATCH_INTERVAL_S, 90.0)

    def test_a_healthy_camera_is_never_swept(self) -> None:
        src = _FakeSource("192.168.1.13")
        loc = _locator(f"192.168.1.13 dev wlp6s0 lladdr {CAM} REACHABLE\n")
        rec = RecoveringCameraSource(src, loc)
        w = RecoveryWatcher(rec)

        for _ in range(5):
            self.assertTrue(w.check_once())

        self.assertEqual(w.checks, 5)
        self.assertEqual(w.failures_seen, 0)
        self.assertEqual(w.recoveries, 0)
        self.assertEqual(loc.scan_count, 0, "a healthy camera must never be swept")

    def test_a_moved_camera_is_followed_by_the_heartbeat(self) -> None:
        """The camera moved while nobody was watching: the heartbeat brings it back.

        A probe that RAISES is repaired inside the call itself, so the heartbeat
        ends up healthy. What matters is the outcome: the address followed the MAC.
        """
        src = _FakeSource("192.168.1.13", reachable_at="192.168.1.6")
        loc = _locator(f"192.168.1.6 dev wlp6s0 lladdr {CAM} REACHABLE\n")
        rec = RecoveringCameraSource(src, loc)
        w = RecoveryWatcher(rec)

        self.assertTrue(w.check_once(), "the heartbeat recovered it, so it is reachable")
        self.assertEqual(src._host, "192.168.1.6", "the address followed the MAC")
        self.assertEqual(rec.state, "LIVE")
        self.assertEqual(loc.scan_count, 0, "a table hit needs no sweep")
        self.assertTrue(w.check_once(), "and it stays healthy")

    def test_it_keeps_retrying_until_the_camera_answers(self) -> None:
        """The whole point: it does not give up after one attempt.

        Uses the REAL camera's behaviour - an unreachable camera is reported as a
        falsy RESULT, not an exception - which is precisely the case where nothing
        else would ever trigger recovery.
        """
        table = {"text": ""}          # nothing discoverable yet
        src = _ResultSource("192.168.1.13", reachable_at="192.168.1.99")
        loc = CameraLocator(
            mac=CAM, last_host="192.168.1.13",
            neigh_reader=lambda: table["text"],
            sweep=lambda subnet, timeout: None,
        )
        rec = RecoveringCameraSource(src, loc)
        w = RecoveryWatcher(rec)

        # camera gone, and recovery genuinely cannot find it yet
        for i in range(1, 4):
            self.assertFalse(w.check_once(), f"attempt {i} should report down")
        self.assertEqual(w.failures_seen, 3, "it kept trying, heartbeat after heartbeat")
        self.assertEqual(w.recoveries, 0, "nothing repaired because nothing was found")
        self.assertEqual(src._host, "192.168.1.13", "an unverified move is never adopted")

        # it comes back on a new address and becomes discoverable
        table["text"] = f"192.168.1.99 dev wlp6s0 lladdr {CAM} REACHABLE\n"
        self.assertFalse(w.check_once(), "this heartbeat finds it down, then repairs")
        self.assertEqual(w.recoveries, 1, "the retry finally paid off")
        self.assertEqual(src._host, "192.168.1.99")

        # and it is healthy from here on, without any further repair
        self.assertTrue(w.check_once(), "now healthy")
        self.assertTrue(w.check_once(), "still healthy")
        self.assertEqual(w.failures_seen, 4, "no extra failures after recovery")
        self.assertEqual(w.recoveries, 1)

    def test_a_probe_that_reports_failure_as_a_result_still_triggers_recovery(self) -> None:
        """The real camera reports unreachable as a RESULT, not an exception."""

        src = _ResultSource("192.168.1.13", reachable_at="192.168.1.6")
        loc = _locator(f"192.168.1.6 dev wlp6s0 lladdr {CAM} REACHABLE\n")
        rec = RecoveringCameraSource(src, loc)
        w = RecoveryWatcher(rec)

        self.assertFalse(w.check_once())
        self.assertEqual(w.recoveries, 1, "a failure RESULT must still authorise recovery")
        self.assertEqual(src._host, "192.168.1.6")

    def test_start_runs_the_heartbeat_and_stop_ends_it(self) -> None:
        src = _FakeSource("192.168.1.13")
        rec = RecoveringCameraSource(src, _locator(f"192.168.1.13 dev wlp6s0 lladdr {CAM} REACHABLE\n"))
        w = RecoveryWatcher(rec, interval=1.0)   # fast, only for this test

        self.assertFalse(w.running)
        w.start()
        self.assertTrue(w.running)
        deadline = time.monotonic() + 6.0
        while w.checks < 2 and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertGreaterEqual(w.checks, 2, "the thread must actually be beating")

        w.stop()
        self.assertFalse(w.running)
        settled = w.checks
        time.sleep(1.5)
        self.assertEqual(w.checks, settled, "stop() must actually end the heartbeats")

    def test_stop_returns_promptly_rather_than_waiting_out_the_interval(self) -> None:
        w = RecoveryWatcher(_watched(), interval=90.0)
        w.start()
        t0 = time.monotonic()
        w.stop()
        self.assertLess(time.monotonic() - t0, 3.0, "stop() must not block for the interval")

    def test_it_is_used_as_a_context_manager(self) -> None:
        w = RecoveryWatcher(_watched(), interval=30.0)
        with w as started:
            self.assertTrue(started.running)
        self.assertFalse(w.running)


class _Result:
    """Stands in for the real ProbeResult: truthy when the camera answered."""

    def __init__(self, ok: bool, detail: str = "") -> None:
        self.ok = ok
        self.detail = detail

    def __bool__(self) -> bool:
        return self.ok


class _ResultSource(_FakeSource):
    """A camera whose probe REPORTS failure instead of raising, like the real one.

    `JuanHttpSource.probe()` catches its own exceptions and returns a ProbeResult,
    so nothing above it sees a raise. That is the important case: it is exactly
    why an autonomous watcher is needed at all.
    """

    def probe(self, timeout: float = 8.0):
        if self._host != self.reachable_at:
            return _Result(False, f"nothing at {self._host}")
        return _Result(True, "200 OK")


class TestWatchEnrolment(unittest.TestCase):
    """Autonomous polling must be explicitly switched on, in .env, never inferred."""

    NO_DOTENV = "/nonexistent/hg-test-absent.env"

    def setUp(self) -> None:
        import os

        self._saved = {k: os.environ.pop(k, None)
                       for k in ("HG_CAMERA_WATCH", "HG_CAMERA_WATCH_INTERVAL_S")}

    def tearDown(self) -> None:
        import os

        for k, v in self._saved.items():
            if v is not None:
                os.environ[k] = v
            else:
                os.environ.pop(k, None)

    def test_absent_means_no_watching(self) -> None:
        from camera_ingest.recovery import watch_enabled_from_env

        self.assertFalse(watch_enabled_from_env(self.NO_DOTENV), "must not poll by default")

    def test_various_truthy_spellings_arm_it(self) -> None:
        import os

        from camera_ingest.recovery import watch_enabled_from_env

        for value in ("1", "true", "YES", "On", "enabled"):
            os.environ["HG_CAMERA_WATCH"] = value
            self.assertTrue(watch_enabled_from_env(self.NO_DOTENV), value)

    def test_rubbish_does_not_arm_it(self) -> None:
        import os

        from camera_ingest.recovery import watch_enabled_from_env

        for value in ("", "0", "false", "no", "maybe"):
            os.environ["HG_CAMERA_WATCH"] = value
            self.assertFalse(watch_enabled_from_env(self.NO_DOTENV), value)

    def test_interval_defaults_to_ninety_and_honours_an_override(self) -> None:
        import os

        from camera_ingest.recovery import watch_interval_from_env

        self.assertEqual(watch_interval_from_env(self.NO_DOTENV), 90.0)
        os.environ["HG_CAMERA_WATCH_INTERVAL_S"] = "45"
        self.assertEqual(watch_interval_from_env(self.NO_DOTENV), 45.0)
        os.environ["HG_CAMERA_WATCH_INTERVAL_S"] = "nonsense"
        self.assertEqual(watch_interval_from_env(self.NO_DOTENV), 90.0)
        os.environ["HG_CAMERA_WATCH_INTERVAL_S"] = "0.01"
        self.assertEqual(watch_interval_from_env(self.NO_DOTENV), 90.0,
                         "a sub-second heartbeat must not be accepted")


if __name__ == "__main__":
    unittest.main(verbosity=2)
